#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/home.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kNowWall = 1'762'500'000'000;  // fixed wall clock for "opened yesterday"

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    SyncStatus status;
    std::vector<std::string> opened;
    int searches = 0, settings = 0;
    HomeScreen home{status, HomeActions{[this] { ++searches; }, [this] { ++settings; },
                                        [this](const ItemRow& r) { opened.push_back(r.key); }, nullptr},
                    [] { return kNowWall; }};
    Canvas canvas{1236, 1547};
    explicit Rig(int items = 40) {
        if (items) fillFixtureLibrary(lib, items);
        home.onShow(nav);
    }
    void draw() { home.draw(canvas); }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

}  // namespace

TEST_CASE("sync status: lines for progress, recent sync and errors") {
    SyncStatus s;
    CHECK(s.line(0).empty());
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncProgress;
    e.progress = {"items", 1200, 6560};
    s.onEvent(e, 0);
    CHECK(s.syncing());
    CHECK(s.line(0) == "Syncing… 1,200 of 6,560");
    e.kind = SyncEvent::Kind::SyncFinished;
    s.onEvent(e, 10'000);
    CHECK_FALSE(s.syncing());
    CHECK(s.line(10'000 + 30'000) == "Synced just now");
    CHECK(s.line(10'000 + 5 * 60'000) == "Synced 5 min ago");
    CHECK(s.msSinceSync(10'000 + 1000) == 1000);
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "curl 6: x"};
    s.onEvent(e, 20'000);
    CHECK(s.hasError());
    e.error.reset();
    s.onEvent(e, 30'000);
    CHECK_FALSE(s.hasError());
}

TEST_CASE("home: the real library matches the reviewed golden") {
    if (!fonts()) return;
    Rig rig;
    rig.lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
    rig.home.onShow(rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-real.png");
}

TEST_CASE("home: an empty library during the first sync says so") {
    if (!fonts()) return;
    Rig rig(0);
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncProgress;
    e.progress = {"items", 1200, 6560};
    rig.home.onSyncEvent(e, rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-empty.png");
}

TEST_CASE("home: a sync error shows a banner with details") {
    if (!fonts()) return;
    Rig rig;
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.",
                        "curl 6: Could not resolve host: api.zotero.org"};
    rig.home.onSyncEvent(e, rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-error.png");
}

TEST_CASE("home: taps go to sync, search, settings and items") {
    Rig rig;
    rig.draw();
    const int before = rig.nav.syncFake.syncs;  // showing Home already asked for one
    rig.home.onGesture(tapAt(HomeScreen::syncIndicatorRect().center()), rig.nav);
    CHECK(rig.nav.syncFake.syncs == before + 1);
    rig.home.onGesture(tapAt(HomeScreen::searchFieldRect().center()), rig.nav);
    CHECK(rig.searches == 1);
    rig.home.onGesture(tapAt(HomeScreen::settingsRect().center()), rig.nav);
    CHECK(rig.settings == 1);
    auto rows = rig.home.recentRowRects();
    REQUIRE(rows.size() == 3);  // what fits under the card at mockup C's sizes
    rig.home.onGesture(tapAt(rows[0].center()), rig.nav);
    REQUIRE(rig.opened.size() == 1);
    CHECK(rig.opened[0] == "IT000038");  // newest item not already in the card
    rig.home.onGesture(tapAt(HomeScreen::cardRect().center()), rig.nav);
    CHECK(rig.opened.size() == 2);
}

TEST_CASE("home: sync progress repaints only the header; a finished sync repaints only what it changed") {
    Rig rig;
    rig.nav.damages.clear();
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncProgress;
    e.progress = {"annotations", 50, 300};
    rig.home.onSyncEvent(e, rig.nav);
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Ui);
    CHECK(rig.nav.damages[0].rect.y + rig.nav.damages[0].rect.h <= kHeaderH);
    rig.nav.damages.clear();
    rig.home.onSyncEvent(e, rig.nav);  // the same line again: nothing to repaint
    CHECK(rig.nav.damages.empty());
    // A sync that brought something: the card and rows, with quality, but never the whole screen.
    rig.lib.markOpened("IT000006", "EP000006", kNowWall);
    e.kind = SyncEvent::Kind::SyncFinished;
    rig.home.onSyncEvent(e, rig.nav);
    bool quality = false;
    for (const auto& d : rig.nav.damages) {
        quality |= d.waveform == Waveform::Quality;
        CHECK_FALSE(d.rect == Rect{0, 0, 1236, 1547});  // not the whole screen (every 6th of those flashes)
    }
    CHECK(quality);
}

TEST_CASE("home: a periodic sync that changed nothing repaints only the status line") {
    Rig rig;
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    rig.home.onSyncEvent(e, rig.nav);  // "Synced just now"
    rig.nav.now += 10 * 60'000;
    for (int i = 0; i < 3; ++i) {
        rig.nav.damages.clear();
        e.kind = SyncEvent::Kind::SyncStarted;
        rig.home.onSyncEvent(e, rig.nav);
        e.kind = SyncEvent::Kind::SyncFinished;
        rig.home.onSyncEvent(e, rig.nav);
        bool rests = false;  // "Syncing…" quickly, then the line it rests on cleanly
        for (const auto& d : rig.nav.damages) {
            CHECK(d.rect.y + d.rect.h <= kHeaderH);
            rests = d.waveform == Waveform::Quality;
        }
        CHECK(rests);
        rig.nav.now += 10 * 60'000;
    }
}

TEST_CASE("home: a sync that keeps failing the same way repaints the banner once") {
    Rig rig;
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", ""};
    rig.nav.damages.clear();
    rig.home.onSyncEvent(e, rig.nav);
    bool banner = false;
    for (const auto& d : rig.nav.damages) banner |= d.rect.y > kHeaderH && d.waveform == Waveform::Quality;
    CHECK(banner);
    rig.nav.damages.clear();
    rig.home.onSyncEvent(e, rig.nav);  // offline again: what Home shows hasn't changed
    CHECK(rig.nav.damages.empty());
}

TEST_CASE("home: showing Home asks for a sync when the last one is old") {
    Rig rig;
    CHECK(rig.nav.syncFake.syncs == 1);  // never synced in this session
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    rig.status.onEvent(e, rig.nav.now);
    rig.home.onShow(rig.nav);
    CHECK(rig.nav.syncFake.syncs == 1);
    rig.nav.now += 3 * 60'000;
    rig.home.onShow(rig.nav);
    CHECK(rig.nav.syncFake.syncs == 2);
}

TEST_CASE("home: very long Latin and Cyrillic titles stay inside the card and rows") {
    if (!fonts()) return;
    Rig rig(0);
    const std::string latin(300, 'x');
    const char* titles[] = {"On the extraordinarily long and winding titles of nineteenth-century treatises concerning the "
                            "theory of value, wages, rent, profits and the natural price of labour in a commercial society",
                            "Философия общего дела: статьи, мысли и письма Николая Фёдоровича Фёдорова, изданные посмертно "
                            "В. А. Кожевниковым и Н. П. Петерсоном, том первый и второй вместе"};
    for (int i = 0; i < 4; ++i) {
        ItemRecord r;
        r.key = test::fixtureKey("LT", i);
        r.title = i == 3 ? latin : titles[i % 2];
        for (int c = 0; c < 6; ++c) r.creators.push_back({"A.", "Verylongsurnameforlayout" + std::to_string(c), "", "author"});
        r.year = "1903";
        r.dateAdded = "2024-01-0" + std::to_string(i + 1);
        r.json = "{}";
        rig.lib.upsertItem(r);
        rig.lib.upsertAttachment({test::fixtureKey("LA", i), r.key, "application/epub+zip", "b.epub", "", "imported_file", 0,
                                  "", AttachmentRecord::State::Remote});
    }
    rig.lib.markOpened("LT000001", "LA000001", kNowWall - 3600 * 1000LL);
    rig.home.onShow(rig.nav);
    rig.draw();
    test::checkGolden(rig.canvas, "tests/golden/home-stress-real.png");
}

TEST_CASE("home: an upload repaints the header, fast when it starts and quality when it is done") {
    Rig rig;
    rig.nav.damages.clear();
    SyncEvent e;
    e.kind = SyncEvent::Kind::PushStarted;
    e.total = 2;
    rig.home.onSyncEvent(e, rig.nav);
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Ui);
    CHECK(rig.nav.damages[0].rect.y + rig.nav.damages[0].rect.h <= kHeaderH);
    CHECK(rig.status.line(0) == "Uploading 2 changes…");
    rig.nav.damages.clear();
    e.kind = SyncEvent::Kind::PushFinished;
    rig.home.onSyncEvent(e, rig.nav);
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Quality);
}

TEST_CASE("home: a close button right of the sync button quits the app") {
    Rig rig;
    const Rect close = HomeScreen::closeRect(), sync = HomeScreen::syncIndicatorRect(), gear = HomeScreen::settingsRect();
    CHECK(close.x >= sync.x + sync.w);    // to the right of sync
    CHECK(sync.x >= gear.x + gear.w);     // settings, sync, close
    CHECK(close.x + close.w <= 1236 - kMargin + 1);
    CHECK_FALSE(rig.nav.quitted);
    rig.home.onGesture(tapAt(close.center()), rig.nav);
    CHECK(rig.nav.quitted);
}

// ---- "Version X is available" ----

namespace {

struct UpdateRig : Rig {
    std::optional<std::string> notice;
    int updatesOpened = 0, dismissed = 0, pluginsOpened = 0;
    bool safeNotice = false;
    std::unique_ptr<HomeScreen> home;  // hides Rig's: built with the update actions
    UpdateRig() : Rig(40) {}
    void wire() {
        HomeActions a{[this] { ++searches; }, [this] { ++settings; },
                      [this](const ItemRow& r) { opened.push_back(r.key); }, nullptr};
        a.updateNotice = [this] { return notice; };
        a.openUpdate = [this] { ++updatesOpened; };
        a.safeModeNotice = [this] { return safeNotice; };
        a.openPlugins = [this] {
            ++pluginsOpened;
            safeNotice = false;
        };
        a.dismissUpdate = [this] {
            ++dismissed;
            notice.reset();
        };
        home = std::make_unique<HomeScreen>(status, a, [] { return kNowWall; });
        home->onShow(nav);
    }
};

}  // namespace

TEST_CASE("home: a newer version shows a banner that opens the update") {
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    REQUIRE(rig.home->updateBannerText());
    CHECK(*rig.home->updateBannerText() == "Version 0.2.5 is available");
    const size_t rowsWith = rig.home->recentRowRects().size();
    rig.home->onGesture(tapAt(rig.home->updateBannerRect().center()), rig.nav);
    CHECK(rig.updatesOpened == 1);
    rig.notice.reset();
    rig.home->onTick(rig.nav);
    CHECK_FALSE(rig.home->updateBannerText());
    CHECK(rig.home->recentRowRects().size() >= rowsWith);
}

TEST_CASE("home: × hides the update banner") {
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    rig.nav.damages.clear();
    rig.home->onGesture(tapAt(rig.home->updateDismissRect().center()), rig.nav);
    CHECK(rig.dismissed == 1);
    CHECK(rig.updatesOpened == 0);
    CHECK_FALSE(rig.home->updateBannerText());
    CHECK_FALSE(rig.nav.damages.empty());
}

TEST_CASE("home: an update found while Home is shown appears without a tap") {
    UpdateRig rig;
    rig.wire();
    CHECK_FALSE(rig.home->updateBannerText());
    rig.notice = "0.2.5";
    rig.nav.damages.clear();
    rig.home->onTick(rig.nav);
    CHECK(rig.home->updateBannerText());
    CHECK_FALSE(rig.nav.damages.empty());
    rig.nav.damages.clear();
    rig.home->onTick(rig.nav);  // nothing new: no repaint
    CHECK(rig.nav.damages.empty());
}

TEST_CASE("home: a sync error takes the banner's place") {
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", ""};
    rig.home->onSyncEvent(e, rig.nav);
    CHECK_FALSE(rig.home->updateBannerText());
    rig.home->onGesture(tapAt(rig.home->updateBannerRect().center()), rig.nav);
    CHECK(rig.updatesOpened == 0);
}

TEST_CASE("home: the update banner matches the reviewed golden") {
    if (!fonts()) return;
    UpdateRig rig;
    rig.notice = "0.2.5";
    rig.wire();
    rig.lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
    rig.home->onShow(rig.nav);
    rig.home->draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/home-update.png");
}

TEST_CASE("home: the automatic case shows \"Plugins were turned off because the app stopped. Settings → Plugins\"") {
    UpdateRig rig;
    rig.safeNotice = true;
    rig.notice = "0.2.5";  // the notice comes before the update banner
    rig.wire();
    REQUIRE(rig.home->safeModeBannerText());
    CHECK(*rig.home->safeModeBannerText() == "Plugins were turned off because the app stopped. Settings → Plugins");
    CHECK_FALSE(rig.home->updateBannerText());

    // A tap opens Settings → Plugins; once seen, the notice goes (the update banner is back).
    rig.home->onGesture(tapAt(rig.home->safeModeBannerRect().center()), rig.nav);
    CHECK(rig.pluginsOpened == 1);
    CHECK(rig.updatesOpened == 0);
    rig.home->onShow(rig.nav);
    CHECK_FALSE(rig.home->safeModeBannerText());
    CHECK(rig.home->updateBannerText());
}

TEST_CASE("home: no safe-mode notice unless asked, and a sync error takes its place") {
    UpdateRig rig;
    rig.wire();
    CHECK_FALSE(rig.home->safeModeBannerText());
    rig.safeNotice = true;
    rig.home->onTick(rig.nav);  // appears without a tap
    CHECK(rig.home->safeModeBannerText());
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "curl 6: x"};
    rig.status.onEvent(e, 1000);
    CHECK_FALSE(rig.home->safeModeBannerText());
}

TEST_CASE("home: the safe-mode notice matches the reviewed golden") {
    if (!fonts()) return;
    UpdateRig rig;
    rig.safeNotice = true;
    rig.wire();
    rig.lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
    rig.home->onShow(rig.nav);
    rig.home->draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/home-safe-mode.png");
}

// ---- A plugin's body (Settings → Home): the frame stays Home's, the blocks go in bodyRect() ----

#include <cstring>
#include <deque>

#include "screens/item_details.h"
#include "screens/item_source.h"
#include "screens/plugin_body.h"

namespace {

using json = nlohmann::json;

bool inside(Rect outer, Rect inner) { return outer.intersected(inner) == inner; }
bool meets(Rect a, Rect b) { return !a.intersected(b).empty(); }

PluginHostEvent homeTree(const json& j, uint64_t instance, uint64_t requestId = 0) {
    std::string why;
    auto t = parseTree(j, &why, TreeUse::HomeBody);
    INFO(why);
    REQUIRE(t);
    PluginHostEvent ev{PluginHostEvent::Kind::ScreenTree, "desk", "Reading Desk", "", {}, requestId, ""};
    ev.instance = instance;
    ev.tree = std::make_shared<const Block>(std::move(*t));
    return ev;
}

// A Reading Desk-like body: two Continue cards, pinned rows, a button.
json deskTree(const std::string& pinnedLabel = "PINNED") {
    json j = json::parse(R"({"kind":"column","children":[
      {"kind":"items","id":"continue","style":"card","items":[{"key":"IT000006","label":"BOOK · CONTINUE"}]},
      {"kind":"text","text":"PINNED","style":"label"},
      {"kind":"items","id":"pinned","style":"row","items":["IT000009","GONE0000","IT000012"]},
      {"kind":"button","id":"all","title":"All pins"}
    ]})");
    j["children"][1]["text"] = pinnedLabel;
    return j;
}

// Home's link as main's PluginHome is, with the plugin side faked: the body's events are recorded,
// trees are given through `mail`.
struct FakeHomeLink : HomeBodyLink, ScreenLink {
    std::deque<PluginHostEvent> mail;
    std::vector<json> sent;
    std::optional<json> kept;  // the kept tree, given first to a new body
    bool available = true;     // the chosen plugin can show its screen (plugins on, it is on)
    int opens = 0, rerenders = 0, syncs = 0, drops = 0, lefts = 0;
    uint64_t instance = 0;
    ItemSource* items = nullptr;
    ItemTap tap;
    std::function<Rect(Nav&)> area;
    std::function<Rect()> window;
    std::unique_ptr<PluginBody> b;

    PluginBody* body() override { return b.get(); }
    void shown(Nav&, std::function<Rect(Nav&)> a, std::function<Rect()> w) override {
        area = std::move(a);
        window = std::move(w);
        if (b) {
            ++rerenders;
            return;
        }
        if (!available) return;
        ++opens;
        b = std::make_unique<PluginBody>(*this, ++instance, "Reading Desk", "Desk", [this](Nav& n) { return area(n); },
                                         TreeUse::HomeBody, items, tap);
        b->setWindow([this] { return window(); });
        if (kept) mail.push_front(homeTree(*kept, instance));
    }
    void synced() override { ++syncs; }
    void drop(Nav& nav) override {
        ++drops;
        b->onClose(nav);
        b.reset();
        mail.clear();
    }
    void send(uint64_t, json e) override { sent.push_back(std::move(e)); }
    void left(uint64_t) override { ++lefts; }
    bool take(uint64_t, PluginHostEvent& out) override {
        if (mail.empty()) return false;
        out = std::move(mail.front());
        mail.pop_front();
        return true;
    }
    void give(PluginHostEvent ev) { mail.push_back(std::move(ev)); }
};

struct PluginRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    SyncStatus status;
    LibraryItemSource source{lib, nullptr, [] { return kNowWall; }};
    FakeHomeLink link;
    std::vector<std::string> opened, tapped;
    std::unique_ptr<HomeScreen> home;
    Canvas canvas{metrics().width, metrics().height};
    explicit PluginRig(std::optional<json> kept = deskTree(), bool show = true) {
        fillFixtureLibrary(lib, 40);
        lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
        link.items = &source;
        link.tap = ItemTap{[this](const ItemRow& r, std::function<void(const std::string&)> done) {
            tapped.push_back(r.key);
            if (done) done("");
        }};
        link.kept = std::move(kept);
        HomeActions a{[] {}, [] {}, [this](const ItemRow& r) { opened.push_back(r.key); }, nullptr};
        a.body = [this]() -> HomeBodyLink* { return &link; };
        home = std::make_unique<HomeScreen>(status, a, [] { return kNowWall; });
        if (show) home->onShow(nav);
    }
    const Laid& laid() {
        REQUIRE(home->pluginBody());
        REQUIRE(home->pluginBody()->laid());
        return *home->pluginBody()->laid();
    }
    Point centreOf(Rect r) { return {r.x + r.w / 2, r.y + r.h / 2}; }
    bool damaged(Rect r) const {
        for (const auto& d : nav.damages)
            if (meets(d.rect, r)) return true;
        return false;
    }
};

}  // namespace

