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
              const std::string& added, const std::string& collection, const std::string& fileType,
              const std::vector<std::pair<std::string, std::string>>& authors = {}, const std::string& year = "") {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = type;
    r.title = title;
    r.dateAdded = added;
    r.dateModified = added;
    r.json = "{}";
    for (const auto& [first, last] : authors) r.creators.push_back({first, last, "", "author"});
    r.date = r.year = year;
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

constexpr int64_t kDayMs = 86400000;
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
// An ISO 8601 instant (UTC) `ago` milliseconds before now, as Zotero writes dateAdded.
std::string isoAgo(int64_t ago) {
    const std::time_t t = static_cast<std::time_t>((nowMs() - ago) / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

using Strings = std::vector<std::string>;
using Sides = std::vector<std::pair<std::string, std::string>>;  // {kicker, key}

// An item piece (zk.ui.item) of a tree: its key, its texts in order, its bar, whether it has a cover.
struct Piece {
    std::string key;
    Strings texts;
    std::optional<double> progress;
    bool cover = false;
    const Block* block = nullptr;
};

// A part of a tree (a band of Home's body, or a whole screen), read the way a test needs it.
struct Part {
    std::string kind;  // on Home: "pair", "pinned", "inbox", "link" (Your desk ›), "quote", "text"
    Strings labels;    // caps texts: section labels and kickers, in order
    std::vector<std::pair<std::string, std::string>> links;  // a tappable text's or box's id, its text
    std::string selected;  // the tappable box filled with ink (a switch's selected segment)
    std::vector<Piece> items;
    Strings texts;  // other texts, outside items and links
    const Block* grid = nullptr;
    const Block* block = nullptr;
    std::shared_ptr<const Block> tree;  // a whole screen's: what the pointers above point into

    std::string link(const std::string& id) const {
        for (const auto& [i, t] : links)
            if (i == id) return t;
        return "";
    }
    Strings keys() const {
        Strings out;
        for (const auto& p : items) out.push_back(p.key);
        return out;
    }
};

void piece(const Block& b, Piece& p) {
    if (b.kind == Block::Kind::Text) p.texts.push_back(plainText(b));
    if (b.kind == Block::Kind::Progress) p.progress = b.fraction;
    if (b.kind == Block::Kind::Cover) p.cover = true;
    for (const auto& c : b.children) piece(c, p);
}

std::string textsOf(const Block& b) {
    std::string out = b.kind == Block::Kind::Text ? plainText(b) : "";
    for (const auto& c : b.children) out += textsOf(c);
    return out;
}

void gather(const Block& b, Part& p) {
    using K = Block::Kind;
    if (b.kind == K::Item) {
        Piece it;
        it.key = b.items.empty() ? "" : b.items[0].key;
        it.block = &b;
        for (const auto& c : b.children) piece(c, it);
        p.items.push_back(std::move(it));
        return;
    }
    if (b.kind == K::Box && !b.id.empty()) {
        p.links.emplace_back(b.id, textsOf(b));
        if (b.fill == Block::Fill::Ink) p.selected = b.id;
        return;
    }
    if (b.kind == K::Grid) p.grid = &b;
    if (b.kind == K::Text) {
        if (!b.id.empty()) p.links.emplace_back(b.id, plainText(b));
        else if (b.caps) p.labels.push_back(plainText(b));
        else p.texts.push_back(plainText(b));
    }
    for (const auto& c : b.children) gather(c, p);
}

const Block* firstOf(const Block& b, Block::Kind kind) {
    if (b.kind == kind) return &b;
    for (const auto& c : b.children)
        if (const Block* f = firstOf(c, kind)) return f;
    return nullptr;
}

// Home's body: its bands, top to bottom.
struct HomeTree {
    std::shared_ptr<const Block> root;
    std::vector<Part> parts;

    explicit HomeTree(std::shared_ptr<const Block> r) : root(std::move(r)) {
        for (const auto& c : root->children) {
            Part p;
            p.block = &c;
            gather(c, p);
            if (c.kind == Block::Kind::Quote) p.kind = "quote";
            else if (!p.link("all-pinned").empty()) p.kind = "pinned";
            else if (!p.link("all-inbox").empty()) p.kind = "inbox";
            else if (!p.link("your-desk").empty()) p.kind = "link";
            else if (!p.items.empty()) p.kind = "pair";
            else p.kind = "text";
            parts.push_back(std::move(p));
        }
    }
    Strings order() const {
        Strings out;
        for (const auto& p : parts) out.push_back(p.kind);
        return out;
    }
    bool has(const std::string& kind) const {
        return std::any_of(parts.begin(), parts.end(), [&](const Part& p) { return p.kind == kind; });
    }
    Part at(const std::string& kind) const {
        for (const auto& p : parts)
            if (p.kind == kind) return p;
        return {};
    }
    // The pair's sides, left to right: {kicker, key}.
    Sides pair() const {
        const Part p = at("pair");
        Sides out;
        for (size_t i = 0; i < p.items.size(); ++i) out.emplace_back(i < p.labels.size() ? p.labels[i] : "", p.items[i].key);
        return out;
    }
    const Block* quote() const {
        for (const auto& p : parts)
            if (p.kind == "quote") return p.block;
        return nullptr;
    }
    std::string quoteText() const { return quote() ? quote()->text : ""; }
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
        deskItem(lib, "BOOK0001", "book", "A Book", "2024-01-01T00:00:00Z", "", "application/pdf", {{"Jane", "Austen"}}, "1813");
        deskItem(lib, "BOOK0002", "bookSection", "A Chapter", "2024-01-01T01:00:00Z", "", "application/pdf");
        deskItem(lib, "BOOK0003", "book", "A Pinned Book", "2024-01-01T02:00:00Z", "", "application/pdf");
        deskItem(lib, "EPUB0001", "journalArticle", "An EPUB", "2024-01-02T00:00:00Z", "", "application/epub+zip");
        deskItem(lib, "PAPR0001", "journalArticle", "A Paper", "2024-01-03T00:00:00Z", "", "application/pdf");
        deskItem(lib, "PAPR0002", "journalArticle", "Another Paper", "2024-01-04T00:00:00Z", "", "application/pdf",
                 {{"Ashish", "Vaswani"}, {"Noam", "Shazeer"}, {"Niki", "Parmar"}, {"Jakob", "Uszkoreit"}, {"Llion", "Jones"}}, "2017");
        deskItem(lib, "INBX0001", "journalArticle", "Old", "2024-02-01T12:00:00Z", "COLLINBX", "application/pdf");
        deskItem(lib, "INBX0002", "journalArticle", "Newest", "2024-03-01T12:00:00Z", "COLLINBX", "application/pdf");
        deskItem(lib, "INBX0003", "journalArticle", "Middle", "2024-02-15T12:00:00Z", "COLLINBX", "application/pdf");
        deskItem(lib, "INBX0004", "journalArticle", "Done", "2024-04-01T12:00:00Z", "COLLINBX", "application/pdf");
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
    HomeTree render() {
        if (!inst) inst = host->openScreen("reading-desk", "desk", nullptr, TreeUse::HomeBody);
        else host->rerender(inst);
        return tree(inst);
    }
    // The latest tree of `instance` once the plugin is idle (a pin's zk.ui.refresh renders too).
    std::shared_ptr<const Block> latest(uint64_t instance) {
        auto t = next(PluginHostEvent::Kind::ScreenTree, instance);
        CAPTURE(fp.log("reading-desk"));
        REQUIRE(t);
        REQUIRE(host->waitIdle(8000));
        while (auto more = next(PluginHostEvent::Kind::ScreenTree, instance, 50)) t = more;
        REQUIRE(t->tree);
        return t->tree;
    }
    HomeTree tree(uint64_t instance) { return HomeTree(latest(instance)); }
    std::string text(uint64_t instance) { return allText(*latest(instance)); }
    // What the user did on Home's body; the tree that answers it.
    HomeTree event(const json& e) {
        host->screenEvent(inst, e);
        return tree(inst);
    }
    // Home shows again later: a new instance of the body (what it keeps is in storage).
    HomeTree reopen() {
        host->closeScreen(inst);
        inst = 0;
        return render();
    }
    // The screen Home's body opened on a tap (zk.ui.open): its id and params.
    std::pair<std::string, json> tapOpens(const std::string& id) {
        host->screenEvent(inst, {{"kind", "tap"}, {"id", id}});
        auto o = next(PluginHostEvent::Kind::ScreenOpen, inst);
        REQUIRE(o);
        tree(inst);
        return {o->screenId, o->value};
    }
    // Your desk, opened with `params`: the whole screen as one part.
    uint64_t all = 0;
    Part allPart(std::shared_ptr<const Block> b) {
        Part p;
        p.tree = b;
        gather(*b, p);
        p.texts.push_back(allText(*b));
        return p;
    }
    Part openAll(const json& params) {
        all = host->openScreen("reading-desk", "all", params);
        return allPart(latest(all));
    }
    Part allEvent(const json& e) {
        host->screenEvent(all, e);
        return allPart(latest(all));
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
    HomeTree passage(const std::function<bool(const Block*)>& ok, int ms = 6000) {
        HomeTree t = render();
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!ok(t.quote()) && std::chrono::steady_clock::now() < end)
            if (auto more = next(PluginHostEvent::Kind::ScreenTree, inst, 200)) t = HomeTree(more->tree);
        return t;
    }
    HomeTree withPassage() { return passage([](const Block* q) { return q != nullptr; }); }
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
};

const char* const kLongPassage = "A passage long enough to be shown under the desk, every day of the week.";

}  // namespace

TEST_CASE("example: Reading Desk — the pair: the newest opened unread book and paper side by side, BOOK and PAPER, a bar and when") {
    ScopedMetrics pw5(kPw5);
    Desk d("");
    const int64_t now = nowMs();
    d.lib.markOpened("PAPR0001", "FAPR0001", now - 3 * kDayMs);
    d.lib.markOpened("BOOK0001", "FOOK0001", now - kDayMs);
    d.lib.markOpened("PAPR0002", "FAPR0002", now);
    d.lib.saveReading("BOOK0001", "", 0.13);
    d.lib.saveReading("PAPR0002", "", 0.67);
    HomeTree t = d.render();
    CHECK(t.root->fit == Block::Fit::Trim);  // never pages
    CHECK(t.order() == Strings{"pair", "link"});  // no All › on Home: Your desk › instead
    CHECK(t.at("link").link("your-desk") == "Your desk ›");
    CHECK(t.pair() == Sides{{"Book", "BOOK0001"}, {"Paper", "PAPR0002"}});
    const Part pair = t.at("pair");
    REQUIRE(pair.items.size() == 2);
    CHECK(pair.items[0].cover);
    CHECK(pair.items[0].progress == 0.13);
    CHECK(pair.items[0].texts == Strings{"A Book", "Jane Austen", "13% · yesterday"});
    CHECK(pair.items[1].texts == Strings{"Another Paper", "Vaswani et al.", "67% · today"});
    const Block* twoUp = firstOf(*pair.items[0].block, Block::Kind::Cover);
    REQUIRE(twoUp);

    // Read: the next paper opened takes its place; an EPUB is a book whatever its type.
    d.lib.markRead("PAPR0002", true);
    d.lib.markOpened("EPUB0001", "FPUB0001", now + 1000);
    t = d.render();
    CHECK(t.pair() == Sides{{"Book", "EPUB0001"}, {"Paper", "PAPR0001"}});

    // A kind with nothing at all: the other side alone, full width (a narrower share for its cover).
    d.lib.markRead("PAPR0001", true);
    t = d.render();
    CHECK(t.pair() == Sides{{"Book", "EPUB0001"}});
    REQUIRE(t.at("pair").items.size() == 1);
    const Block* alone = firstOf(*t.at("pair").items[0].block, Block::Kind::Cover);
    REQUIRE(alone);
    CHECK(alone->widthPct < twoUp->widthPct);
}

TEST_CASE("example: Reading Desk — nothing opened of a kind: its first pin never opened and not read, UP NEXT, Not started; Pinned is a row of covers minus the pair") {
    ScopedMetrics pw5(kPw5);
    Desk d("");
    d.lib.markOpened("BOOK0001", "FOOK0001", nowMs());
    deskItem(d.lib, "PAPR0003", "journalArticle", "Started, then read", "2024-01-05T00:00:00Z", "", "application/pdf");
    d.lib.markOpened("PAPR0003", "FAPR0003", 600);
    d.lib.saveReading("PAPR0003", "", 0.5);
    d.lib.markRead("PAPR0003", true);
    CHECK(d.pin("PAPR0003") == "Pinned to Home");  // opened (and read): not up next
    d.pin("INBX0004");                             // read: not up next
    d.pin("BOOK0001");                             // the pair's book: not in the row
    d.pin("PAPR0001");                             // the first paper never opened and not read: up next
    d.pin("PAPR0002");
    d.pin("BOOK0003");
    HomeTree t = d.render();
    CHECK(t.order() == Strings{"pair", "pinned"});
    CHECK(t.pair() == Sides{{"Book", "BOOK0001"}, {"Up next", "PAPR0001"}});
    const Part pair = t.at("pair");
    REQUIRE(pair.items.size() == 2);
    CHECK_FALSE(pair.items[1].progress);
    CHECK(pair.items[1].texts.back() == "Not started");

    // Pinned: a heading with All n › (n: every pin), one row of covers in pin order minus the pair,
    // no captions, a bar only once opened.
    const Part pinned = t.at("pinned");
    CHECK(pinned.labels == Strings{"Pinned"});
    CHECK(pinned.link("all-pinned") == "All 6 ›");
    REQUIRE(pinned.grid);
    CHECK(pinned.grid->maxRows == 1);
    REQUIRE(pinned.keys() == Strings{"PAPR0003", "INBX0004", "PAPR0002", "BOOK0003"});
    for (const auto& p : pinned.items) {
        CHECK(p.cover);
        CHECK(p.texts.empty());
    }
    CHECK(pinned.items[0].progress == 0.5);
    CHECK_FALSE(pinned.items[1].progress);

    // Nothing opened at all: both sides are pins, the book's first too.
    d.lib.markRead("BOOK0001", true);
    CHECK(d.render().pair() == Sides{{"Up next", "BOOK0003"}, {"Up next", "PAPR0001"}});
    CHECK(d.pin("BOOK0003") == "Unpinned");
    CHECK(d.render().pair() == Sides{{"Up next", "PAPR0001"}});

    // All › opens Your desk on Pinned.
    d.render();
    CHECK(d.tapOpens("all-pinned") == std::pair<std::string, json>{"all", {{"tab", "pinned"}}});
}

TEST_CASE("example: Reading Desk — the pinned row holds as many covers as fit whole: 5 on a Paperwhite, 4 on a basic Kindle, 7 on a Scribe") {
    Desk d("");
    d.lib.markOpened("BOOK0001", "FOOK0001", nowMs());
    d.lib.markOpened("PAPR0001", "FAPR0001", nowMs() - 1000);
    for (int i = 0; i < 10; ++i) {
        const std::string key = "PINX000" + std::to_string(i);
        deskItem(d.lib, key, "book", "Pinned book " + std::to_string(i), "2024-01-05T00:00:00Z", "", "application/pdf");
        d.pin(key);
    }
    for (const auto& [dev, n] : {std::pair{&kPw5, 5}, std::pair{&kKt4, 4}, std::pair{&kScribe, 7}}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        const HomeTree t = d.render();
        CHECK(t.at("pinned").keys().size() == static_cast<size_t>(n));
    }
}

TEST_CASE("example: Reading Desk — first use: the hint and the passage; nothing else") {
    ScopedMetrics pw5(kPw5);
    Desk d("");
    addHighlight(d.lib, "ANNOLONG", "FOOK0001", kLongPassage, "", "12", "00001|000100|00100");
    HomeTree t = d.withPassage();
    CHECK(t.order() == Strings{"text", "link", "quote"});
    REQUIRE_FALSE(t.at("text").texts.empty());
    CHECK(t.at("text").texts.front() ==
          "Open a book or a paper and it will wait for you here. Pin things from an item's menu: long-press → More… → Pin to Home.");
    d.lib.markOpened("PAPR0001", "FAPR0001", nowMs());
    t = d.render();
    CHECK(t.order() == Strings{"pair", "link", "quote"});  // no inbox: the passage takes its band
    CHECK(d.tapOpens("your-desk") == std::pair<std::string, json>{"all", {{"tab", "reading"}}});
}

TEST_CASE("example: Reading Desk — Inbox: the newest unread as lines, minus the pair, bylines and when added; up to 5 and the passage on a Scribe, else 2") {
    Desk d(R"({"inbox":"COLLINBX"})");
    deskItem(d.lib, "INBX0005", "journalArticle", "Deep Residual Learning for Image Recognition", isoAgo(0), "COLLINBX",
             "application/pdf", {{"Kaiming", "He"}, {"Xiangyu", "Zhang"}, {"Shaoqing", "Ren"}, {"Jian", "Sun"}}, "2016");
    deskItem(d.lib, "INBX0006", "journalArticle", "Mastering the game of Go", isoAgo(kDayMs), "COLLINBX", "application/pdf",
             {{"David", "Silver"}, {"Julian", "Schrittwieser"}, {"Karen", "Simonyan"}, {"Ioannis", "Antonoglou"}, {"Aja", "Huang"}},
             "2017");
    deskItem(d.lib, "INBX0007", "journalArticle", "Three days ago", isoAgo(3 * kDayMs), "COLLINBX", "application/pdf");
    addHighlight(d.lib, "ANNOLONG", "FOOK0001", kLongPassage, "", "12", "00001|000100|00100");
    {
        ScopedMetrics scribe(kScribe);
        HomeTree t = d.withPassage();
        CHECK(t.order() == Strings{"inbox", "quote"});
        const Part inbox = t.at("inbox");
        CHECK(inbox.labels == Strings{"Inbox · 6 new"});
        CHECK(inbox.link("all-inbox") == "All ›");
        REQUIRE(inbox.keys() == Strings{"INBX0005", "INBX0006", "INBX0007", "INBX0002", "INBX0003"});
        CHECK(inbox.items[0].texts == Strings{"Deep Residual Learning for Image Recognition", "He, Zhang, Ren, Sun · 2016", "today"});
        CHECK(inbox.items[1].texts == Strings{"Mastering the game of Go", "Silver et al. · 2017", "yesterday"});
        static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        const std::time_t t3 = static_cast<std::time_t>((nowMs() - 3 * kDayMs) / 1000);
        std::tm tm{};
        localtime_r(&t3, &tm);
        CHECK(inbox.items[2].texts == Strings{"Three days ago", kDays[tm.tm_wday]});  // no authors, no year: no byline
    }
    ScopedMetrics pw5(kPw5);
    HomeTree t = d.render();
    CHECK(t.order() == Strings{"inbox"});  // with an inbox, the passage only on a Scribe
    CHECK(t.at("inbox").keys() == Strings{"INBX0005", "INBX0006"});

    // The pair's item is neither a line nor counted; older ones say their date.
    d.lib.markOpened("INBX0005", "FNBX0005", nowMs());
    d.lib.markOpened("INBX0006", "FNBX0006", nowMs() - 1000);
    for (const char* k : {"INBX0007", "INBX0003", "INBX0001"}) d.lib.markRead(k, true);
    t = d.render();
    CHECK(t.pair() == Sides{{"Paper", "INBX0005"}});
    CHECK(t.at("inbox").labels == Strings{"Inbox · 3 new"});  // every unread item, the pair's too
    REQUIRE(t.at("inbox").keys() == Strings{"INBX0006", "INBX0002"});
    CHECK(t.at("inbox").items[1].texts.back().find("2024") != std::string::npos);  // "Mar 1, 2024"
    CHECK(d.tapOpens("all-inbox") == std::pair<std::string, json>{"all", {{"tab", "inbox"}}});

    // Within the year, older than a week: "Nov 12" (the year only for another year's).
    deskItem(d.lib, "INBX0008", "journalArticle", "Ten days ago", isoAgo(10 * kDayMs), "COLLINBX", "application/pdf");
    t = d.render();
    REQUIRE(t.at("inbox").keys() == Strings{"INBX0006", "INBX0008"});
    {
        static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const std::time_t now = std::time(nullptr), then = static_cast<std::time_t>((nowMs() - 10 * kDayMs) / 1000);
        std::tm n{}, a{};
        localtime_r(&now, &n);
        localtime_r(&then, &a);
        std::string want = std::string(kMonths[a.tm_mon]) + " " + std::to_string(a.tm_mday);
        if (a.tm_year != n.tm_year) want += ", " + std::to_string(1900 + a.tm_year);
        CHECK(t.at("inbox").items[1].texts.back() == want);
    }
    d.lib.markRead("INBX0008", true);

    // Nothing unread in it: the passage takes the band.
    for (const char* k : {"INBX0006", "INBX0002"}) d.lib.markRead(k, true);
    t = d.render();
    CHECK(t.order() == Strings{"pair", "link", "quote"});
}

TEST_CASE("example: Reading Desk — Your desk: a switch Reading · Pinned · Inbox n on the tab asked for, a grid of covers with title and author · 42%") {
    ScopedMetrics pw5(kPw5);
    Desk d(R"({"inbox":"COLLINBX"})");
    d.lib.markOpened("PAPR0001", "FAPR0001", 1000);
    d.lib.markOpened("BOOK0001", "FOOK0001", 2000);
    d.lib.saveReading("BOOK0001", "", 0.42);
    d.lib.markOpened("EPUB0001", "FPUB0001", 3000);
    d.lib.markRead("EPUB0001", true);
    d.pin("BOOK0003");
    d.pin("BOOK0001");
    d.pin("INBX0004");

    Part a = d.openAll({{"tab", "pinned"}});
    CHECK(a.links == std::vector<std::pair<std::string, std::string>>{
                         {"tab-reading", "Reading"}, {"tab-pinned", "Pinned"}, {"tab-inbox", "Inbox 3"}});
    CHECK(a.selected == "tab-pinned");
    REQUIRE(a.keys() == Strings{"BOOK0003", "BOOK0001", "INBX0004"});
    REQUIRE(a.grid);
    CHECK(a.grid->maxRows == 0);  // it pages
    CHECK(a.items[0].texts == Strings{"A Pinned Book"});  // never opened, no author: the title alone
    CHECK_FALSE(a.items[0].progress);
    CHECK(a.items[1].texts == Strings{"A Book", "Austen · 42%"});
    CHECK(a.items[1].progress == 0.42);
    CHECK(a.items[2].texts == Strings{"Done", "Read"});
    for (const auto& p : a.items) {
        CHECK(p.cover);
        CHECK(firstOf(*p.block, Block::Kind::Box));  // the bar's slot, reserved with or without a bar
    }

    // Reading: opened and not read, books and papers together, most recent first.
    a = d.allEvent({{"kind", "tap"}, {"id", "tab-reading"}});
    CHECK(a.selected == "tab-reading");
    CHECK(a.keys() == Strings{"BOOK0001", "PAPR0001"});
    // Inbox: unread, newest added first.
    a = d.allEvent({{"kind", "tap"}, {"id", "tab-inbox"}});
    CHECK(a.selected == "tab-inbox");
    CHECK(a.keys() == Strings{"INBX0002", "INBX0003", "INBX0001"});
    CHECK_FALSE(d.stored("tab"));  // the tab is the screen's, never stored

    // No inbox: no Inbox segment (asked for, Reading shows); no params: Reading; the empty texts.
    Desk e("");
    a = e.openAll({{"tab", "inbox"}});
    CHECK(a.links.size() == 2);
    CHECK(a.selected == "tab-reading");
    CHECK(a.texts.back().find("Books and papers you open show up here.") != std::string::npos);
    a = e.openAll(nullptr);
    CHECK(a.selected == "tab-reading");
    a = e.allEvent({{"kind", "tap"}, {"id", "tab-pinned"}});
    CHECK(a.texts.back().find("Nothing pinned yet. Long-press an item → More… → Pin to Home / Unpin.") != std::string::npos);
}

TEST_CASE("example: Reading Desk — the settings screen picks the inbox (over settings.json), turns the passage off, and unpins") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    addHighlight(d.lib, "ANNO0001", "FOOK0001", kLongPassage, "", "12", "00001|000100|00100");
    d.lib.markOpened("PAPR0001", "FAPR0001", nowMs());
    CHECK_FALSE(d.render().has("inbox"));

    // Plugins → Reading Desk: the settings; its Inbox row opens the picker.
    const uint64_t settings = d.host->openScreen("reading-desk", "settings");
    std::string shown = d.text(settings);
    CHECK(shown.find("Inbox collection") != std::string::npos);
    CHECK(shown.find("None: no inbox on Home") != std::string::npos);
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
    CHECK(d.render().at("inbox").labels == Strings{"Inbox · 3 new"});

    // The passage: on by default (under the inbox on a Scribe); the settings' row turns it off.
    CHECK(d.withPassage().quote());
    d.host->screenEvent(settings, {{"kind", "select"}, {"id", "prefs"}, {"row", "passage"}});
    shown = d.text(settings);
    CHECK(shown.find("A passage each day Off") != std::string::npos);
    CHECK_FALSE(d.render().quote());

    // Pins: a row asks, Unpin unpins.
    d.pin("PAPR0002");
    d.pin("INBX0004");  // a paper, read: in the row (the pair's paper is the one opened)
    d.host->rerender(settings);
    shown = d.text(settings);
    CHECK(shown.find("PINNED (2)") != std::string::npos);
    d.host->screenEvent(settings, {{"kind", "select"}, {"id", "pins"}, {"row", "0:PAPR0002"}});
    shown = d.text(settings);
    CHECK(shown.find("Unpin “Another Paper”?") != std::string::npos);
    d.host->screenEvent(settings, {{"kind", "tap"}, {"id", "unpin"}});
    shown = d.text(settings);
    CHECK(shown.find("PINNED (1)") != std::string::npos);
    CHECK(d.render().at("pinned").link("all-pinned") == "All 1 ›");

    // "None" overrides settings.json's inbox.
    Desk e(R"({"inbox":"COLLINBX"})");
    CHECK(e.render().has("inbox"));
    const uint64_t p2 = e.host->openScreen("reading-desk", "inbox");
    REQUIRE(e.next(PluginHostEvent::Kind::ScreenTree, p2));
    e.host->screenEvent(p2, {{"kind", "select"}, {"id", "collections"}, {"row", "none"}});
    REQUIRE(e.next(PluginHostEvent::Kind::ScreenClose, p2));
    CHECK_FALSE(e.render().has("inbox"));
}

TEST_CASE("example: Reading Desk — the passage: a highlight of 40 to 400 characters, the same all day; a tap opens the reader at it") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    addHighlight(d.lib, "ANNOSHRT", "FOOK0001", "Too short to show.", "", "3", "00001|000010|00010");
    addHighlight(d.lib, "ANNOLONG", "FOOK0001", std::string("  ") + kLongPassage + "  ", "", "12", "00001|000100|00100");
    addHighlight(d.lib, "ANNOPAPR", "FAPR0001", "Another passage, from a paper this time, also long enough to be picked.",
                 "", "", "00001|000200|00200");
    HomeTree t = d.withPassage();
    REQUIRE(t.quote());
    const std::string text = t.quoteText();
    const bool book = text.rfind("A passage long enough", 0) == 0;
    CHECK((book || text.rfind("Another passage", 0) == 0));
    CHECK(t.quote()->meta == (book ? "A Book · p. 12" : "A Paper"));  // trimmed; no page: the title alone
    CHECK(t.order().back() == "quote");
    // Stable: the same passage on every render that day, kept in storage.
    for (int i = 0; i < 3; ++i) CHECK(d.render().quoteText() == text);
    CHECK(d.reopen().quoteText() == text);

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
    CHECK(d.render().quoteText() == "Kept for today.");
    // Yesterday's pick shows until today's is picked, off the render.
    kept["day"] = "2000-01-01";
    d.store("passage", kept);
    t = d.passage([](const Block* q) { return q && q->text != "Kept for today."; });
    CHECK(t.quoteText() != "Kept for today.");
}

