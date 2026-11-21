// Switching accounts in-process (spec: multiple accounts, The account session → Switching): the
// screens are left first, what is unsent is sent within a budget (Skip ends it), the old session
// stops in a fixed order leaving nothing of it running, `active` is written, the new session opens
// (after the PIN) and its Home shows; a failure offers Try again / Switch back and touches nothing.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "app/account_switch.h"
#include "app/accounts.h"
#include "app/app.h"
#include "app/device_settings.h"
#include "app/plugin_events.h"
#include "app/session.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_groups.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fn_http.h"
#include "open_guard.h"
#include "plugins/host.h"
#include "screens/account_error.h"
#include "screens/plugin_screen.h"
#include "screens/reader.h"
#include "screens/switching.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/config.h"
#include "sync/null_sync.h"
#include "sync/sync_service.h"
#include "test_paths.h"

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Blocks until the request is cancelled (or 20 s pass), then answers as a cancelled transfer.
HttpResponse hangUntilCancelled(const HttpRequest& r) {
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < until && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    HttpResponse out;
    out.curlCode = 42;
    out.curlError = "cancelled";
    return out;
}

// Zotero for two users (11 and 22): each a library of their own with two books, one with a file in
// Zotero storage that downloads slowly when asked to.
struct Zotero {
    std::mutex mu;
    std::map<int64_t, std::unique_ptr<FakeZoteroHub>> hubs;
    std::atomic<bool> slowWrites{false};  // writes (a push) hang until cancelled
    std::atomic<bool> slowFile{false};    // the file hangs, part-way, until cancelled
    std::atomic<int> writesSeen{0};

    Zotero() {
        for (int64_t id : {11, 22}) {
            auto hub = std::make_unique<FakeZoteroHub>();
            hub->userId = id;
            hub->groupAccess = nullptr;
            hub->user.libraryPath = "users/" + std::to_string(id) + "/";
            const std::string u = std::to_string(id);
            hub->user.put(FakeZotero::item("BOOK" + u + "01", "First book of " + u));
            hub->user.put(FakeZotero::item("BOOK" + u + "02", "Second book of " + u));
            hub->user.put(FakeZotero::attachment("SLOW" + u + "01", "BOOK" + u + "01", "5d41402abc4b2a76b9719d911017c592"));
            hubs[id] = std::move(hub);
        }
    }
    HttpResponse send(const HttpRequest& r) {
        if (r.url.rfind("https://files.example.org/", 0) == 0) {
            if (r.outFile.empty()) return {};
            std::ofstream(r.outFile, std::ios::binary) << "hel";  // part of it
            if (slowFile) return hangUntilCancelled(r);
            std::ofstream(r.outFile, std::ios::binary | std::ios::app) << "lo";
            HttpResponse out;
            out.status = 200;
            return out;
        }
        int64_t user = 0;
        for (const auto& [k, v] : r.headers)
            if (k == "Zotero-API-Key") user = std::atoll(v.c_str() + 3);  // "key<id>"
        if (!hubs.count(user)) return {};
        if (r.method != "GET") {
            ++writesSeen;
            if (slowWrites) return hangUntilCancelled(r);
        }
        if (r.url.find("/file") != std::string::npos && r.url.find("/items/SLOW") != std::string::npos) {
            HttpResponse out;
            out.status = 302;
            out.headers["location"] = "https://files.example.org/" + std::to_string(user) + "/file";
            return out;
        }
        std::lock_guard<std::mutex> lock(mu);
        return hubs[user]->send(r);
    }
};

// Stands for an account's Home: which account it was made for.
struct AccountHome final : Screen {
    int64_t userId;
    explicit AccountHome(int64_t id) : userId(id) {}
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
};