TEST_CASE("home plugin: the body is drawn from the plugin's tree in bodyRect; the frame is the built-in Home's (golden)") {
    if (!fonts()) return;
    PluginRig rig;
    REQUIRE(rig.home->showsPluginBody());
    const Rect body = rig.home->bodyRect();
    CHECK(body.y >= HomeScreen::searchFieldRect().y + HomeScreen::searchFieldRect().h);
    CHECK(body.y + body.h <= metrics().height - kTabBarH);
    for (const Laid& c : rig.laid().children) CHECK(inside(body, c.rect));
    rig.home->draw(rig.canvas);
    test::checkGolden(rig.canvas, "tests/golden/home-plugin.png");

    // The frame (header, icons, search) is pixel for pixel the built-in Home's.
    Rig builtIn;
    builtIn.lib.markOpened("IT000006", "EP000006", kNowWall - 26 * 3600 * 1000LL);
    builtIn.home.onShow(builtIn.nav);
    builtIn.draw();
    cairo_surface_t* a = rig.canvas.surface();
    cairo_surface_t* b = builtIn.canvas.surface();
    cairo_surface_flush(a);
    cairo_surface_flush(b);
    const int stride = cairo_image_surface_get_stride(a);
    REQUIRE(stride == cairo_image_surface_get_stride(b));
    CHECK(std::memcmp(cairo_image_surface_get_data(a), cairo_image_surface_get_data(b), static_cast<size_t>(stride) * body.y) == 0);
}

