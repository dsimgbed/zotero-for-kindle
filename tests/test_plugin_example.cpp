// Task 19: the example plugin end to end, and the desktop developer loop's watcher.

#include <doctest/doctest.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <vector>

#include "app/plugin_watch.h"
#include "fake_plugins.h"
#include "plugins/events.h"
#include "plugins/host.h"
#include "store/library.h"
#include "ui/metrics.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

ItemRecord book(const std::string& key, const std::string& title, const std::string& extra) {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = "book";
    r.title = title;
    r.json = nlohmann::json{{"extra", extra}}.dump();
    r.creators = {{"Robert", "Adler", "", "author"}};
    r.year = "2007";
    r.dateAdded = "2024-01-01T00:00:00Z";
    return r;
}

void addPdf(Library& lib, const std::string& key, const std::string& parent) {
    AttachmentRecord pdf;
    pdf.key = key;
    pdf.parentKey = parent;
    pdf.contentType = "application/pdf";
    pdf.filename = "b.pdf";
    pdf.linkMode = "imported_file";
    pdf.state = AttachmentRecord::State::Local;
    pdf.localPath = "tests/data/pdf/sample.pdf";
    lib.upsertAttachment(pdf);
}

void addHighlight(Library& lib, const std::string& key, const std::string& parent, const std::string& text,
                  const std::string& comment, const std::string& page, const std::string& sort) {
    AnnotationRecord a;
    a.key = key;
    a.parentKey = parent;
    a.type = "highlight";
    a.text = text;
    a.comment = comment;
    a.color = "#ffd400";
    a.pageLabel = page;
    a.sortIndex = sort;
    a.dateModified = "2025-03-02T10:00:00Z";
    a.json = R"({"dateAdded":"2025-03-01T10:00:00Z"})";
    lib.upsertAnnotation(a);
}

}  // namespace

