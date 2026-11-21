// The account session (spec: multiple accounts, The account session): it opens one account's
// library, sweeps that account's files, runs its sync and binds the plugin host to it; stop()
// leaves no thread, open file or cache of it behind.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "app/accounts.h"
#include "app/app.h"
#include "app/device_settings.h"
#include "app/home_tree.h"
#include "app/session.h"
#include "app/startup.h"
#include "fake_display.h"
#include "fake_groups.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fn_http.h"
#include "open_guard.h"
#include "plugins/host.h"
#include "screens/account_error.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/null_sync.h"
#include "test_paths.h"

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A device with an accounts folder in a fresh temp dir; accounts are made in it with account().
struct Device {
    fs::path dir;
    Paths paths;
    DeviceSettings device{""};
    Device() {
        static int n = 0;
        dir = uniqueTempPath("zk-session-" + std::to_string(++n));
        fs::remove_all(dir);
        fs::create_directories(dir);
        paths.configDir = (dir / "config").string();
        paths.accountsRoot = (dir / "accounts").string();
        paths.pluginsDir = (dir / "plugins").string();
        paths.deviceSettingsFile = (dir / "device.json").string();
        // The old single-account places: never touched by a session.
        paths.dbPath = (dir / "zotero.db").string();
        paths.filesDir = (dir / "files").string();
        paths.cacheDir = (dir / "cache").string();
        paths.stateDir = (dir / "state").string();
        fs::create_directories(paths.pluginsDir);
    }
    ~Device() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    AccountEntry entry(int64_t userId) const {
        AccountEntry e;
        e.userId = userId;
        e.name = "user" + std::to_string(userId);
        return e;
    }
    Config config(int64_t userId) const {
        Config c;
        c.userId = userId;
        c.apiKey = "key" + std::to_string(userId);
        return c;
    }
    // The account's folder with a library that was pulled once (version 5) holding one attachment,
    // its file downloaded.
    AccountPaths account(int64_t userId, const std::string& attachmentKey = "ATTACH01") const {
        const AccountPaths a = accountPaths(paths, userId);
        fs::create_directories(a.filesDir);
        {
            Db db = Db::open(a.dbPath);
            Library lib(db);
            lib.setLibraryVersion(5);
            AttachmentRecord att;
            att.key = attachmentKey;
            att.contentType = "application/epub+zip";
            att.filename = "book.epub";
            lib.upsertAttachment(att);
        }
        fs::create_directories(fs::path(a.filesDir) / attachmentKey);
        std::ofstream(fs::path(a.filesDir) / attachmentKey / "book.epub") << "the book";
        return a;
    }
    SessionDeps deps() const {
        SessionDeps d;
        d.makeHttp = [] { return std::make_unique<FnHttp>([](const HttpRequest&) { return HttpResponse{}; }); };
        d.wifiUp = [] { return false; };
        return d;
    }
};

struct Screen0 {
    FakeDisplay display;
    Db db = Db::open(":memory:");
    Library lib{db};
    NullSync sync;
    FakeKeyboard keyboard;
    App app{display, lib, sync, keyboard, [] { return int64_t{0}; }};
};

}  // namespace

TEST_CASE("session: open, startServices and stop leave no thread, open file or database of the account") {
    Device d;
    const AccountPaths a = d.account(11);
    FakePlugins fp;
    PluginHost host(fp.deps());
    Screen0 s;
    std::atomic<bool> listening{false};
    const size_t before = threadCount();
    REQUIRE(before > 0);
    REQUIRE(openFilesUnder(a.root).empty());
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
    REQUIRE_MESSAGE(session, why);
    CHECK(session->userId() == 11);
    CHECK(session->paths().root == a.root);
    CHECK_FALSE(openFilesUnder(a.root).empty());  // its library is open
    session->startServices(s.app, &host, listening);
    REQUIRE(session->sync());
    CHECK(threadCount() > before);
    session->stop();
    session->stop();  // idempotent
    CHECK(session->sync() == nullptr);
    CHECK(threadCount() <= before);
    CHECK(openFilesUnder(a.root).empty());
    session.reset();  // the destructor after stop(): nothing more
    CHECK(openFilesUnder(a.root).empty());
}

TEST_CASE("session: the destructor stops what runs") {
    Device d;
    const AccountPaths a = d.account(11);
    FakePlugins fp;
    PluginHost host(fp.deps());
    Screen0 s;
    std::atomic<bool> listening{false};
    const size_t before = threadCount();
    {
        std::string why;
        auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
        REQUIRE(session);
        session->startServices(s.app, &host, listening);
    }
    CHECK(threadCount() <= before);
    CHECK(openFilesUnder(a.root).empty());
}