TEST_CASE("home plugin: the first draw uses the kept tree, before the plugin answered anything") {
    PluginRig rig;
    CHECK(rig.link.opens == 1);
    CHECK(rig.home->showsPluginBody());  // drawn in the first frame (plugins load after it)
    CHECK(rig.home->recentRowRects().size() == 3);  // the built-in body stays ready
    // A tap on its button before the plugin answered goes to the plugin (ids are its own).
    const Laid* all = findLaid(rig.laid(), "all");
    REQUIRE(all);
    rig.home->onGesture(tapAt(rig.centreOf(all->rect)), rig.nav);
    REQUIRE(rig.link.sent.size() == 1);
    CHECK(rig.link.sent[0] == json{{"kind", "tap"}, {"id", "all"}});
    // The fresh tree replaces it: only what changed repaints.
    rig.nav.damages.clear();
    rig.link.give(homeTree(deskTree("PINNED (2)"), rig.link.instance, 1));
    rig.home->onTick(rig.nav);
    const Laid* label = &rig.laid().children.at(1);
    CHECK(plainText(*label->block) == "PINNED (2)");
    REQUIRE_FALSE(rig.nav.damages.empty());
    for (const auto& d : rig.nav.damages) CHECK(d.rect.intersected(label->rect) == d.rect);
}