TEST_CASE("example: Highlights to Markdown, loaded by the host against the fixture library, writes data/<title>.md with the book's highlights after a highlightCreated and after syncFinished") {
    FakePlugins fp;
    fs::copy("tests/data/plugins/examples/highlights-to-markdown", fp.path("highlights-to-markdown"),
             fs::copy_options::recursive);
    Db db = Db::open(fp.path("library.db"));
    Library lib(db);
    lib.upsertItem(book("BOOKCITE", "The Geometry of Random Fields", "Citation Key: adler2007"));
    lib.upsertItem(book("BOOKPLAI", "Plain: Title?", ""));  // no citation key: named after the title
    addPdf(lib, "PDFCITE1", "BOOKCITE");
    addPdf(lib, "PDFPLAI1", "BOOKPLAI");
    addHighlight(lib, "ANNOTAT1", "PDFCITE1", "random fields", "nice", "12", "00002|000100|00100");
    addHighlight(lib, "ANNOTAT0", "PDFCITE1", "first line", "", "3", "00001|000100|00100");

    PluginHostDeps d = fp.deps();
    d.dbPath = fp.path("library.db");
    d.handlerBudgetMs = 5000;
    PluginHost host(d);
    host.setEnabled("highlights-to-markdown", true);
    host.start();

    // A highlight made on the Kindle: the book's file is written at once, citation key as its name.
    std::vector<PluginEvent> got;
    FnPluginSink sink([&](PluginEvent e) { got.push_back(std::move(e)); });
    plugins::annotationEvent(sink, lib, "highlightCreated", "ANNOTAT1");
    REQUIRE(got.size() == 1);
    host.dispatch(got[0]);
    REQUIRE(host.waitIdle(8000));
    CAPTURE(fp.log("highlights-to-markdown"));
    REQUIRE(fp.exists("highlights-to-markdown/data/adler2007.md"));
    const std::vector<std::string> expected = {
        "# The Geometry of Random Fields",
        "",
        "Robert Adler (2007). Citation key: `adler2007`",
        "",
        "> first line",
        "",
        "— p. 3",
        "",
        "> random fields",
        "",
        "nice — p. 12",
    };
    CHECK(linesOf(fp.read("highlights-to-markdown/data/adler2007.md")) == expected);

    // A highlight that came in with a sync (made in Zotero): only noted, written when the sync ends.
    addHighlight(lib, "ANNOTAT2", "PDFPLAI1", "line one\nline two", "a comment", "", "00001|000100|00100");
    host.dispatch({"highlightCreated", {{"origin", "zotero"}, {"annotations", nlohmann::json::array({{{"key", "ANNOTAT2"}, {"itemKey", "BOOKPLAI"}}})}}, false});
    REQUIRE(host.waitIdle(8000));
    CHECK_FALSE(fp.exists("highlights-to-markdown/data/Plain Title.md"));
    host.dispatch({"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false});
    REQUIRE(host.waitIdle(8000));
    CAPTURE(fp.log("highlights-to-markdown"));
    REQUIRE(fp.exists("highlights-to-markdown/data/Plain Title.md"));
    const std::vector<std::string> expected2 = {
        "# Plain: Title?",
        "",
        "Robert Adler (2007)",
        "",
        "> line one",
        "> line two",
        "",
        "a comment",
    };
    CHECK(linesOf(fp.read("highlights-to-markdown/data/Plain Title.md")) == expected2);
    host.stop();
}

TEST_CASE("example: the watcher reloads a plugin whose main.js changed, once a second, and only that one") {
    FakePlugins fp;
    fp.add("a", "console.log('a');");
    fp.add("b", "console.log('b');");
    std::vector<std::string> reloaded;
    PluginWatcher w(fp.dir(), [&](const std::string& id) { reloaded.push_back(id); });
    w.poll(0);  // the first look only records
    CHECK(reloaded.empty());
    fp.write("a/main.js", "console.log('a2');");
    fs::last_write_time(fp.path("a/main.js"), fs::last_write_time(fp.path("a/main.js")) + std::chrono::seconds(5));
    w.poll(500);  // too soon
    CHECK(reloaded.empty());
    w.poll(1000);
    CHECK(reloaded == std::vector<std::string>{"a"});
    w.poll(2000);  // unchanged since: nothing more
    CHECK(reloaded.size() == 1);
    fp.add("c", "console.log('c');");  // a new plugin folder appears: reloaded (it loads, if enabled)
    w.poll(3000);
    CHECK(reloaded == std::vector<std::string>{"a", "c"});
    w.poll(4000);
    CHECK(reloaded.size() == 2);
}

TEST_CASE("example: the watcher survives a deleted main.js: no reload, no crash; recreating it reloads") {
    FakePlugins fp;
    fp.add("a", "console.log('a');");
    std::vector<std::string> reloaded;
    PluginWatcher w(fp.dir(), [&](const std::string& id) { reloaded.push_back(id); });
    w.poll(0);
    fs::remove(fp.path("a/main.js"));
    w.poll(1000);
    CHECK(reloaded.empty());
    fp.write("a/main.js", "console.log('a again');");
    fs::last_write_time(fp.path("a/main.js"), fs::last_write_time(fp.path("a/main.js")) + std::chrono::seconds(7));
    w.poll(2000);
    CHECK(reloaded == std::vector<std::string>{"a"});
}

namespace {
bool logged(FakePlugins& fp, const std::string& id, const std::string& text) {
    for (int i = 0; i < 80; ++i) {
        if (fp.log(id).find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
}  // namespace

TEST_CASE("example: with loadEagerly start() loads enabled plugins at once; by default they wait for an event") {
    for (bool eager : {true, false}) {
        FakePlugins fp;
        fp.add("a", "console.log('a loaded');");
        fp.add("off", "console.log('off loaded');");
        PluginHostDeps d = fp.deps();
        d.loadEagerly = eager;
        PluginHost host(d);
        host.setEnabled("a", true);
        host.start();
        host.waitIdle(3000);
        if (eager) {
            CHECK(logged(fp, "a", "a loaded"));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("a").empty());
        }
        CHECK(fp.log("off").empty());  // never enabled: never loaded
        // An edit: reload(id) loads an enabled plugin that wasn't loaded, in the eager mode only.
        fp.write("a/main.js", "console.log('a v2 loaded');");
        host.reload("a");
        host.waitIdle(3000);
        if (eager) CHECK(logged(fp, "a", "a v2 loaded"));
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("a").find("v2") == std::string::npos);
        }
        host.stop();
    }
}

TEST_CASE("example: turning a plugin on loads it at once in the eager mode (developer); by default it waits for an event") {
    for (bool eager : {true, false}) {
        FakePlugins fp;
        fp.add("a", "console.log('a loaded');");
        PluginHostDeps d = fp.deps();
        d.loadEagerly = eager;
        PluginHost host(d);
        host.start();
        host.waitIdle(3000);  // nothing is on: nothing loaded
        CHECK(fp.log("a").empty());
        host.setEnabled("a", true);  // Settings → Plugins → Turn on
        host.waitIdle(3000);
        if (eager) {
            CHECK(logged(fp, "a", "a loaded"));
            host.setEnabled("a", true);  // on again while loaded: not reloaded
            host.waitIdle(3000);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const std::string log = fp.log("a");
            size_t n = 0;
            for (size_t at = log.find("a loaded"); at != std::string::npos; at = log.find("a loaded", at + 1)) ++n;
            CHECK(n == 1);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("a").empty());  // the Kindle: lazy, until its first event
        }
        host.stop();
    }
}

TEST_CASE("example: reload(id) in the eager mode loads a folder created after start() that the state enables; not otherwise") {
    for (bool eager : {true, false}) {
        FakePlugins fp;
        fp.write(".state.json", R"({"plugins":{"late":{"enabled":true,"everEnabled":true}}})");
        PluginHostDeps d = fp.deps();
        d.loadEagerly = eager;
        PluginHost host(d);
        host.start();
        host.waitIdle(3000);
        fp.add("late", "console.log('late loaded');");  // the folder appears while the app runs
        host.reload("late");
        host.waitIdle(3000);
        if (eager) {
            CHECK(logged(fp, "late", "late loaded"));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(fp.log("late").empty());
        }
        host.stop();
    }
}

TEST_CASE("example: in the eager mode a plugin turned on loads; reload(id) of a disabled or a broken one stays silent") {
    FakePlugins fp;
    fp.add("wasoff", "console.log('wasoff loaded');");
    fp.add("off", "console.log('off loaded');");
    fp.write("broken/plugin.json", "{ not json");
    fp.write("broken/main.js", "console.log('broken loaded');");
    PluginHostDeps d = fp.deps();
    d.loadEagerly = true;
    PluginHost host(d);
    host.start();
    host.waitIdle(3000);  // nothing is enabled: nothing loaded
    host.setEnabled("wasoff", true);  // turning on loads it (developer mode)
    host.reload("off");
    host.reload("broken");
    host.waitIdle(3000);
    CHECK(logged(fp, "wasoff", "wasoff loaded"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(fp.log("off").empty());
    CHECK(fp.log("broken").empty());
    host.stop();
}

TEST_CASE("example: --enable-plugin turns a plugin on in developer mode; the notes say why the others won't run") {
    FakePlugins fp;
    fp.add("hello", "console.log('hello loaded');");
    fp.add("quiet", "console.log('quiet loaded');");
    addPlugin(fp, "aged", "Aged", "\"library:read\"", ",\"minApp\":\"99.0.0\"");
    fp.write("broken/plugin.json", "{ not json");
    fp.write("broken/main.js", "");
    PluginHostDeps d = fp.deps();
    d.loadEagerly = true;
    PluginHost host(d);

    // Before: everything is off or can't load, and each line says what to do.
    auto notes = devLoopNotes(host.status(), d.appVersion);
    CHECK(std::count(notes.begin(), notes.end(),
                     "plugin hello: off — turn it on in Settings → Plugins, or start with --enable-plugin hello") == 1);
    CHECK(std::count(notes.begin(), notes.end(),
                     "plugin aged: incompatible — needs app 99.0.0 (this app is 9.9.9); lower minApp in plugin.json to run it "
                     "here") == 1);
    bool brokenNoted = false;
    for (const auto& n : notes) brokenNoted = brokenNoted || n.rfind("plugin broken: can't load — ", 0) == 0;
    CHECK(brokenNoted);
    CHECK(notes.size() == 4);

    // The flag: hello is turned on (and remembered); an incompatible folder stays off; an unknown id is reported.
    const auto unknown = enablePluginsForDev(host, {"hello", "aged", "nope"});
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0] == "plugin nope: --enable-plugin found no folder \"nope\" in the plugins folder");
    CHECK(statusOf(host.status(), "hello")->enabled);
    CHECK_FALSE(statusOf(host.status(), "aged")->enabled);
    CHECK(fp.read(".state.json").find("\"hello\"") != std::string::npos);
    notes = devLoopNotes(host.status(), d.appVersion);
    CHECK(notes.size() == 3);  // hello is no longer noted; quiet, aged and broken are
    CHECK(std::count(notes.begin(), notes.end(),
                     "plugin quiet: off — turn it on in Settings → Plugins, or start with --enable-plugin quiet") == 1);

    host.start();  // developer mode: what is on loads at once
    host.waitIdle(3000);
    CHECK(logged(fp, "hello", "hello loaded"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(fp.log("quiet").empty());
    host.stop();
}

// ───────────── Feed Riffle (the screens example) end to end ─────────────

#include <chrono>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#include "fake_http.h"
#include "fn_http.h"

namespace {

const char* kFeed = R"(<?xml version="1.0"?><rss version="2.0"><channel><title>Probability</title>
<item><title>Brownian sheets</title><link>https://arxiv.org/abs/2401.00001</link><guid>oai:arXiv.org:2401.00001</guid>
<description>About sheets.</description></item>
<item><title>Random fields</title><link>https://arxiv.org/abs/2401.00002</link><guid>oai:arXiv.org:2401.00002</guid>
<description>About fields.</description></item>
</channel></rss>)";

// All the text a tree shows: text, cards, notices, buttons, list rows, chips, joined by " | ".
void collect(const Block& b, std::string& out) {
    auto add = [&](const std::string& x) { if (!x.empty()) out += x + " | "; };
    add(plainText(b));
    add(b.title);
    add(b.body);
    add(b.text);
    for (const auto& r : b.rows) add(r.title + " " + r.meta + (r.subtitle.empty() ? "" : " " + r.subtitle));
    for (const auto& c : b.chips) add(c.title);
    for (const auto& c : b.children) collect(c, out);
}
std::string allText(const Block& b) {
    std::string out;
    collect(b, out);
    return out;
}

struct Riffle {
    FakePlugins fp;
    Db db = Db::open(fp.path("library.db"));
    Library lib{db};
    std::unique_ptr<PluginHost> host;
    std::deque<PluginHostEvent> got;
    std::mutex mu;
    FakeHttp fake;

    Riffle() {
        fs::copy("tests/data/plugins/examples/feed-riffle", fp.path("feed-riffle"), fs::copy_options::recursive);
        fp.write("feed-riffle/settings.json", R"({"feeds":["https://feeds.test/prob.xml"],"tags":["riffled"]})");
        lib.upsertCollection({"COLLINB1", "Inbox", "", 1});
        fake.expect("GET", "https://feeds.test/prob.xml", FakeHttp::ok(kFeed));
        PluginHostDeps d = fp.deps();
        d.dbPath = fp.path("library.db");
        d.handlerBudgetMs = 5000;
        d.backgroundBudgetMs = 10000;
        d.renderBudgetMs = 5000;
        d.actionBudgetMs = 10000;
        d.makeHttp = [this] {
            return std::make_unique<FnHttp>([this](const HttpRequest& r) {
                std::lock_guard<std::mutex> lock(mu);
                return fake.send(r);
            });
        };
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("feed-riffle", true);
        host->start();
    }
    ~Riffle() { host->stop(); }

    std::optional<PluginHostEvent> next(PluginHostEvent::Kind kind, uint64_t instance, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            PluginHostEvent ev;
            while (host->poll(ev)) got.push_back(std::move(ev));
            for (auto it = got.begin(); it != got.end(); ++it)
                if (it->kind == kind && (instance == 0 || it->instance == instance)) {
                    PluginHostEvent out = std::move(*it);
                    got.erase(it);
                    return out;
                }
            if (std::chrono::steady_clock::now() >= end) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // The next tree of `instance` whose text (all blocks) contains `needle`.
    std::optional<PluginHostEvent> treeWith(uint64_t instance, const std::string& needle) {
        for (int i = 0; i < 20; ++i) {
            auto t = next(PluginHostEvent::Kind::ScreenTree, instance);
            if (!t) return std::nullopt;
            if (allText(*t->tree).find(needle) != std::string::npos) return t;
        }
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("example: Feed Riffle — open, render, swipe, keep into a collection, close with a result, the summary; no network") {
    Riffle r;
    // The background run fetches the (fake) feed and deals the cards.
    r.host->runBackground(1);
    REQUIRE(r.host->waitIdle(15000));
    CAPTURE(r.fp.log("feed-riffle"));
    REQUIRE(r.fake.sent.size() == 1);

    // Open the deck: the first card, and what is left.
    const uint64_t deck = r.host->openScreen("feed-riffle", "deck");
    r.host->screenEvent(deck, {{"kind", "open"}});
    auto t = r.treeWith(deck, "2 left");
    REQUIRE(t);
    CHECK(allText(*t->tree).find("Brownian sheets") != std::string::npos);

    // Swipe left: discard. The next card is on top, with a line saying so.
    r.host->screenEvent(deck, {{"kind", "swipe"}, {"id", "card"}, {"direction", "left"}});
    t = r.treeWith(deck, "1 left");
    REQUIRE(t);
    CHECK(allText(*t->tree).find("Random fields") != std::string::npos);
    CHECK(allText(*t->tree).find("Discarded") != std::string::npos);

    // Swipe right: keep opens the filing screen.
    r.host->screenEvent(deck, {{"kind", "swipe"}, {"id", "card"}, {"direction", "right"}});
    auto o = r.next(PluginHostEvent::Kind::ScreenOpen, 0);
    REQUIRE(o);
    CHECK(o->parent == deck);
    CHECK(o->screenId == "file");
    const uint64_t filing = o->instance;
    r.host->screenEvent(filing, {{"kind", "open"}});
    t = r.treeWith(filing, "Inbox");
    REQUIRE(t);
    r.host->screenEvent(filing, {{"kind", "select"}, {"id", "collections"}, {"row", "COLLINB1"}});
    r.host->screenEvent(filing, {{"kind", "change"}, {"id", "note"}, {"text", "read this"}});
    r.host->screenEvent(filing, {{"kind", "tap"}, {"id", "file"}});

    // Filing closes the screen with a result line; the deck shows it and has run out.
    auto c = r.next(PluginHostEvent::Kind::ScreenClose, filing);
    REQUIRE(c);
    REQUIRE(c->value.is_string());
    CHECK(c->value.get<std::string>() == "Kept into Inbox");
    t = r.treeWith(deck, "cleared");
    REQUIRE(t);
    CHECK(allText(*t->tree).find("1 kept") != std::string::npos);
    CHECK(allText(*t->tree).find("1 discarded") != std::string::npos);

    // The filing is queued for Zotero: the item, its collection, its tags and the note.
    REQUIRE(r.host->waitIdle(8000));
    std::set<std::string> kinds;
    json created;
    for (const auto& ch : r.lib.pendingPluginChanges("feed-riffle")) {
        kinds.insert(kindName(ch.kind));
        if (ch.kind == PluginChange::Kind::CreateItem) created = json::parse(ch.value);
    }
    CHECK(kinds == std::set<std::string>{"createItem", "addNote"});
    CHECK(created["fields"]["title"] == "Random fields");
    CHECK(created["collections"] == json::array({"COLLINB1"}));
    CHECK(created["tags"] == json::array({"riffled"}));

    // Done: the summary screen.
    r.host->screenEvent(deck, {{"kind", "tap"}, {"id", "done"}});
    o = r.next(PluginHostEvent::Kind::ScreenOpen, 0);
    REQUIRE(o);
    CHECK(o->screenId == "summary");
    t = r.treeWith(o->instance, "cleared");
    REQUIRE(t);
    r.host->closeScreen(deck);
}

// ───────────────────────────── Reading Desk ─────────────────────────────

namespace {

void deskItem(Library& lib, const std::string& key, const std::string& type, const std::string& title,
              const std::string& added, const std::string& collection, const std::string& fileType) {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = type;
    r.title = title;
    r.dateAdded = added;
    r.dateModified = added;
    r.json = "{}";
    if (!collection.empty()) r.collections.push_back(collection);
    lib.upsertItem(r);
    if (fileType.empty()) return;
    AttachmentRecord a;
    a.key = "F" + key.substr(1);
    a.parentKey = key;
    a.contentType = fileType;
    a.filename = "file";
    a.linkMode = "imported_file";
    lib.upsertAttachment(a);
}

using Keys = std::vector<std::pair<std::string, std::string>>;

// A Home body tree, read the way a test needs it: its items blocks by id ({key, label}, and the
// style), its label texts in order, the tabs' chips, and the quote.
struct DeskTree {
    json tree;
    std::map<std::string, Keys> items;
    std::map<std::string, std::string> styles;
    std::vector<std::string> labels;  // text blocks of style "label", in order
    std::vector<std::string> order;   // the root's children: an items block's id, "label:<text>", or a kind
    std::vector<std::string> tabs;    // chip titles
    std::vector<std::string> selected;
    json quote;  // null when there is none

    explicit DeskTree(json t) : tree(std::move(t)) {
        std::function<void(const json&, bool)> walk = [&](const json& n, bool top) {
            if (!n.is_object()) return;  // a falsy child, skipped as the app skips it
            const std::string kind = n.value("kind", "");
            if (kind == "items") {
                const std::string id = n["id"].get<std::string>();
                auto& v = items[id];
                styles[id] = n.value("style", "");
                for (const auto& e : n["items"])
                    v.push_back(e.is_string() ? std::make_pair(e.get<std::string>(), std::string())
                                              : std::make_pair(e["key"].get<std::string>(), e.value("label", "")));
                if (top) order.push_back(id);
            } else if (kind == "text" && n.value("style", "") == "label") {
                labels.push_back(n["text"].get<std::string>());
                if (top) order.push_back("label:" + labels.back());
            } else if (kind == "chips" && n.value("id", "") == "tabs") {
                for (const auto& c : n["chips"]) tabs.push_back(c["title"].get<std::string>());
                for (const auto& c : n["selected"]) selected.push_back(c.get<std::string>());
                if (top) order.push_back("tabs");
            } else if (kind == "quote") {
                quote = n;
                if (top) order.push_back("quote");
            } else if (top) {
                order.push_back(kind);
            }
            if (n.contains("children"))
                for (const auto& c : n["children"]) walk(c, kind == "column" && &n == &tree ? true : false);
        };
        walk(tree, false);
    }
    bool has(const std::string& id) const { return items.count(id) > 0; }
    Keys at(const std::string& id) const { return has(id) ? items.at(id) : Keys{}; }
};

// Today on this machine, as the plugin dates its passage ("2026-11-20").
std::string localToday() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

struct Desk {
    FakePlugins fp;
    Db db = Db::open(fp.path("library.db"));
    Library lib{db};
    std::unique_ptr<PluginHost> host;
    std::deque<PluginHostEvent> got;

    // renderBudgetMs: the app's own is 1000; tests that don't measure speed give room.
    explicit Desk(const std::string& settings, int renderBudgetMs = 5000) {
        fs::copy("tests/data/plugins/examples/reading-desk", fp.path("reading-desk"), fs::copy_options::recursive);
        if (!settings.empty()) fp.write("reading-desk/settings.json", settings);
        lib.upsertCollection({"COLLINBX", "Inbox", "", 1});
        lib.upsertCollection({"COLLOTHR", "Reading", "", 1});
        // Books (by type, or by an EPUB file), papers, and the inbox (added dates differ).
        deskItem(lib, "BOOK0001", "book", "A Book", "2024-01-01T00:00:00Z", "", "application/pdf");
        deskItem(lib, "BOOK0002", "bookSection", "A Chapter", "2024-01-01T01:00:00Z", "", "application/pdf");
        deskItem(lib, "BOOK0003", "book", "A Pinned Book", "2024-01-01T02:00:00Z", "", "application/pdf");
        deskItem(lib, "EPUB0001", "journalArticle", "An EPUB", "2024-01-02T00:00:00Z", "", "application/epub+zip");
        deskItem(lib, "PAPR0001", "journalArticle", "A Paper", "2024-01-03T00:00:00Z", "", "application/pdf");
        deskItem(lib, "PAPR0002", "journalArticle", "Another Paper", "2024-01-04T00:00:00Z", "", "application/pdf");
        deskItem(lib, "INBX0001", "journalArticle", "Old", "2024-02-01T00:00:00Z", "COLLINBX", "application/pdf");
        deskItem(lib, "INBX0002", "journalArticle", "Newest", "2024-03-01T00:00:00Z", "COLLINBX", "application/pdf");
        deskItem(lib, "INBX0003", "journalArticle", "Middle", "2024-02-15T00:00:00Z", "COLLINBX", "application/pdf");
        deskItem(lib, "INBX0004", "journalArticle", "Done", "2024-04-01T00:00:00Z", "COLLINBX", "application/pdf");
        lib.markRead("INBX0004", true);
        PluginHostDeps d = fp.deps();
        d.dbPath = fp.path("library.db");
        d.handlerBudgetMs = 5000;
        d.renderBudgetMs = renderBudgetMs;
        d.actionBudgetMs = 10000;
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("reading-desk", true);
        host->start();
    }
    ~Desk() { host->stop(); }

    // A value in the plugin's zk.storage, written as the app keeps it (before the plugin reads it).
    void store(const std::string& key, const json& value) {
        Db s = Db::open(fp.path("reading-desk/storage.db"));
        s.exec("CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
        s.prepare("INSERT OR REPLACE INTO kv(key, value) VALUES(?, ?)").bind(1, key).bind(2, value.dump()).step();
    }

    std::optional<PluginHostEvent> next(PluginHostEvent::Kind kind, uint64_t key, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            PluginHostEvent ev;
            while (host->poll(ev)) got.push_back(std::move(ev));
            for (auto it = got.begin(); it != got.end(); ++it) {
                // ScreenOpen: by the screen it opened over.
                const bool match = kind == PluginHostEvent::Kind::ActionResult
                                       ? (it->kind == kind || it->kind == PluginHostEvent::Kind::ActionError) && it->requestId == key
                                   : kind == PluginHostEvent::Kind::ScreenOpen ? it->kind == kind && it->parent == key
                                                                               : it->kind == kind && it->instance == key;
                if (match) {
                    PluginHostEvent out = std::move(*it);
                    got.erase(it);
                    return out;
                }
            }
            if (std::chrono::steady_clock::now() >= end) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // Home's body, rendered afresh: opens the screen the first time, then rerenders.
    uint64_t inst = 0;
    DeskTree render() {
        if (!inst) inst = host->openScreen("reading-desk", "desk", nullptr, TreeUse::HomeBody);
        else host->rerender(inst);
        return tree(inst);
    }
    // The latest tree of `instance` once the plugin is idle (a pin's zk.ui.refresh renders too).
    std::shared_ptr<const Block> latest(uint64_t instance, json* value = nullptr) {
        auto t = next(PluginHostEvent::Kind::ScreenTree, instance);
        CAPTURE(fp.log("reading-desk"));
        REQUIRE(t);
        REQUIRE(host->waitIdle(8000));
        while (auto more = next(PluginHostEvent::Kind::ScreenTree, instance, 50)) t = more;
        if (value) *value = t->value;
        return t->tree;
    }
    DeskTree tree(uint64_t instance) {
        json value;
        latest(instance, &value);
        REQUIRE(value.is_object());
        return DeskTree(value);
    }
    std::string text(uint64_t instance) { return allText(*latest(instance)); }
    // What the user did on Home's body; the tree that answers it.
    DeskTree event(const json& e) {
        host->screenEvent(inst, e);
        return tree(inst);
    }
    // Home shows again later: a new instance of the body (what it keeps is in storage).
    DeskTree reopen() {
        host->closeScreen(inst);
        inst = 0;
        return render();
    }
    std::string pin(const std::string& key) {
        const uint64_t run = host->runAction("reading-desk", "itemAction", "pin", {{"key", key}, {"libraryId", 0}});
        auto r = next(PluginHostEvent::Kind::ActionResult, run);
        REQUIRE(r);
        REQUIRE(r->kind == PluginHostEvent::Kind::ActionResult);
        return r->value["text"].get<std::string>();
    }
    // The passage is picked off the render (a timer, then a quiet refresh, merged to one a
    // second): the trees that follow, until one whose quote satisfies `ok`.
    DeskTree passage(const std::function<bool(const json&)>& ok, int ms = 6000) {
        DeskTree t = render();
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!ok(t.quote) && std::chrono::steady_clock::now() < end)
            if (auto more = next(PluginHostEvent::Kind::ScreenTree, inst, 200)) t = DeskTree(more->value);
        return t;
    }
    DeskTree withPassage() { return passage([](const json& q) { return !q.is_null(); }); }
    // What the passage's tap did: the ItemOpen it asked for, answered with `error` ("" opened).
    json tapPassage(const std::string& error) {
        host->screenEvent(inst, {{"kind", "tap"}, {"id", "passage"}});
        auto open = next(PluginHostEvent::Kind::ItemOpen, inst);
        REQUIRE(open);
        host->answerReader(open->requestId, error);
        return open->value;
    }
    std::optional<json> stored(const std::string& key) {
        REQUIRE(host->waitIdle(8000));
        Db s = Db::open(fp.path("reading-desk/storage.db"));
        auto st = s.prepare("SELECT value FROM kv WHERE key = ?");
        st.bind(1, key);
        if (!st.step()) return std::nullopt;
        return json::parse(st.text(0));
    }
    DeskTree tab(const std::string& id) { return event({{"kind", "chips"}, {"id", "tabs"}, {"chip", id}, {"selected", {id}}}); }
};

}  // namespace

TEST_CASE("example: Reading Desk — Now reading is the item opened last of any kind: a card on a tall screen, a label over a row on a short one") {
    Desk d("");
    d.lib.markOpened("PAPR0001", "FAPR0001", 1000);
    d.lib.markOpened("BOOK0001", "FOOK0001", 2000);
    d.lib.markOpened("PAPR0002", "FAPR0002", 3000);  // the newest: a paper
    {
        ScopedMetrics tall(kScribe);
        DeskTree t = d.render();
        CHECK(t.at("now") == Keys{{"PAPR0002", "CONTINUE · PAPER"}});
        CHECK(t.styles["now"] == "card");
        CHECK(t.order.front() == "now");
    }
    {
        ScopedMetrics short_(kKt4);
        DeskTree t = d.render();
        CHECK(t.at("now") == Keys{{"PAPR0002", ""}});
        CHECK(t.styles["now"] == "row");
        REQUIRE_FALSE(t.labels.empty());
        CHECK(t.labels.front() == "CONTINUE · PAPER");
        CHECK(t.order.front() == "column");  // the label and its row: one block, never paged apart
    }
    // Read: the next one opened takes its place; an EPUB counts as a book whatever its type.
    ScopedMetrics tall(kScribe);
    d.lib.markRead("PAPR0002", true);
    CHECK(d.render().at("now") == Keys{{"BOOK0001", "CONTINUE · BOOK"}});
    d.lib.markOpened("EPUB0001", "FPUB0001", 4000);
    CHECK(d.render().at("now") == Keys{{"EPUB0001", "CONTINUE · BOOK"}});
}

TEST_CASE("example: Reading Desk — nothing opened: START HERE with the newest unread pin, else no Now reading; the empty texts") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    DeskTree t = d.render();
    CHECK_FALSE(t.has("now"));
    CHECK(t.tabs == std::vector<std::string>{"Books", "Papers", "Pinned"});  // no inbox: no New tab
    CHECK(t.selected == std::vector<std::string>{"books"});
    CHECK(t.at("books").empty());
    CHECK(t.tree.dump().find("Books you open show up here.") != std::string::npos);
    CHECK(d.tab("pinned").tree.dump().find("Long-press an item → More… → Pin.") != std::string::npos);

    CHECK(d.pin("PAPR0001") == "Pinned to Home");
    CHECK(d.pin("BOOK0003") == "Pinned to Home");
    CHECK(d.pin("INBX0004") == "Pinned to Home");  // read: not a place to start
    t = d.render();
    CHECK(t.at("now") == Keys{{"BOOK0003", "START HERE"}});
    CHECK(t.at("pinned") == Keys{{"PAPR0001", ""}, {"INBX0004", ""}});  // pin order, minus the hero
    CHECK(t.styles["pinned"] == "shelf");

    CHECK(d.pin("BOOK0003") == "Unpinned");
    CHECK(d.render().at("now") == Keys{{"PAPR0001", "START HERE"}});
}

TEST_CASE("example: Reading Desk — the tabs: Books by default, a chip picks one and Home keeps it; the Books shelf") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    d.lib.markOpened("BOOK0002", "FOOK0002", 1000);
    d.lib.markOpened("PAPR0001", "FAPR0001", 1500);
    d.lib.markOpened("EPUB0001", "FPUB0001", 2000);
    d.lib.markOpened("BOOK0001", "FOOK0001", 3000);  // the hero
    d.pin("BOOK0003");                                // a pinned book not started
    d.pin("BOOK0002");                                // pinned and in progress: listed once
    d.pin("PAPR0002");                                // a paper: not on the Books shelf
    DeskTree t = d.render();
    CHECK(t.selected == std::vector<std::string>{"books"});
    CHECK(t.at("books") == Keys{{"EPUB0001", ""}, {"BOOK0002", ""}, {"BOOK0003", ""}});
    CHECK(t.styles["books"] == "shelf");

    t = d.tab("papers");
    CHECK(t.selected == std::vector<std::string>{"papers"});
    CHECK_FALSE(t.has("books"));
    CHECK(t.has("papers"));
    // Kept: Home's body shows again (a new instance) on the same tab.
    t = d.reopen();
    CHECK(t.selected == std::vector<std::string>{"papers"});
    CHECK(t.has("papers"));
    // A tab that can't show (New without an inbox) falls back to Books.
    d.store("tab", "new");
    CHECK(d.render().selected == std::vector<std::string>{"books"});
}

TEST_CASE("example: Reading Desk — Papers: papers in progress, most recent first, then UP NEXT: pinned papers not started") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    d.lib.markOpened("PAPR0001", "FAPR0001", 1000);
    d.lib.markOpened("PAPR0002", "FAPR0002", 2000);
    d.lib.markOpened("INBX0001", "FNBX0001", 3000);
    d.lib.markOpened("BOOK0001", "FOOK0001", 4000);  // the hero, a book
    d.store("tab", "papers");
    DeskTree t = d.render();
    CHECK(t.at("papers") == Keys{{"INBX0001", ""}, {"PAPR0002", ""}, {"PAPR0001", ""}});
    CHECK(t.styles["papers"] == "row");
    CHECK_FALSE(t.has("next"));

    d.pin("INBX0003");
    d.pin("PAPR0002");  // in progress: listed above, not under Up next
    d.pin("BOOK0003");  // a book: not here
    d.pin("INBX0002");
    d.pin("INBX0004");  // read: not up next
    d.lib.markOpened("EPUB0001", "FPUB0001", 500);
    d.lib.markRead("EPUB0001", true);
    deskItem(d.lib, "PAPR0003", "journalArticle", "Started, then read", "2024-01-05T00:00:00Z", "", "application/pdf");
    d.lib.markOpened("PAPR0003", "FAPR0003", 600);
    d.lib.saveReading("PAPR0003", "", 0.5);
    d.lib.markRead("PAPR0003", true);
    d.pin("PAPR0003");  // started (and read): not up next
    t = d.render();
    CHECK(t.at("next") == Keys{{"INBX0003", ""}, {"INBX0002", ""}});
    const std::vector<std::string> order = {"tabs", "papers", "label:UP NEXT", "next"};
    CHECK(std::vector<std::string>(t.order.begin() + 1, t.order.begin() + 5) == order);

    // The hero, a paper now, isn't listed again.
    d.lib.markOpened("PAPR0001", "FAPR0001", 5000);
    t = d.render();
    CHECK(t.at("now") == Keys{{"PAPR0001", "CONTINUE · PAPER"}});
    CHECK(t.at("papers") == Keys{{"INBX0001", ""}, {"PAPR0002", ""}});
}

TEST_CASE("example: Reading Desk — New: the inbox's unread items, newest added first, minus the hero; settings.json's inbox still works") {
    ScopedMetrics tall(kScribe);
    Desk d(R"({"inbox":"COLLINBX"})");
    DeskTree t = d.render();
    CHECK(t.tabs == std::vector<std::string>{"Books", "Papers", "Pinned", "New 3"});
    t = d.tab("new");
    CHECK(t.at("new") == Keys{{"INBX0002", ""}, {"INBX0003", ""}, {"INBX0001", ""}});  // INBX0004 is read
    CHECK(t.styles["new"] == "row");

    d.lib.markOpened("INBX0002", "FNBX0002", 5000);  // the hero now
    d.pin("INBX0003");                               // pinned: still new
    t = d.render();
    CHECK(t.at("now") == Keys{{"INBX0002", "CONTINUE · PAPER"}});
    CHECK(t.at("new") == Keys{{"INBX0003", ""}, {"INBX0001", ""}});
    CHECK(t.tabs.back() == "New 2");

    for (const char* k : {"INBX0001", "INBX0003"}) d.lib.markRead(k, true);
    t = d.render();
    CHECK(t.tabs.back() == "New");
    CHECK(t.tree.dump().find("Nothing new in the inbox.") != std::string::npos);
}

TEST_CASE("example: Reading Desk — the settings screen picks the inbox (over settings.json), turns the passage off, and unpins") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    addHighlight(d.lib, "ANNO0001", "FOOK0001", "A passage long enough to be shown under the desk, every day of the week.",
                 "", "12", "00001|000100|00100");
    CHECK(d.render().tabs.size() == 3);

    // Plugins → Reading Desk: the settings; its Inbox row opens the picker.
    const uint64_t settings = d.host->openScreen("reading-desk", "settings");
    std::string shown = d.text(settings);
    CHECK(shown.find("Inbox collection") != std::string::npos);
    CHECK(shown.find("None: the New tab is hidden") != std::string::npos);
    d.host->screenEvent(settings, {{"kind", "select"}, {"id", "prefs"}, {"row", "inbox"}});
    auto opened = d.next(PluginHostEvent::Kind::ScreenOpen, settings);
    REQUIRE(opened);
    CHECK(opened->screenId == "inbox");
    const uint64_t picker = opened->instance;
    shown = d.text(picker);
    CHECK(shown.find("None") != std::string::npos);
    CHECK(shown.find("Reading") != std::string::npos);
    d.host->screenEvent(picker, {{"kind", "change"}, {"id", "find"}, {"text", "inb"}});
    shown = d.text(picker);
    CHECK(shown.find("Inbox") != std::string::npos);
    CHECK(shown.find("Reading") == std::string::npos);  // the search narrows the list
    d.host->screenEvent(picker, {{"kind", "select"}, {"id", "collections"}, {"row", "0:COLLINBX"}});
    REQUIRE(d.next(PluginHostEvent::Kind::ScreenClose, picker));
    CHECK(d.render().tabs.back() == "New 3");

    // The passage: on by default; the settings' row turns it off.
    CHECK_FALSE(d.withPassage().quote.is_null());
    d.host->screenEvent(settings, {{"kind", "select"}, {"id", "prefs"}, {"row", "passage"}});
    shown = d.text(settings);
    CHECK(shown.find("A passage each day Off") != std::string::npos);
    CHECK(d.render().quote.is_null());

    // Pins: a row asks, Unpin unpins.
    d.pin("PAPR0001");
    d.pin("PAPR0002");
    d.host->rerender(settings);
    shown = d.text(settings);
    CHECK(shown.find("PINNED (2)") != std::string::npos);
    d.host->screenEvent(settings, {{"kind", "select"}, {"id", "pins"}, {"row", "0:PAPR0001"}});
    shown = d.text(settings);
    CHECK(shown.find("Unpin “A Paper”?") != std::string::npos);
    d.host->screenEvent(settings, {{"kind", "tap"}, {"id", "unpin"}});
    shown = d.text(settings);
    CHECK(shown.find("PINNED (1)") != std::string::npos);
    CHECK(d.render().at("now") == Keys{{"PAPR0002", "START HERE"}});  // the one pin left

    // "None" overrides settings.json's inbox.
    Desk e(R"({"inbox":"COLLINBX"})");
    CHECK(e.render().tabs.back() == "New 3");
    const uint64_t p2 = e.host->openScreen("reading-desk", "inbox");
    REQUIRE(e.next(PluginHostEvent::Kind::ScreenTree, p2));
    e.host->screenEvent(p2, {{"kind", "select"}, {"id", "collections"}, {"row", "none"}});
    REQUIRE(e.next(PluginHostEvent::Kind::ScreenClose, p2));
    CHECK(e.render().tabs.back() == "Pinned");
}

TEST_CASE("example: Reading Desk — the passage: a highlight of 40 to 400 characters, the same all day; a tap opens the reader at it") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    addHighlight(d.lib, "ANNOSHRT", "FOOK0001", "Too short to show.", "", "3", "00001|000010|00010");
    addHighlight(d.lib, "ANNOLONG", "FOOK0001", "  A passage long enough to be shown under the desk, every day of the week.  ",
                 "", "12", "00001|000100|00100");
    addHighlight(d.lib, "ANNOPAPR", "FAPR0001", "Another passage, from a paper this time, also long enough to be picked.",
                 "", "", "00001|000200|00200");
    DeskTree t = d.withPassage();
    REQUIRE_FALSE(t.quote.is_null());
    const std::string text = t.quote["text"];
    const bool book = text.rfind("A passage long enough", 0) == 0;
    CHECK((book || text.rfind("Another passage", 0) == 0));
    CHECK(t.quote["source"] == (book ? "A Book · p. 12" : "A Paper"));  // trimmed; no page: the title alone
    CHECK(t.order.back() == "quote");
    // Stable: the same passage on every render that day, kept in storage.
    for (int i = 0; i < 3; ++i) CHECK(d.render().quote["text"] == text);
    CHECK(d.reopen().quote["text"] == text);

    // A tap opens the item at that highlight.
    d.host->screenEvent(d.inst, {{"kind", "tap"}, {"id", "passage"}});
    auto open = d.next(PluginHostEvent::Kind::ItemOpen, d.inst);
    REQUIRE(open);
    CHECK(open->value["key"] == (book ? "BOOK0001" : "PAPR0001"));
    CHECK(open->value["annotation"] == (book ? "ANNOLONG" : "ANNOPAPR"));
    d.host->answerReader(open->requestId, "");
    d.tree(d.inst);

    // A new day picks again; within the day the kept pick is shown.
    json kept = {{"day", localToday()},
                 {"pick", {{"key", "ANNOPAPR"}, {"itemKey", "PAPR0001"}, {"library", 0}, {"text", "Kept for today."}, {"source", "A Paper"}}}};
    d.store("passage", kept);
    CHECK(d.render().quote["text"] == "Kept for today.");
    // Yesterday's pick shows until today's is picked, off the render.
    kept["day"] = "2000-01-01";
    d.store("passage", kept);
    t = d.passage([](const json& q) { return !q.is_null() && q["text"] != "Kept for today."; });
    CHECK(t.quote["text"] != "Kept for today.");
}

TEST_CASE("example: Reading Desk — a tapped passage that can't open: the item gone re-picks, the highlight gone opens the item, Cancelled is quiet") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    addHighlight(d.lib, "ANNOLONG", "FOOK0001", "A passage long enough to be shown under the desk, every day of the week.",
                 "", "12", "00001|000100|00100");
    REQUIRE_FALSE(d.withPassage().quote.is_null());

    // Cancelled (the download): nothing more, no error, the pick kept.
    CHECK(d.tapPassage("Cancelled")["annotation"] == "ANNOLONG");
    d.tree(d.inst);
    CHECK(d.stored("passage").has_value());

    // Today's pick, as kept (its text marks it): a pick that can't open is dropped and picked again.
    const json kept = {{"day", localToday()},
                       {"pick", {{"key", "ANNOLONG"}, {"itemKey", "BOOK0001"}, {"library", 0}, {"text", "Kept for today."}, {"source", "A Book"}}}};
    auto repicked = [&] { return d.passage([](const json& q) { return !q.is_null() && q["text"] != "Kept for today."; }); };

    // The highlight was deleted: the item opens where it was left, and the pick is dropped.
    d.store("passage", kept);
    CHECK(d.render().quote["text"] == "Kept for today.");
    CHECK(d.tapPassage("Annotation not found")["annotation"] == "ANNOLONG");
    auto again = d.next(PluginHostEvent::Kind::ItemOpen, d.inst);
    REQUIRE(again);
    CHECK(again->value["key"] == "BOOK0001");
    CHECK_FALSE(again->value.contains("annotation"));
    d.host->answerReader(again->requestId, "");
    d.tree(d.inst);
    CHECK(repicked().quote["text"] != "Kept for today.");

    // The item is gone (trashed): no error pop-up, and the pick is dropped too.
    d.store("passage", kept);
    CHECK(d.render().quote["text"] == "Kept for today.");
    d.tapPassage("Item not found");
    d.tree(d.inst);
    CHECK(repicked().quote["text"] != "Kept for today.");
    for (const auto& e : d.got) CHECK(e.kind != PluginHostEvent::Kind::ScreenActionError);
}

TEST_CASE("example: Reading Desk — a big inbox stays inside the app's 1 s render: the walk stops at 100, New 99+") {
    ScopedMetrics short_(kPw5);
    Desk d(R"({"inbox":"COLLINBX"})", 1000);
    {
        auto tx = d.lib.begin();
        for (int i = 0; i < 1500; ++i) {
            char key[16], added[32];
            std::snprintf(key, sizeof key, "BIG%05d", i);
            std::snprintf(added, sizeof added, "2025-%02d-%02dT%02d:00:00Z", 1 + i / 500, 1 + i % 500 / 20, i % 20);  // rising
            deskItem(d.lib, key, "journalArticle", std::string("Inbox paper ") + key, added, "COLLINBX", "application/pdf");
        }
        tx.commit();
    }
    for (int i = 0; i < 40; ++i) d.pin("BIG" + std::string(i < 10 ? "0000" : "000") + std::to_string(i));
    d.store("tab", "new");
    for (int round = 0; round < 3; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        DeskTree t = d.render();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE("render with a 1,500-item inbox: " << ms << " ms");
        CHECK(ms < 1000);
        CHECK(t.tabs.back() == "New 99+");
        CHECK(t.at("new").size() == 100);
        CHECK(t.at("new").front().first == "BIG01499");  // newest added first
    }
    for (const auto& e : d.got) CHECK(e.kind != PluginHostEvent::Kind::ScreenError);
}

TEST_CASE("example: Reading Desk — the passage never costs a page: left out where the content would page because of it") {
    Desk d(R"({"inbox":"COLLINBX"})");
    addHighlight(d.lib, "ANNOLONG", "FOOK0001", "A passage long enough to be shown under the desk, every day of the week.",
                 "", "12", "00001|000100|00100");
    d.store("tab", "new");
    {
        ScopedMetrics short_(kKt4);  // nothing opened: the tabs, 3 rows and the passage fit
        DeskTree t = d.withPassage();
        CHECK(t.at("new").size() == 3);
        CHECK_FALSE(t.quote.is_null());
    }
    d.lib.markOpened("BOOK0001", "FOOK0001", 1000);
    {
        ScopedMetrics short_(kKt4);  // with Now reading the rows page anyway; the passage would make it 3 pages
        CHECK(d.render().quote.is_null());
    }
    ScopedMetrics tall(kScribe);  // room for all of it
    DeskTree t = d.withPassage();
    CHECK(t.at("new").size() == 3);
    CHECK_FALSE(t.quote.is_null());
}