TEST_CASE("session: open sweeps the account's own files only") {
    Device d;
    const AccountPaths a = d.account(11, "ATTACH01");
    const AccountPaths b = d.account(22, "ATTACH02");
    // A's: a folder of an attachment gone from its library, and a download cut short.
    fs::create_directories(fs::path(a.filesDir) / "GONE0001");
    std::ofstream(fs::path(a.filesDir) / ".PART0001.part") << "half";
    // B's look the same to A's library (none of B's keys are A's).
    fs::create_directories(fs::path(b.filesDir) / "GONE0002");
    std::ofstream(fs::path(b.filesDir) / ".PART0002.part") << "half";
    OpenGuard guard(d.paths.accountsRoot);
    guard.setActive(a.root);
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
    REQUIRE(session);
    CHECK_FALSE(fs::exists(fs::path(a.filesDir) / "GONE0001"));
    CHECK_FALSE(fs::exists(fs::path(a.filesDir) / ".PART0001.part"));
    CHECK(fs::exists(fs::path(a.filesDir) / "ATTACH01" / "book.epub"));
    CHECK(fs::exists(fs::path(b.filesDir) / "GONE0002"));
    CHECK(fs::exists(fs::path(b.filesDir) / ".PART0002.part"));
    CHECK(fs::exists(fs::path(b.filesDir) / "ATTACH02" / "book.epub"));
    session->stop();
    CHECK_FALSE(guard.opens().empty());  // the guard saw the session's opens...
    CHECK(guard.outside().empty());       // ...all in A's folder
}

TEST_CASE("session: a library never pulled isn't swept") {
    Device d;
    const AccountPaths a = accountPaths(d.paths, 11);
    fs::create_directories(fs::path(a.filesDir) / "KEEP0001");
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
    REQUIRE(session);
    CHECK(fs::exists(fs::path(a.filesDir) / "KEEP0001"));
}

TEST_CASE("session: a database that can't be opened is refused with the reason; its files are left as they were") {
    Device d;
    const AccountPaths a = accountPaths(d.paths, 11);
    fs::create_directories(a.root);
    std::ofstream(a.dbPath, std::ios::binary) << std::string(4096, 'x');  // not a database
    fs::create_directories(fs::path(a.filesDir) / "GONE0001");
    const std::string bytes = slurp(a.dbPath);
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
    CHECK_FALSE(session);
    CHECK_FALSE(why.empty());
    CHECK(slurp(a.dbPath) == bytes);
    CHECK(fs::exists(fs::path(a.filesDir) / "GONE0001"));
    CHECK(openFilesUnder(a.root).empty());
}

TEST_CASE("session: a key of another user is refused") {
    Device d;
    d.account(11);
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(22), d.device, &why, d.deps());
    CHECK_FALSE(session);
    CHECK_FALSE(why.empty());
}

TEST_CASE("session: open moves device keys left in the account's library to device.json, and leaves account keys") {
    Device d;
    const AccountPaths a = d.account(11);
    {
        Db db = Db::open(a.dbPath);
        Library lib(db);
        lib.setMetaValue("settings", R"({"flashEvery":9,"homeScreen":"plug/main"})");
    }
    DeviceSettings device(d.paths.deviceSettingsFile);
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), device, &why, d.deps());
    REQUIRE(session);
    CHECK(device.get("flashEvery") == std::optional<std::string>("9"));
    const std::string left = session->library().metaValue("settings");
    CHECK(left.find("flashEvery") == std::string::npos);
    CHECK(left.find("homeScreen") != std::string::npos);
}

TEST_CASE("session: the caches, Home's kept tree, plugin data and the crengine cache are the account's") {
    Device d;
    d.account(11);
    d.account(22);
    std::string why;
    auto a = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
    REQUIRE(a);
    const AccountPaths ap = a->paths();
    CHECK(isUnder(a->previews().dir(), ap.root));
    const PluginAccount pa = a->pluginAccount();
    CHECK(pa.dbPath == ap.dbPath);
    CHECK(pa.storageRoot == ap.pluginDataRoot);
    CHECK(pa.fullTextDir == ap.fullTextDir);
    EngineEnv device{"data", {"fonts"}, ""};
    CHECK(a->engineEnv(device).cacheDir == ap.creDir);
    CHECK(a->engineEnv(device).dataDir == "data");
    a->homeTree().save("plug", "main", nlohmann::json{{"type", "column"}});
    CHECK(a->homeTree().load("plug", "main"));
    a->stop();
    a.reset();
    auto b = AccountSession::open(d.paths, d.entry(22), d.config(22), d.device, &why, d.deps());
    REQUIRE(b);
    CHECK_FALSE(b->homeTree().load("plug", "main"));  // A's Home isn't B's
    CHECK(fs::exists(ap.stateDir + "/home-tree.json"));
}