TEST_CASE("home plugin: no kept tree and no tree within 1000 ms: the built-in body, never Loading…") {
    PluginRig rig(std::nullopt);
    REQUIRE(rig.home->pluginBody());
    CHECK_FALSE(rig.home->showsPluginBody());  // the built-in body shows while it waits
    CHECK(std::find(rig.nav.wakeAts.begin(), rig.nav.wakeAts.end(), rig.nav.now + HomeScreen::kFirstTreeMs) != rig.nav.wakeAts.end());
    rig.nav.now += HomeScreen::kFirstTreeMs - 1;
    rig.home->onTick(rig.nav);
    CHECK(rig.home->pluginBody());
    rig.nav.damages.clear();
    rig.nav.now += 1;
    rig.home->onTick(rig.nav);
    CHECK_FALSE(rig.home->pluginBody());  // given up on: the built-in body, which already shows
    CHECK(rig.link.drops == 1);
    CHECK(rig.link.lefts == 1);
    CHECK(rig.nav.damages.empty());
    rig.link.give(homeTree(deskTree(), 1));  // too late: nothing takes it
    rig.home->onTick(rig.nav);
    CHECK_FALSE(rig.home->showsPluginBody());
    // The next show tries again; a tree within the time shows.
    rig.home->onShow(rig.nav);
    CHECK(rig.link.opens == 2);
    rig.link.mail.clear();
    rig.nav.now += 500;
    rig.link.give(homeTree(deskTree(), rig.link.instance));
    rig.home->onTick(rig.nav);
    CHECK(rig.home->showsPluginBody());
}

