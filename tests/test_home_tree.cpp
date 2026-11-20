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
let mode = "fresh", n = 0;
zk.screen("desk", {
  title: "Desk", home: true,
  render: () => {
    if (mode === "throw") throw new Error("broken");
    if (mode === "search") return zk.ui.column([zk.ui.search({ id: "s" })]);
    return zk.ui.column([zk.ui.text(mode), zk.ui.button({ id: "throw", title: "Break" }), zk.ui.button({ id: "search", title: "Search" }),
                         zk.ui.button({ id: "again", title: "Again" })]);
  },
  onEvent(e) { if (e.kind === "tap") mode = e.id === "again" ? `again${++n}` : e.id; },
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

    explicit HomeRig(std::optional<json> kept = std::nullopt, const char* script = kDesk) {
        fillFixtureLibrary(lib, 10);
        fp.add("desk", script, {}, "Reading Desk");
        if (kept) store.save("desk", "desk", *kept);
        PluginHostDeps d = fp.deps();
        d.renderBudgetMs = 500;
        d.actionBudgetMs = 1000;
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("desk", true);
        events = std::make_unique<PluginEvents>(host.get());
        link = std::make_unique<PluginHome>(*events, store, [this] { return choice; }, &items, ItemTap{}, [this] { return now; });
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

TEST_CASE("home tree: a hidden body keeps only its newest tree waiting; errors are all kept (R-D)") {
    HomeRig r;
    r.choice = "";  // Home opens no body itself: this one is never shown, as a body under the reader
    r.start();
    const PluginEvents::Opened o = r.events->openHome("desk", "desk");
    REQUIRE(o.instance != 0);
    for (int i = 0; i < 20; ++i) {
        r.events->rerender(o.instance);
        REQUIRE(r.host->waitIdle(2000));
        r.events->poll(r.app);
    }
    int trees = 0, errors = 0;
    PluginHostEvent ev;
    while (r.events->take(o.instance, ev)) {
        if (ev.kind == PluginHostEvent::Kind::ScreenTree) ++trees;
        else ++errors;
    }
    CHECK(trees == 1);
    CHECK(errors == 0);

    // An error waiting stays; a newer tree after it replaces only the older trees.
    r.events->rerender(o.instance);
    REQUIRE(r.host->waitIdle(2000));
    r.events->poll(r.app);
    r.events->send(o.instance, json{{"kind", "tap"}, {"id", "throw"}});  // render now throws: a ScreenError
    REQUIRE(r.host->waitIdle(2000));
    r.events->poll(r.app);
    std::vector<PluginHostEvent::Kind> kinds;
    while (r.events->take(o.instance, ev)) kinds.push_back(ev.kind);
    REQUIRE(kinds.size() == 2);
    CHECK(kinds[0] == PluginHostEvent::Kind::ScreenTree);
    CHECK(kinds[1] == PluginHostEvent::Kind::ScreenError);
    r.events->left(o.instance);
}

TEST_CASE("home tree: the kept tree is written at most every 30 s, and when Home hides") {
    HomeRig r;
    r.start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    auto kept = [&] { return (*r.store.load("desk", "desk"))["children"][0]["text"].get<std::string>(); };
    CHECK(kept() == "fresh");  // the first one at once
    r.tap("again");
    REQUIRE(r.until([&] { return r.shownText() == "again1"; }));
    CHECK(kept() == "fresh");  // within 30 s of the last write: waits
    r.app.switchTab(1);        // Home hides: what waits is written
    r.app.step(0);
    CHECK(kept() == "again1");
    r.app.switchTab(0);
    r.app.step(0);
    r.now += PluginHome::kKeepEveryMs;
    r.tap("again");
    REQUIRE(r.until([&] { return r.shownText() == "again2"; }));
    CHECK(kept() == "again2");  // 30 s on: at once
}

TEST_CASE("home tree: the chosen screen gone from its plugin: the kept tree goes too") {
    HomeRig r(textTree("kept"));
    r.store.save("desk", "gone", textTree("stale"));
    r.choice = "desk/gone";
    r.app.start();
    CHECK(r.shownText() == "stale");  // drawn before plugins load
    r.host->start();
    REQUIRE(r.until([&] { return !r.home().pluginBody(); }));
    CHECK_FALSE(r.store.load("desk", "gone"));
    CHECK_FALSE(r.fp.exists("state/home-tree.json"));
}

TEST_CASE("home tree: a tap on a text or a box with an id brings {tap, id} to the Home body, as a quote's does") {
    HomeRig r(std::nullopt, R"JS(
let said = "fresh";
zk.screen("desk", {
  title: "Desk", home: true,
  render: () => zk.ui.column([zk.ui.text(said), zk.ui.text("All ›", { id: "all-inbox" }),
                              zk.ui.box(zk.ui.text("Reading"), { id: "reading", border: "thin" })]),
  onEvent(e) { if (e.kind === "tap") said = "tapped " + e.id; },
});
)JS");
    r.start();
    REQUIRE(r.until([&] { return r.shownText() == "fresh"; }));
    r.tap("all-inbox");
    REQUIRE(r.until([&] { return r.shownText() == "tapped all-inbox"; }));
    r.tap("reading");
    REQUIRE(r.until([&] { return r.shownText() == "tapped reading"; }));
}

TEST_CASE("home tree: an item piece opens the item sheet on a long-press; an image comes from the plugin's package") {
    HomeRig r(std::nullopt, R"JS(
zk.screen("desk", {
  title: "Desk", home: true,
  render: () => zk.ui.column([
    zk.ui.item("IT000009", zk.ui.row([zk.ui.cover("IT000009", { width: "20%" }), zk.ui.text("Pinned one")])),
    zk.ui.image({ src: "art/pic.png", id: "pic", width: "25%" }),
  ]),
});
)JS");
    {  // a 40×20 PNG in the plugin's folder
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 40, 20);
        std::filesystem::create_directories(r.fp.path("desk/art"));
        REQUIRE(cairo_surface_write_to_png(s, r.fp.path("desk/art/pic.png").c_str()) == CAIRO_STATUS_SUCCESS);
        cairo_surface_destroy(s);
    }
    r.start();
    REQUIRE(r.until([&] {
        const PluginBody* b = r.home().pluginBody();
        return r.home().showsPluginBody() && b->laid() && findLaid(*b->laid(), "pic");
    }));
    const Laid& laid = *r.home().pluginBody()->laid();
    const Rect pic = findLaid(laid, "pic")->rect;
    CHECK(pic.w > 0);
    CHECK(pic.h == pic.w / 2);
    const Rect piece = laid.children.at(0).rect;
    const Point at = piece.center();
    r.home().onGesture(Gesture{GestureType::LongPress, at, at}, r.app);
    REQUIRE(r.home().pluginBody()->sheet());
    CHECK(r.home().pluginBody()->sheet()->row().key == "IT000009");
}