TEST_CASE("session: the developer loop keeps plugin data in the plugins' folders") {
    Device d;
    d.account(11);
    SessionDeps deps = d.deps();
    deps.pluginDataInPluginsDir = true;
    std::string why;
    auto s = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, deps);
    REQUIRE(s);
    CHECK(s->pluginAccount().dbPath == s->paths().dbPath);
    CHECK(s->pluginAccount().storageRoot.empty());
    CHECK(s->pluginAccount().fullTextDir.empty());
}

TEST_CASE("session: App reaches the library and the sync through the session it is given") {
    Device d;
    d.account(11);
    FakePlugins fp;
    PluginHost host(fp.deps());
    Screen0 s;
    std::atomic<bool> listening{false};
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
    REQUIRE(session);
    CHECK(&s.app.library() == &s.lib);  // no session: the app's own (empty) library
    s.app.setSession(session.get());
    CHECK(&s.app.library() == &session->library());
    CHECK(&s.app.sync() == &s.sync);  // services not started: no sync yet
    session->startServices(s.app, &host, listening);
    CHECK(&s.app.sync() == static_cast<SyncControl*>(session->sync()));
    session->stopServices();
    CHECK(&s.app.sync() == &s.sync);
    CHECK(&s.app.library() == &session->library());  // the library stays open
    s.app.setSession(nullptr);
    CHECK(&s.app.library() == &s.lib);
}

TEST_CASE("session: the plugin host runs for the account while its services do") {
    Device d;
    d.account(11);
    FakePlugins fp;
    fp.add("a", R"(zk.on("syncFinished", async () => { await zk.storage.set("k", "v"); });)");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    Screen0 s;
    std::atomic<bool> listening{false};
    std::string why;
    auto session = AccountSession::open(d.paths, d.entry(11), d.config(11), d.device, &why, d.deps());
    REQUIRE(session);
    session->startServices(s.app, &host, listening);
    const PluginEvent e{"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false};
    host.dispatch(e);
    REQUIRE(host.waitIdle(3000));
    CHECK(fs::exists(session->paths().pluginStorage("a")));
    session->stop();
    host.dispatch(e);  // stopped with the session: nothing runs
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(host.waitIdle(100));
}

TEST_CASE("session: a library that can't be opened at startup shows why, and Close Zotero quits") {
    AccountErrorScreen screen("Can't open pavel's library", "can't open the library: file is not a database");
    CHECK_FALSE(screen.showsTabBar());
    Canvas c(metrics().width, metrics().height);
    screen.draw(c);
    Db db = Db::open(":memory:");
    Library lib(db);
    FakeNav nav(lib);
    screen.onGesture(Gesture{GestureType::Tap, {10, 10}, {10, 10}}, nav);
    CHECK_FALSE(nav.quitted);
    const Rect r = AccountErrorScreen::closeRect();
    screen.onGesture(Gesture{GestureType::Tap, r.center(), r.center()}, nav);
    CHECK(nav.quitted);
}

TEST_CASE("session: the first account (Setup saved its config.json on a new install) moves into the layout and opens") {
    Device d;
    fs::create_directories(d.paths.configDir);
    REQUIRE(saveConfig(d.paths.configDir, d.config(11)));
    Paths p = d.paths;
    std::string why;
    REQUIRE(openAccountLayout(p, &why) == std::optional<int64_t>(11));
    CHECK_FALSE(fs::exists(p.configFile()));
    CHECK(loadCredentials(p, 11));
    const auto accounts = readAccounts(p.accountsFile());
    REQUIRE(accounts);
    CHECK(accounts->active == 11);
    CHECK_FALSE(fs::exists(d.paths.dbPath));  // nothing made in the old places
    auto s = AccountSession::open(p, d.entry(11), *loadCredentials(p, 11), d.device, &why, d.deps());
    REQUIRE_MESSAGE(s, why);
    CHECK(fs::exists(accountPaths(p, 11).dbPath));
    CHECK(fs::is_directory(accountPaths(p, 11).filesDir));
}
