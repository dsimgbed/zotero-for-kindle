// Isolation between accounts (spec: multiple accounts, Isolation checks; plan check 3): two accounts
// that share a group library have the same item and attachment keys. Syncing, downloading and
// sweeping in one, and removing it, leaves the other's library and files byte for byte as they
// were; and while a session runs, nothing outside its account's folder is opened.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "app/accounts.h"
#include "app/app.h"
#include "app/device_settings.h"
#include "app/session.h"
#include "fake_display.h"
#include "fake_groups.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fn_http.h"
#include "open_guard.h"
#include "plugins/host.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/null_sync.h"
#include "sync/sync_service.h"
#include "test_paths.h"

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kHelloMd5 = "5d41402abc4b2a76b9719d911017c592";  // md5("hello")
constexpr int64_t kGroup = 777;

// Every file under `dir` (relative path -> bytes); the database's -wal/-shm are left out (SQLite's
// own, made and removed as connections come and go).
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) {
            if (it->is_directory()) out[fs::relative(it->path(), dir).string() + "/"] = "";
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name.size() > 4 && (name.ends_with("-wal") || name.ends_with("-shm"))) continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        out[fs::relative(it->path(), dir).string()] = ss.str();
    }
    return out;
}

// One Zotero user with a library of their own and the shared group (the same keys for every user).
struct ZoteroUser {
    FakeZoteroHub hub;
    std::mutex mu;
    explicit ZoteroUser(int64_t userId, const std::string& ownKey) {
        hub.userId = userId;
        hub.user.libraryPath = "users/" + std::to_string(userId) + "/";
        hub.user.put(FakeZotero::item(ownKey, "Own book of " + std::to_string(userId)));
        FakeZotero& g = hub.addGroup(kGroup, "Shared");
        g.put(FakeZotero::item("SHARED01", "The shared book"));
        g.put(FakeZotero::attachment("SHATT001", "SHARED01", kHelloMd5));
    }
    // Zotero storage: the file's place (a redirect), then the file.
    HttpResponse send(const HttpRequest& r) {
        std::lock_guard<std::mutex> lock(mu);
        HttpResponse out;
        if (r.url.find("/items/SHATT001/file") != std::string::npos) {
            out.status = 302;
            out.headers["location"] = "https://files.example.org/SHATT001";
            return out;
        }
        if (r.url.rfind("https://files.example.org/", 0) == 0) {
            std::ofstream(r.outFile, std::ios::binary) << "hello";
            out.status = 200;
            return out;
        }
        return hub.send(r);
    }
};

struct Kindle {
    fs::path dir;
    Paths paths;
    DeviceSettings device{""};
    FakePlugins fp;
    std::unique_ptr<PluginHost> host;
    FakeDisplay display;
    Db emptyDb = Db::open(":memory:");
    Library emptyLib{emptyDb};
    NullSync nullSync;
    FakeKeyboard keyboard;
    App app{display, emptyLib, nullSync, keyboard, [] { return int64_t{0}; }};
    std::atomic<bool> listening{false};