struct Kindle {
    fs::path dir;
    Paths paths;
    DeviceSettings device{""};
    Zotero zotero;
    std::atomic<bool> online{false};
    FakePlugins fp;
    std::unique_ptr<PluginHost> host;
    std::unique_ptr<PluginEvents> events;  // main's routing of what plugins ask of the screen
    int itemOpens = 0, setups = 0;
    FakeDisplay display;
    Db emptyDb = Db::open(":memory:");
    Library emptyLib{emptyDb};
    NullSync nullSync;
    FakeKeyboard keyboard;
    std::atomic<int64_t> nowMs{1'000'000};
    App app{display, emptyLib, nullSync, keyboard, [this] { return nowMs.load(); }};
    std::atomic<bool> listening{false};
    std::unique_ptr<OpenGuard> guard;
    std::vector<std::string> crossings;  // opens outside the account whose session it was
    std::string activeRoot;              // the account whose session opens files now
    std::vector<int64_t> opened;         // onOpened, in order
    int leaves = 0;
    std::function<void(int64_t, std::function<void(bool)>)> unlock;
    std::unique_ptr<AccountSwitcher> switcher;

    explicit Kindle(bool withHomePlugin = true) {
        dir = uniqueTempPath("zk-switch");
        fs::remove_all(dir);
        paths.configDir = (dir / "config").string();
        paths.accountsRoot = (dir / "accounts").string();
        paths.pluginsDir = fp.dir();
        paths.deviceSettingsFile = (dir / "device.json").string();
        paths.dbPath = (dir / "zotero.db").string();  // the old single-account places: never touched
        paths.filesDir = (dir / "files").string();
        fs::create_directories(paths.configDir);
        AccountsState st;
        st.active = 11;
        for (int64_t id : {11, 22}) {
            AccountEntry e;
            e.userId = id;
            e.name = id == 11 ? "Pavel" : "Anna";
            e.order = static_cast<int>(st.accounts.size());
            st.accounts.push_back(e);
            Config c;
            c.userId = id;
            c.apiKey = "key" + std::to_string(id);
            REQUIRE(saveCredentials(paths, c));
            fs::create_directories(accountPaths(paths, id).filesDir);
        }
        REQUIRE(writeAccounts(paths.accountsFile(), st));
        // A plugin listening to syncs (its storage is the account's) and drawing a Home body that
        // takes its time.
        fp.add("p", R"JS(
zk.on("syncFinished", async () => { await zk.storage.set("k", "v"); });
zk.screen("home", { title: "Home", render: async () => { const t = Date.now(); while (Date.now() - t < 250) {} return zk.ui.text("home"); } });
)JS");
        // A plugin whose item action, run late, puts things on screen: a screen and an item opened.
        fp.add("q", R"JS(
zk.itemAction({ id: "go", title: "Go", run: async () => {
  zk.ui.open("s");
  try { await zk.library.open({ key: "BOOK1101", library: 0 }); console.log("item opened"); }
  catch (e) { console.log("item refused: " + e.message); }
} });
zk.screen("s", { title: "S", render: () => zk.ui.text("s") });
)JS", {"library:read"});
        host = std::make_unique<PluginHost>(fp.deps());
        if (withHomePlugin) host->setEnabled("p", true);
        host->setEnabled("q", true);
        guard = std::make_unique<OpenGuard>(paths.accountsRoot);

        SwitcherDeps d;
        d.paths = paths;
        d.device = &device;
        d.session.makeHttp = [this] { return std::make_unique<FnHttp>([this](const HttpRequest& r) { return zotero.send(r); }); };
        d.session.wifiUp = [this] { return online.load(); };
        d.credentials = [this](int64_t id) {
            // From here on, opens are checked against this account (the old one is stopped).
            for (const auto& p : guard->outside()) crossings.push_back(p);
            activeRoot = accountPaths(paths, id).root;
            guard->setActive(activeRoot);
            return loadCredentials(paths, id);
        };
        d.unlock = [this](int64_t id, std::function<void(bool)> done) {
            if (unlock) unlock(id, std::move(done));
            else done(true);
        };
        // The app has drawn once, as on the device before any switch: text drawing's process-wide
        // font thread (pango's) is up, so thread counts compare accounts only.
        app.setRoot(0, std::make_unique<SwitchingScreen>([] { return SwitchingScreen::View{"Zotero", "", false}; }, nullptr, nullptr));
        app.start();
        switcher = std::make_unique<AccountSwitcher>(app, d);
        events = std::make_unique<PluginEvents>(host.get());
        events->attach(app);
        events->setRefusing([this] { return switcher->busy(); });
        events->setItemOpener([this](App& a, const ItemRef&, std::optional<std::string>, std::optional<std::string>,
                                     std::function<void(const std::string&)> answer) {
            ++itemOpens;  // as main's would: a reader of the open library over what shows
            a.push(std::make_unique<AccountHome>(0));
            answer("");
        });
        app.setPollHook([this] {
            events->poll(app);
            switcher->tick();
        });
        switcher->onSetup([this] { ++setups; });
        switcher->onLeave([this] { ++leaves; });
        switcher->onOpened([this](AccountSession& s) {
            opened.push_back(s.userId());
            listening = true;
            s.startServices(app, host.get(), listening);
            app.replaceRoot(0, std::make_unique<AccountHome>(s.userId()));
        });
    }
    ~Kindle() {
        app.setPollHook(nullptr);
        switcher.reset();
        events.reset();
        host.reset();
        guard.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    AccountPaths ap(int64_t id) const { return accountPaths(paths, id); }
    AccountSession* session() const { return switcher->session(); }
    // Loop steps until the switch is over (or 20 s pass).
    bool settle() {
        const auto until = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < until) {
            app.step(0);
            if (!switcher->busy()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    void steps(int n, int sleepMs = 2) {
        for (int i = 0; i < n; ++i) {
            app.step(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
    // Online once: the account's library pulled (its items and the attachment record).
    void pull() {
        const bool was = online;
        online = true;
        session()->sync()->syncNow();
        REQUIRE(eventually([&] { return session()->library().attachment("SLOW" + std::to_string(session()->userId()) + "01").has_value(); }, 10000));
        REQUIRE(eventually([&] { return !session()->sync()->busy(); }, 10000));
        steps(3);
        online = was;
    }
    // Two of the account's books marked read on the Kindle: two changes not sent yet.
    void twoUnsent() {
        const std::string u = std::to_string(session()->userId());
        session()->library().markRead("BOOK" + u + "01", true);
        session()->library().markRead("BOOK" + u + "02", true);
        REQUIRE(session()->library().readMarks().size() == 2);
    }
    // The account's unsent changes, read from its library on disk (no session of it open).
    size_t unsentOnDisk(int64_t id) {
        for (const auto& p : guard->outside()) crossings.push_back(p);
        size_t n = 0;
        {
            Db db = Db::open(accountPaths(paths, id).dbPath);  // the test's own look: not a crossing
            Library lib(db);
            n = lib.readMarks().size();
        }
        guard->setActive(activeRoot);
        return n;
    }
    const Screen* top() const { return app.top(); }
    SwitchingScreen* switching() { return dynamic_cast<SwitchingScreen*>(const_cast<Screen*>(app.top())); }
    int64_t homeOf() const {
        auto* h = dynamic_cast<const AccountHome*>(app.top());
        return h ? h->userId : 0;
    }
    std::vector<std::string> allCrossings() {
        auto out = crossings;
        for (const auto& p : guard->outside()) out.push_back(p);
        return out;
    }
};

// A reader of the open account's first book (a fake engine on a real render worker).
std::unique_ptr<ReaderScreen> readerOf(Kindle& k) {
    const std::string u = std::to_string(k.session()->userId());
    auto row = k.app.library().rowFor("BOOK" + u + "01");
    REQUIRE(row);
    return std::make_unique<ReaderScreen>(*row, "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
}

}  // namespace

TEST_CASE("switch: startup opens the active account through the switcher and shows its Home") {
    Kindle k;
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    CHECK_FALSE(k.switcher->busy());  // nothing to leave: done before returning
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 11);
    CHECK(&k.app.library() == &k.session()->library());
    CHECK(k.homeOf() == 11);
    CHECK(k.leaves == 0);
    CHECK(k.allCrossings().empty());
}

TEST_CASE("switch: A to B offline with the reader open, a Home plugin drawing and a download under way — A's changes stay queued, nothing of A is left") {
    Kindle k;
    const size_t idle = threadCount();
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.pull();
    const size_t withA = threadCount();
    k.twoUnsent();
    const AccountPaths a = k.ap(11), b = k.ap(22);
    // A book A has on the Kindle (kept), and one downloading (hanging part-way).
    fs::create_directories(fs::path(a.filesDir) / "KEPT0001");
    std::ofstream(fs::path(a.filesDir) / "KEPT0001" / "book.epub") << "kept";
    k.zotero.slowFile = true;
    k.session()->sync()->openBook("BOOK1101", "SLOW1101");
    REQUIRE(eventually([&] { return fs::exists(fs::path(a.filesDir) / ".SLOW1101.part"); }, 5000));
    // Home's plugin body being drawn, and the reader open over Library.
    k.host->openScreen("p", "home", nullptr, TreeUse::HomeBody);
    k.app.switchTab(1);
    k.app.push(readerOf(k));
    k.steps(5);
    REQUIRE(dynamic_cast<const ReaderScreen*>(k.top()));

    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    // 1. Every screen left: only the Switching screen is there.
    REQUIRE(k.switching());
    CHECK(k.switching()->shown().title == "Switching to Anna…");
    CHECK_FALSE(k.switching()->shown().canSkip);  // offline: nothing is sent
    CHECK(k.leaves == 1);
    CHECK(k.switcher->busy());
    CHECK_FALSE(k.switcher->switchTo(11, AccountSwitcher::Why::User));  // one at a time
    REQUIRE(k.settle());

    REQUIRE(k.session());
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
    CHECK(k.opened == std::vector<int64_t>{11, 22});
    CHECK(readAccounts(k.paths.accountsFile())->active == 22);  // 4.
    // The download was cancelled: its .part went, nothing else of A's files.
    CHECK_FALSE(fs::exists(fs::path(a.filesDir) / ".SLOW1101.part"));
    CHECK(slurp(fs::path(a.filesDir) / "KEPT0001" / "book.epub") == "kept");
    // A's threads, files and library are gone; its two changes are still queued in its library.
    CHECK(openFilesUnder(a.root).empty());
    CHECK(eventually([&] { return threadCount() <= withA; }, 500));
    CHECK(k.unsentOnDisk(11) == 2);
    // B works: it pulls its own library, into its own folder.
    k.pull();
    CHECK(k.session()->library().attachment("SLOW2201"));
    CHECK_FALSE(k.session()->library().attachment("SLOW1101"));
    CHECK(fs::exists(b.dbPath));
    for (const auto& p : k.allCrossings()) MESSAGE("opened outside its account: " << p);
    CHECK(k.allCrossings().empty());
    k.switcher->close();
    // (A render worker is detached: it says it is done a moment before its thread is gone.)
    CHECK(eventually([&] { return threadCount() <= idle; }, 500));
    CHECK(openFilesUnder(b.root).empty());
}

TEST_CASE("switch: online, a push that takes too long ends after 15 s; the switch goes on and the changes stay queued") {
    Kindle k;
    k.online = true;
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.pull();
    k.twoUnsent();
    k.online = true;
    k.zotero.slowWrites = true;
    const int writesBefore = k.zotero.writesSeen;
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    REQUIRE(eventually([&] {
        k.app.step(0);
        return k.zotero.writesSeen > writesBefore;
    }, 5000));  // the push is under way, and hangs
    k.steps(20, 5);
    CHECK(k.switcher->busy());
    REQUIRE(k.switching());
    CHECK(k.switching()->shown().canSkip);
    CHECK(k.switching()->shown().progress.find("Pavel") != std::string::npos);
    k.nowMs += 14'000;
    k.steps(3);
    CHECK(k.switcher->busy());  // within the budget it waits
    k.nowMs += 1'000;
    const auto t0 = Clock::now();
    REQUIRE(k.settle());
    CHECK(Clock::now() - t0 < std::chrono::seconds(3));
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
    CHECK(k.unsentOnDisk(11) == 2);
}

TEST_CASE("switch: Skip ends the push at once; the changes stay queued") {
    Kindle k;
    k.online = true;
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.pull();
    k.twoUnsent();
    k.online = true;
    k.zotero.slowWrites = true;
    const int writesBefore = k.zotero.writesSeen;
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    REQUIRE(eventually([&] {
        k.app.step(0);
        return k.zotero.writesSeen > writesBefore;
    }, 5000));
    k.steps(2);
    REQUIRE(k.switching());
    REQUIRE(k.switching()->shown().canSkip);
    const auto t0 = Clock::now();
    const Rect skip = SwitchingScreen::skipRect();
    k.switching()->onGesture(tapAt(skip.center()), k.app);
    REQUIRE(k.settle());
    CHECK(Clock::now() - t0 < std::chrono::seconds(3));
    CHECK(k.session()->userId() == 22);
    CHECK(k.unsentOnDisk(11) == 2);
}

TEST_CASE("switch: online, what is unsent is sent before the old account stops") {
    Kindle k;
    k.online = true;
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.pull();
    k.twoUnsent();
    k.online = true;
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    REQUIRE(k.settle());
    CHECK(k.session()->userId() == 22);
    CHECK(k.unsentOnDisk(11) == 0);
}

TEST_CASE("switch: a target library that can't be opened shows why; its file is untouched; Switch to the previous one reopens it") {
    Kindle k;
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    const AccountPaths b = k.ap(22);
    std::ofstream(b.dbPath, std::ios::binary) << std::string(4096, 'x');  // not a database
    const std::string bytes = slurp(b.dbPath);
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    REQUIRE(k.settle());
    CHECK(k.session() == nullptr);
    auto* error = dynamic_cast<const AccountErrorScreen*>(k.top());
    REQUIRE(error);
    CHECK(error->reason().find("Anna") != std::string::npos);
    REQUIRE(error->choices().size() == 2);
    CHECK(error->choices()[0].label == "Try again");
    CHECK(error->choices()[1].label == "Switch to Pavel");
    CHECK(slurp(b.dbPath) == bytes);
    CHECK(openFilesUnder(b.root).empty());
    CHECK(readAccounts(k.paths.accountsFile())->active == 11);  // the next start opens Pavel, not the broken one
    // Try again: the same.
    const_cast<AccountErrorScreen*>(error)->onGesture(tapAt(AccountErrorScreen::choiceRect(0).center()), k.app);
    REQUIRE(k.settle());
    error = dynamic_cast<const AccountErrorScreen*>(k.top());
    REQUIRE(error);
    REQUIRE(error->choices().size() == 2);
    CHECK(error->choices()[1].label == "Switch to Pavel");
    // Switch to Pavel: A again, as it was.
    const_cast<AccountErrorScreen*>(error)->onGesture(tapAt(AccountErrorScreen::choiceRect(1).center()), k.app);
    REQUIRE(k.settle());
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 11);
    CHECK(k.homeOf() == 11);
    CHECK(readAccounts(k.paths.accountsFile())->active == 11);
    CHECK(slurp(b.dbPath) == bytes);
}

TEST_CASE("switch: to an account with a PIN, the PIN is asked before anything of the open account is left; cancelled, it stays open") {
    Kindle k;
    {
        auto st = *readAccounts(k.paths.accountsFile());
        st.accounts[1].pin = PinHash{"salt", "hash"};
        REQUIRE(writeAccounts(k.paths.accountsFile(), st));
    }
    std::function<void(bool)> answer;
    int64_t asked = 0;
    k.unlock = [&](int64_t id, std::function<void(bool)> done) {
        asked = id;
        answer = std::move(done);
    };
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    CHECK(asked == 0);  // no PIN: not asked
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    k.steps(5);
    CHECK(asked == 22);
    CHECK(k.switcher->busy());
    REQUIRE(k.session());  // A still open, its Home still there
    CHECK(k.session()->userId() == 11);
    CHECK(k.homeOf() == 11);
    CHECK(k.leaves == 0);
    answer(false);  // the PIN pad's Back
    CHECK_FALSE(k.switcher->busy());
    CHECK(k.session()->userId() == 11);
    CHECK(k.homeOf() == 11);
    // Again, and the PIN is right: now the switch goes as any other, without asking twice.
    asked = 0;
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    CHECK(asked == 22);
    asked = 0;
    answer(true);
    REQUIRE(k.settle());
    CHECK(asked == 0);
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
}

TEST_CASE("switch: stress — 100 switches with sync, a plugin, a download and the reader running leave the thread count where it was, and no account opens another's files") {
    Kindle k;
    k.online = true;
    const size_t idle = threadCount();
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::Startup));
    k.pull();
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::User));
    REQUIRE(k.settle());
    k.pull();
    k.online = true;
    const size_t withOne = threadCount();  // one account's sync worker and the plugin host
    k.zotero.slowFile = true;              // every download hangs part-way until cancelled
    std::vector<double> ms;
    for (int i = 0; i < 100; ++i) {
        const int64_t from = k.session()->userId(), to = from == 11 ? 22 : 11;
        const std::string u = std::to_string(from);
        k.session()->sync()->syncNow();                                // a sync running as the switch begins,
        k.session()->sync()->openBook("BOOK" + u + "01", "SLOW" + u + "01");  // a download under way,
        k.host->dispatch(PluginEvent{"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false});  // a plugin at work,
        k.app.push(readerOf(k));                                       // and the reader open
        k.app.step(0);
        const auto t0 = Clock::now();
        REQUIRE(k.switcher->switchTo(to, AccountSwitcher::Why::User));
        REQUIRE(k.settle());
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        REQUIRE(k.session());
        REQUIRE(k.session()->userId() == to);
        REQUIRE(k.homeOf() == to);
        // (A render worker is detached: it says it is done a moment before its thread is gone.)
        REQUIRE(eventually([&] { return threadCount() <= withOne; }, 1000));
        REQUIRE(openFilesUnder(k.ap(from).root).empty());
    }
    std::sort(ms.begin(), ms.end());
    MESSAGE("100 switches: median " << ms[ms.size() / 2] << " ms, slowest " << ms.back() << " ms");
    for (const auto& p : k.allCrossings()) MESSAGE("opened outside its account: " << p);
    CHECK(k.allCrossings().empty());
    for (int64_t id : {11, 22})
        for (const auto& f : fs::directory_iterator(k.ap(id).filesDir))
            CHECK_MESSAGE(f.path().extension() != ".part", f.path());  // no download left half-way
    k.switcher->close();
    CHECK(eventually([&] { return threadCount() <= idle; }, 1000));
    CHECK(openFilesUnder(k.paths.accountsRoot).empty());
}

TEST_CASE("switch: the app's loop delivers the open account's sync events to the screens") {
    Kindle k(false);
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    struct Listener final : Screen {
        int finished = 0;
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
        void onSyncEvent(const SyncEvent& e, Nav&) override { finished += e.kind == SyncEvent::Kind::SyncFinished; }
    };
    auto l = std::make_unique<Listener>();
    Listener* listener = l.get();
    k.app.push(std::move(l));
    k.online = true;
    k.session()->sync()->syncNow();
    CHECK(eventually([&] {
        k.app.step(0);
        return listener->finished > 0;
    }, 10000));
}

TEST_CASE("switch: while a switch runs, a plugin can put nothing over the Switching screen; the switch ends on time") {
    Kindle k;
    k.online = true;
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    k.pull();
    k.twoUnsent();
    k.online = true;
    k.zotero.slowWrites = true;
    const int writesBefore = k.zotero.writesSeen;
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::User));
    REQUIRE(eventually([&] {
        k.app.step(0);
        return k.zotero.writesSeen > writesBefore;
    }, 5000));  // the push hangs
    // An action of A's plugin ends now: it opens a screen and an item.
    k.host->runAction("q", "itemAction", "go", {{"key", "BOOK1101"}, {"title", "First book of 11"}});
    bool covered = false;
    REQUIRE(eventually([&] {
        k.app.step(0);
        covered = covered || !k.switching();
        return k.fp.log("q").find("item refused: Switching accounts") != std::string::npos;
    }, 5000));
    k.steps(20, 5);  // the screen it opened would have come by now
    for (int i = 0; i < 20; ++i) covered = covered || !k.switching();
    CHECK_FALSE(covered);
    CHECK(k.itemOpens == 0);
    k.nowMs += 15'000;
    const auto t0 = Clock::now();
    REQUIRE(k.settle());
    CHECK(Clock::now() - t0 < std::chrono::seconds(3));
    CHECK(k.session()->userId() == 22);
    CHECK(k.homeOf() == 22);
    CHECK(k.unsentOnDisk(11) == 2);
    CHECK(openFilesUnder(k.ap(11).root).empty());
    // Once it is over, plugins may show things again.
    k.host->runAction("q", "itemAction", "go", {{"key", "BOOK2201"}, {"title", "First book of 22"}});
    CHECK(eventually([&] {
        k.app.step(0);
        return dynamic_cast<const PluginScreen*>(k.top()) != nullptr || k.itemOpens > 0;
    }, 5000));
}