TEST_CASE("home plugin: a ScreenError while shown (render failed, plugin off or reloaded) → built-in body; the next show retries") {
    PluginRig rig;
    REQUIRE(rig.home->showsPluginBody());
    PluginHostEvent err{PluginHostEvent::Kind::ScreenError, "desk", "Reading Desk", "Reading Desk can't show this screen right now", {}, 0, ""};
    err.instance = rig.link.instance;
    rig.link.give(err);
    rig.nav.damages.clear();
    rig.home->onTick(rig.nav);
    CHECK_FALSE(rig.home->pluginBody());
    CHECK_FALSE(rig.home->showsPluginBody());
    CHECK(rig.damaged(rig.home->bodyRect()));  // the built-in body is drawn there
    CHECK(rig.home->recentRowRects().size() == 3);
    rig.home->draw(rig.canvas);  // no crash, no blank body
    // Plugins turned off (safe mode, or the plugin is off): the next show shows the built-in body.
    rig.link.available = false;
    rig.home->onShow(rig.nav);
    CHECK_FALSE(rig.home->pluginBody());
    CHECK(rig.link.opens == 1);
    // Back on: the next show tries again.
    rig.link.available = true;
    rig.home->onShow(rig.nav);
    CHECK(rig.link.opens == 2);
    CHECK(rig.home->showsPluginBody());
}

