// Home's body written by a plugin, end to end (src/app/home_tree.h): PluginHome over PluginEvents
// and a real plugin host, HomeScreen as the app's root. The kept tree (HomeTreeStore) is drawn in
// the first frame, before the host even starts; plugins off, safe mode, a failing render, a
// search on Home and a rebind all leave the built-in body.

#include <doctest/doctest.h>

#include <filesystem>

#include "app/app.h"
#include "app/home_tree.h"
#include "app/plugin_events.h"
#include "fake_display.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fake_sync.h"
#include "fixture_library.h"
#include "screens/home.h"
#include "screens/item_source.h"
#include "screens/plugin_body.h"
#include "store/db.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

const char* kDesk = R"JS(
let mode = "fresh";
zk.screen("desk", {
  title: "Desk", home: true,
  render: () => {
    if (mode === "throw") throw new Error("broken");
    if (mode === "search") return zk.ui.column([zk.ui.search({ id: "s" })]);
    return zk.ui.column([zk.ui.text(mode), zk.ui.button({ id: "throw", title: "Break" }), zk.ui.button({ id: "search", title: "Search" })]);
  },
  onEvent(e) { if (e.kind === "tap") mode = e.id; },
});
)JS";

struct Blank : Screen {
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
};

json textTree(const std::string& text) {
    return json{{"kind", "column"}, {"children", json::array({json{{"kind", "text"}, {"text", text}}})}};
}

struct HomeRig {
    FakePlugins fp;
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [this] { return now; }};
    std::unique_ptr<PluginHost> host;
    std::unique_ptr<PluginEvents> events;
    LibraryItemSource items{lib, nullptr, [] { return int64_t{0}; }};
    HomeTreeStore store{fp.path("state/home-tree.json")};
    std::unique_ptr<PluginHome> link;
    SyncStatus status;
    std::string choice = "desk/desk";

    explicit HomeRig(std::optional<json> kept = std::nullopt) {
        fillFixtureLibrary(lib, 10);
        fp.add("desk", kDesk, {}, "Reading Desk");
        if (kept) store.save("desk", "desk", *kept);
        PluginHostDeps d = fp.deps();
        d.renderBudgetMs = 500;
        d.actionBudgetMs = 1000;
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("desk", true);
        events = std::make_unique<PluginEvents>(host.get());
        link = std::make_unique<PluginHome>(*events, store, [this] { return choice; }, &items, ItemTap{});
        app.setPollHook([this] { events->poll(app); });
        HomeActions a;
        a.body = [this]() -> HomeBodyLink* { return link.get(); };
        for (int i = 1; i < App::kTabs; ++i) app.setRoot(i, std::make_unique<Blank>());
        app.setRoot(0, std::make_unique<HomeScreen>(status, a, [] { return int64_t{0}; }));
        app.switchTab(0);
    }
    ~HomeRig() {
        host->stop();
        app.closeAll();
    }
    void start() {
        app.start();
        host->start();  // after the first screen, as main does
    }
    HomeScreen& home() {
        auto* h = dynamic_cast<const HomeScreen*>(app.top());
        REQUIRE(h);
        return *const_cast<HomeScreen*>(h);
    }
    void showAgain() {  // another tab, and back to Home
        app.switchTab(1);
        app.step(0);
        app.switchTab(0);
        app.step(0);
    }
    std::string shownText() {
        const PluginBody* b = home().pluginBody();
        if (!home().showsPluginBody() || !b->laid()) return "";
        return plainText(b->laid()->block->children.at(0));
    }
    bool until(const std::function<bool()>& cond) {
        return eventually([&] {
            app.step(0);
            now += 70;
            return cond();
        });
    }
    void tap(const std::string& id) {
        const Laid* b = findLaid(*home().pluginBody()->laid(), id);
        REQUIRE(b);
        display.tap({b->rect.x + b->rect.w / 2, b->rect.y + b->rect.h / 2}, now);
        app.step(0);
    }
};

}  // namespace