TEST_CASE("example: Reading Desk — a tapped passage that can't open: the item gone re-picks, the highlight gone opens the item, Cancelled is quiet") {
    ScopedMetrics tall(kScribe);
    Desk d("");
    addHighlight(d.lib, "ANNOLONG", "FOOK0001", kLongPassage, "", "12", "00001|000100|00100");
    REQUIRE(d.withPassage().quote());

    // Cancelled (the download): nothing more, no error, the pick kept.
    CHECK(d.tapPassage("Cancelled")["annotation"] == "ANNOLONG");
    d.tree(d.inst);
    CHECK(d.stored("passage").has_value());

    // Today's pick, as kept (its text marks it): a pick that can't open is dropped and picked again.
    const json kept = {{"day", localToday()},
                       {"pick", {{"key", "ANNOLONG"}, {"itemKey", "BOOK0001"}, {"library", 0}, {"text", "Kept for today."}, {"source", "A Book"}}}};
    auto repicked = [&] { return d.passage([](const Block* q) { return q && q->text != "Kept for today."; }); };

    // The highlight was deleted: the item opens where it was left, and the pick is dropped.
    d.store("passage", kept);
    CHECK(d.render().quoteText() == "Kept for today.");
    CHECK(d.tapPassage("Annotation not found")["annotation"] == "ANNOLONG");
    auto again = d.next(PluginHostEvent::Kind::ItemOpen, d.inst);
    REQUIRE(again);
    CHECK(again->value["key"] == "BOOK0001");
    CHECK_FALSE(again->value.contains("annotation"));
    d.host->answerReader(again->requestId, "");
    d.tree(d.inst);
    CHECK(repicked().quoteText() != "Kept for today.");

    // The item is gone (trashed): no error pop-up, and the pick is dropped too.
    d.store("passage", kept);
    CHECK(d.render().quoteText() == "Kept for today.");
    d.tapPassage("Item not found");
    d.tree(d.inst);
    CHECK(repicked().quoteText() != "Kept for today.");
    for (const auto& e : d.got) CHECK(e.kind != PluginHostEvent::Kind::ScreenActionError);
}