TEST_CASE("home plugin: a ScreenError that came while Home was hidden → dropped and reopened with its kept tree in the same show") {
    PluginRig rig;
    REQUIRE(rig.home->showsPluginBody());
    PluginHostEvent err{PluginHostEvent::Kind::ScreenError, "desk", "Reading Desk", "x", {}, 0, ""};
    err.instance = rig.link.instance;
    rig.link.give(err);  // the plugin was reloaded from Settings → Plugins while Home was hidden
    rig.home->onShow(rig.nav);
    CHECK(rig.link.opens == 2);
    CHECK(rig.link.rerenders == 0);
    CHECK(rig.link.drops == 1);
    CHECK(rig.home->showsPluginBody());
}

TEST_CASE("home plugin: zk.ui.close on Home's body drops it; nothing more is sent; built-in until the next show") {
    PluginRig rig;
    PluginHostEvent close{PluginHostEvent::Kind::ScreenClose, "desk", "Reading Desk", "", {}, 0, ""};
    close.instance = rig.link.instance;
    rig.link.give(close);
    rig.home->onTick(rig.nav);
    CHECK_FALSE(rig.home->pluginBody());
    CHECK(rig.link.lefts == 0);  // the plugin closed it itself
    CHECK(rig.link.sent.empty());
    rig.home->onShow(rig.nav);
    CHECK(rig.link.opens == 2);
}