    Kindle() {
        dir = uniqueTempPath("zk-isolation");
        fs::remove_all(dir);
        paths.configDir = (dir / "config").string();
        paths.accountsRoot = (dir / "accounts").string();
        paths.pluginsDir = fp.dir();
        paths.deviceSettingsFile = (dir / "device.json").string();
        // A plugin that writes into its storage and files at every sync: the account's plugin data.
        fp.add("p", R"(zk.on("syncFinished", async () => { await zk.storage.set("k", "v"); await zk.files.write("f.txt", "x"); });)");
        host = std::make_unique<PluginHost>(fp.deps());
        host->setEnabled("p", true);
    }
    ~Kindle() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // One session of `user`: open (its sweep), a sync, the shared file downloaded, then stop.
    void session(int64_t userId, ZoteroUser& user, OpenGuard& guard, bool download = true) {
        guard.setActive(accountPaths(paths, userId).root);
        fs::create_directories(accountPaths(paths, userId).root);  // made when it was added
        AccountEntry e;
        e.userId = userId;
        Config c;
        c.userId = userId;
        c.apiKey = "key";
        SessionDeps deps;
        deps.makeHttp = [&user] { return std::make_unique<FnHttp>([&user](const HttpRequest& r) { return user.send(r); }); };
        deps.wifiUp = [] { return true; };
        std::string why;
        auto s = AccountSession::open(paths, e, c, device, &why, deps);
        REQUIRE_MESSAGE(s, why);
        app.setSession(s.get());
        listening = true;
        s->startServices(app, host.get(), listening);
        REQUIRE(s->sync());
        s->sync()->syncNow();
        waitFor(*s, SyncEvent::Kind::SyncFinished);
        host->dispatch(PluginEvent{"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false});
        REQUIRE(host->waitIdle(3000));
        if (download) {
            s->sync()->openBook("SHARED01", "SHATT001");
            const SyncEvent done = waitFor(*s, SyncEvent::Kind::DownloadFinished);
            CHECK_FALSE(done.error);
        }
        CHECK(s->library().attachment("SHATT001"));
        s->stop();
        app.setSession(nullptr);
        s.reset();
        CHECK(guard.outside().empty());
        for (const auto& p : guard.outside()) MESSAGE("opened outside the account: " << p);
        CHECK_FALSE(guard.opens().empty());
    }

    static SyncEvent waitFor(AccountSession& s, SyncEvent::Kind kind) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        SyncEvent e;
        while (std::chrono::steady_clock::now() < until) {
            while (s.sync()->poll(e))
                if (e.kind == kind) return e;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("no sync event of kind " << static_cast<int>(kind));
        return e;
    }
};

}  // namespace

TEST_CASE("isolation: two accounts sharing a group — A's sync, downloads, sweep and removal leave B's library and files as they were") {
    Kindle k;
    ZoteroUser a(11, "OWNA0001"), b(22, "OWNB0001");
    OpenGuard guard(k.paths.accountsRoot);
    k.session(11, a, guard);
    k.session(22, b, guard);
    const AccountPaths ap = accountPaths(k.paths, 11), bp = accountPaths(k.paths, 22);
    // Both have the shared file in their own files folder, and their own plugin data.
    CHECK(fs::exists(fs::path(ap.filesDir) / "SHATT001" / "book.epub"));
    CHECK(fs::exists(fs::path(bp.filesDir) / "SHATT001" / "book.epub"));
    CHECK(fs::exists(bp.pluginStorage("p")));
    CHECK(fs::exists(bp.pluginData("p") + "/f.txt"));
    const auto before = tree(bp.root);
    REQUIRE(before.count("zotero.db"));
    // A again: its files hold a leftover the sweep takes, named like B's file.
    fs::create_directories(fs::path(ap.filesDir) / "SHATT002");
    std::ofstream(fs::path(ap.filesDir) / ".SHATT001.part") << "half";
    a.hub.group(kGroup).put(FakeZotero::item("SHARED02", "Added in the group since"));  // A pulls it, B doesn't
    k.session(11, a, guard, false);
    CHECK_FALSE(fs::exists(fs::path(ap.filesDir) / "SHATT002"));
    CHECK_FALSE(fs::exists(fs::path(ap.filesDir) / ".SHATT001.part"));
    CHECK(tree(bp.root) == before);
    // A removed (its folder and credentials go).
    fs::remove_all(ap.root);
    CHECK(tree(bp.root) == before);
    // And B opens as it was: the shared attachment and its file are there.
    guard.setActive(bp.root);
    std::string why;
    AccountEntry e;
    e.userId = 22;
    Config c;
    c.userId = 22;
    c.apiKey = "key";
    SessionDeps deps;
    deps.makeHttp = [] { return std::make_unique<FnHttp>([](const HttpRequest&) { return HttpResponse{}; }); };
    deps.wifiUp = [] { return false; };
    auto s = AccountSession::open(k.paths, e, c, k.device, &why, deps);
    REQUIRE(s);
    CHECK(s->library().attachment("SHATT001"));
    CHECK(fs::exists(fs::path(bp.filesDir) / "SHATT001" / "book.epub"));
    s->stop();
    CHECK(guard.outside().empty());
}