TEST_CASE("home tree: the store keeps one screen's tree; another screen's or a broken file is none") {
    FakePlugins fp;
    HomeTreeStore store(fp.path("state/home-tree.json"));
    CHECK_FALSE(store.load("desk", "desk"));
    store.save("desk", "desk", textTree("a"));
    REQUIRE(store.load("desk", "desk"));
    CHECK(*store.load("desk", "desk") == textTree("a"));
    CHECK_FALSE(store.load("desk", "other"));
    CHECK_FALSE(store.load("other", "desk"));
    fp.write("state/home-tree.json", "{not json");
    CHECK_FALSE(store.load("desk", "desk"));
}

TEST_CASE("home tree: cold start draws the kept tree in the first frame; the fresh one replaces it and is kept") {
    HomeRig r(textTree("kept"));
    r.app.start();  // the host hasn't started: no plugin code has run
    CHECK(r.shownText() == "kept");
    r.host->start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    REQUIRE(r.store.load("desk", "desk"));
    CHECK((*r.store.load("desk", "desk"))["children"][0]["text"] == "fresh");
}

TEST_CASE("home tree: no kept tree: the built-in body until the first tree; then it shows and is kept") {
    HomeRig r;
    r.app.start();
    CHECK_FALSE(r.home().showsPluginBody());
    CHECK(r.home().pluginBody());  // waiting for it
    r.host->start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    CHECK(r.store.load("desk", "desk"));
}

TEST_CASE("home tree: a render that throws while shown → built-in body; the failure counts; the next show tries again") {
    HomeRig r;
    r.start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    r.tap("throw");
    REQUIRE(r.until([&] { return !r.home().pluginBody(); }));
    CHECK_FALSE(r.home().showsPluginBody());
    CHECK(statusOf(r.host->status(), "desk")->failures == 1);
    CHECK((*r.store.load("desk", "desk"))["children"][0]["text"] == "fresh");  // a failure isn't kept
    r.showAgain();
    CHECK(r.home().pluginBody());  // tried again
}

TEST_CASE("home tree: a search on Home is an invalid tree for the host: a failure, the built-in body") {
    HomeRig r;
    r.start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    r.tap("search");
    REQUIRE(r.until([&] { return !r.home().pluginBody(); }));
    CHECK(statusOf(r.host->status(), "desk")->failures == 1);
}

TEST_CASE("home tree: the plugin turned off while Home shows → built-in body; Home doesn't flash the kept tree after") {
    HomeRig r(textTree("kept"));
    r.start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    r.host->setEnabled("desk", false);
    REQUIRE(r.until([&] { return !r.home().pluginBody(); }));
    r.showAgain();  // the plugin is off, so nothing opens
    CHECK_FALSE(r.home().pluginBody());
    CHECK_FALSE(r.home().showsPluginBody());
}

TEST_CASE("home tree: safe mode (plugins not running) or Built-in chosen: no body, not even the kept tree") {
    {
        HomeRig r(textTree("kept"));
        r.events->setRunning(false);
        r.app.start();
        CHECK_FALSE(r.home().pluginBody());
    }
    {
        HomeRig r(textTree("kept"));
        r.choice = "";
        r.app.start();
        CHECK_FALSE(r.home().pluginBody());
    }
}

TEST_CASE("home tree: plugins stopped for an install (rebind) → built-in body") {
    HomeRig r;
    r.start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    r.events->setRunning(false);
    r.events->rebind(nullptr, r.app);
    REQUIRE(r.until([&] { return !r.home().pluginBody(); }));
}

TEST_CASE("home tree: Settings → Home changed: the next show drops the old body and shows the built-in one") {
    HomeRig r;
    r.start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    r.choice = "";
    r.showAgain();
    CHECK_FALSE(r.home().pluginBody());
}