TEST_CASE("switch: at startup, an active library that can't be opened offers every other account; that one becomes active") {
    Kindle k;
    {
        auto st = *readAccounts(k.paths.accountsFile());
        st.active = 22;
        REQUIRE(writeAccounts(k.paths.accountsFile(), st));
    }
    std::ofstream(k.ap(22).dbPath, std::ios::binary) << std::string(4096, 'x');  // not a database
    REQUIRE(k.switcher->switchTo(22, AccountSwitcher::Why::Startup));
    CHECK(k.session() == nullptr);
    auto* error = dynamic_cast<const AccountErrorScreen*>(k.top());
    REQUIRE(error);
    REQUIRE(error->choices().size() == 2);
    CHECK(error->choices()[0].label == "Try again");
    CHECK(error->choices()[1].label == "Switch to Pavel");
    const_cast<AccountErrorScreen*>(error)->onGesture(Gesture{GestureType::Tap, AccountErrorScreen::choiceRect(1).center(),
                                                              AccountErrorScreen::choiceRect(1).center()},
                                                      k.app);
    REQUIRE(k.settle());
    REQUIRE(k.session());
    CHECK(k.session()->userId() == 11);
    CHECK(k.homeOf() == 11);
    CHECK(readAccounts(k.paths.accountsFile())->active == 11);
}

TEST_CASE("switch: at startup, the only account's library can't be opened: Try again or Set up") {
    Kindle k;
    {
        auto st = *readAccounts(k.paths.accountsFile());
        st.accounts.pop_back();  // Pavel only
        REQUIRE(writeAccounts(k.paths.accountsFile(), st));
    }
    std::ofstream(k.ap(11).dbPath, std::ios::binary) << std::string(4096, 'x');
    REQUIRE(k.switcher->switchTo(11, AccountSwitcher::Why::Startup));
    CHECK(k.session() == nullptr);
    auto* error = dynamic_cast<const AccountErrorScreen*>(k.top());
    REQUIRE(error);
    REQUIRE(error->choices().size() == 2);
    CHECK(error->choices()[0].label == "Try again");
    CHECK(error->choices()[1].label == "Set up");
    const Rect r = AccountErrorScreen::choiceRect(1);
    const_cast<AccountErrorScreen*>(error)->onGesture(Gesture{GestureType::Tap, r.center(), r.center()}, k.app);
    CHECK(k.setups == 1);
}