TEST_CASE("example: Reading Desk — a big library stays inside the app's 1 s render: 1,500 in the inbox (99+), 250 opened, 40 pins") {
    ScopedMetrics pw5(kPw5);
    Desk d(R"({"inbox":"COLLINBX"})", 1000);
    const int64_t now = nowMs();
    {
        auto tx = d.lib.begin();
        for (int i = 0; i < 1500; ++i) {
            char key[16], added[32];
            std::snprintf(key, sizeof key, "BIG%05d", i);
            std::snprintf(added, sizeof added, "2025-%02d-%02dT%02d:00:00Z", 1 + i / 500, 1 + i % 500 / 20, i % 20);  // rising
            deskItem(d.lib, key, "journalArticle", std::string("Inbox paper ") + key, added, "COLLINBX", "application/pdf");
        }
        // 250 papers opened, none a book: the pair's walk goes to its cap looking for one.
        for (int i = 0; i < 250; ++i) {
            char key[16], file[16];
            std::snprintf(key, sizeof key, "BIG%05d", i);
            std::snprintf(file, sizeof file, "FIG%05d", i);
            d.lib.markOpened(key, file, now - (250 - i) * 1000);
        }
        tx.commit();
    }
    for (int i = 0; i < 40; ++i) d.pin("BIG" + std::string(i < 10 ? "0000" : "000") + std::to_string(i));
    for (int round = 0; round < 3; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        HomeTree t = d.render();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE("Home with a 1,500-item inbox: " << ms << " ms");
        CHECK(ms < 1000);
        CHECK(t.pair() == Sides{{"Paper", "BIG00249"}});  // opened last
        CHECK(t.at("inbox").labels == Strings{"Inbox · 99+ new"});
        CHECK(t.at("inbox").keys() == Strings{"BIG01499", "BIG01498"});  // newest added first
    }
    for (const char* tab : {"inbox", "reading", "pinned"}) {
        const auto t0 = std::chrono::steady_clock::now();
        Part a = d.openAll({{"tab", tab}});
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE("Your desk, " << std::string(tab) << ": " << ms << " ms");
        CHECK(ms < 1000);
        CHECK(a.items.size() == (std::string(tab) == "inbox" ? 100u : std::string(tab) == "reading" ? 60u : 40u));
        if (std::string(tab) == "inbox") CHECK(a.link("tab-inbox") == "Inbox 99+");
        d.host->closeScreen(d.all);
    }
    for (const auto& e : d.got) CHECK(e.kind != PluginHostEvent::Kind::ScreenError);
}