TEST_CASE("home plugin: a sync with no changes repaints nothing of the body and renders nothing; a progress change repaints only its block") {
    PluginRig rig;
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    rig.home->onSyncEvent(e, rig.nav);  // "Synced just now"
    rig.nav.now += 10 * 60'000;
    for (int i = 0; i < 2; ++i) {
        rig.nav.damages.clear();
        e.kind = SyncEvent::Kind::SyncStarted;
        rig.home->onSyncEvent(e, rig.nav);
        e.kind = SyncEvent::Kind::SyncFinished;
        rig.home->onSyncEvent(e, rig.nav);
        for (const auto& d : rig.nav.damages) CHECK(d.rect.y + d.rect.h <= kHeaderH);  // the status line only
        rig.nav.now += 10 * 60'000;
    }
    CHECK(rig.link.syncs == 0);
    // The plugin renders the same tree again: nothing repaints.
    rig.nav.damages.clear();
    rig.link.give(homeTree(deskTree(), rig.link.instance));
    rig.home->onTick(rig.nav);
    CHECK(rig.nav.damages.empty());
    // A pinned item read further (and a sync that brought something): only the pinned block.
    rig.lib.saveReading("IT000012", "", 0.4);
    rig.nav.damages.clear();
    e.report.itemsUpdated = 1;
    rig.home->onSyncEvent(e, rig.nav);
    CHECK(rig.link.syncs == 1);  // rendered again
    const Rect pinned = findLaid(rig.laid(), "pinned")->rect;
    bool any = false;
    for (const auto& d : rig.nav.damages) {
        if (d.rect.y + d.rect.h <= kHeaderH) continue;
        any = true;
        CHECK(d.rect.intersected(pinned) == d.rect);
    }
    CHECK(any);
}

TEST_CASE("home plugin: download progress repaints only the row of that item") {
    PluginRig rig;
    const Laid* pinned = findLaid(rig.laid(), "pinned");
    REQUIRE(pinned);
    REQUIRE(pinned->parts.size() == 2);
    rig.nav.damages.clear();
    SyncEvent e;
    e.kind = SyncEvent::Kind::DownloadProgress;
    e.itemKey = "IT000012";
    rig.home->onSyncEvent(e, rig.nav);
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].rect == pinned->parts[1].rect);
}

TEST_CASE("home plugin: banners still draw over the body's bottom: the body ends where the banner begins") {
    PluginRig rig;
    const Rect before = rig.home->bodyRect();
    CHECK(before.y + before.h <= metrics().height - kTabBarH);
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    e.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", ""};
    rig.nav.damages.clear();
    rig.home->onSyncEvent(e, rig.nav);
    const Rect after = rig.home->bodyRect();
    const Rect banner{0, metrics().height - kTabBarH - 150, metrics().width, 150};
    CHECK(after.y + after.h <= banner.y);
    CHECK(after.h < before.h);
    for (const Laid& c : rig.laid().children) CHECK_FALSE(meets(c.rect, banner));
    CHECK(rig.damaged(banner));
}

TEST_CASE("home plugin: a tap on an items row opens it the app's way; a long-press opens the item sheet above the tab bar") {
    PluginRig rig;
    const Laid* pinned = findLaid(rig.laid(), "pinned");
    REQUIRE(pinned);
    const Rect row = pinned->parts.at(0).rect;
    rig.home->onGesture(tapAt({row.x + 20, row.y + row.h / 2}), rig.nav);
    CHECK(rig.tapped == std::vector<std::string>{"IT000009"});
    CHECK(rig.link.sent.empty());  // the app's own: nothing for the plugin
    rig.home->onGesture(Gesture{GestureType::LongPress, {row.x + 20, row.y + row.h / 2}, {row.x + 20, row.y + row.h / 2}}, rig.nav);
    REQUIRE(rig.home->pluginBody()->sheet());
    CHECK(rig.home->pluginBody()->sheet()->row().key == "IT000009");
    CHECK_FALSE(rig.home->sheet());  // not the built-in one: the body's, over Home's sheet area
    const Rect area{0, 0, metrics().width, metrics().height - kTabBarH};
    rig.home->draw(rig.canvas);
    // Open in the sheet: the app's way again.
    const Rect open = rig.home->pluginBody()->sheet()->openRect(area);
    rig.home->onGesture(tapAt(rig.centreOf(open)), rig.nav);
    CHECK_FALSE(rig.home->pluginBody()->sheet());
    CHECK(rig.tapped == std::vector<std::string>{"IT000009", "IT000009"});
    // The frame's taps still work over a plugin's body.
    rig.home->onGesture(tapAt(HomeScreen::closeRect().center()), rig.nav);
    CHECK(rig.nav.quitted);
}

TEST_CASE("home plugin: item details from the body's sheet outlive the body (dropped meanwhile) and still open the item") {
    PluginRig rig;
    const Rect row = findLaid(rig.laid(), "pinned")->parts.at(0).rect;
    const Point at{row.x + 20, row.y + row.h / 2};
    rig.home->onGesture(Gesture{GestureType::LongPress, at, at}, rig.nav);
    REQUIRE(rig.home->pluginBody()->sheet());
    const Rect area{0, 0, metrics().width, metrics().height - kTabBarH};
    rig.home->onGesture(tapAt(rig.centreOf(rig.home->pluginBody()->sheet()->moreRect(area))), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    // The plugin fails meanwhile: Home drops its body.
    PluginHostEvent err{PluginHostEvent::Kind::ScreenError, "desk", "Reading Desk", "x", {}, 0, ""};
    rig.link.give(err);
    rig.home->onTick(rig.nav);
    REQUIRE_FALSE(rig.home->pluginBody());
    Screen& details = *rig.nav.pushed[0];
    details.onShow(rig.nav);
    details.onGesture(tapAt(rig.centreOf(dynamic_cast<ItemDetailsScreen&>(details).openRect())), rig.nav);
    CHECK(rig.tapped == std::vector<std::string>{"IT000009"});
}

TEST_CASE("home plugin: the body fits above the tab bar on KT4 and Scribe") {
    if (!fonts()) return;
    for (const Metrics* m : {&kKt4, &kScribe, &kPw5}) {
        ScopedMetrics scoped(*m);
        PluginRig rig;
        const Rect body = rig.home->bodyRect();
        CHECK(body.y >= HomeScreen::searchFieldRect().y + HomeScreen::searchFieldRect().h);
        CHECK(body.y + body.h <= m->height - kTabBarH);
        for (const Laid& c : rig.laid().children) {
            INFO(m->device << " body " << body.x << "," << body.y << " " << body.w << "x" << body.h << " block " << c.rect.x << "," << c.rect.y << " " << c.rect.w << "x" << c.rect.h);
            CHECK(inside(body, c.rect));
        }
        rig.home->draw(rig.canvas);
    }
}
