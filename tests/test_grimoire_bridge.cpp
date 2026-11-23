// The frozen Grimoire × Zotero bridge: strict validation, atomic/safe I/O, the three JSON
// documents, the plugin surface, deep-link opening and the clean exit 43 flag.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "bridge/grimoire_bridge.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fixture_library.h"
#include "plugins/host.h"
#include "screens/annotation_bar.h"
#include "screens/home.h"
#include "screens/library.h"
#include "screens/open_flow.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
using Kind = PluginHostEvent::Kind;

namespace fs = std::filesystem;

namespace {

struct BridgeDir {
    std::string path, old;
    bool had = false;

    BridgeDir() {
        static int n = 0;
        path = (fs::temp_directory_path() /
                ("zk-bridge-" + std::to_string(::getpid()) + "-" + std::to_string(++n) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                   .string();
        fs::create_directories(path);
        if (const char* v = std::getenv(bridge::kEnvVar)) {
            old = v;
            had = true;
        }
        ::setenv(bridge::kEnvVar, path.c_str(), 1);
    }
    ~BridgeDir() {
        if (had) ::setenv(bridge::kEnvVar, old.c_str(), 1);
        else ::unsetenv(bridge::kEnvVar);
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    BridgeDir(const BridgeDir&) = delete;
    BridgeDir& operator=(const BridgeDir&) = delete;

    std::string file(const std::string& name) const { return (fs::path(path) / name).string(); }
    static void write(const std::string& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    static std::string read(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    bool exists(const std::string& name) const { return fs::exists(fs::path(path) / name); }
};

json zoteroHandoff(const std::string& nonce = "n1") {
    return {{"schema_version", 1},
            {"nonce", nonce},
            {"created_at", 1732350000},
            {"source", "grimoire"},
            {"target", "zotero"},
            {"action", "open_item"},
            {"zotero", {{"item_key", "ABCD1234"}, {"attachment_key", "EFGH5678"}, {"page", 42}, {"locator", ""}}},
            {"return_to", {{"target", "grimoire"}, {"notebook_id", "NB1"}, {"page_id", 7}, {"page_index", 3}}}};
}

std::optional<PluginHostEvent> actionResult(PluginHost& host, const std::string& id, uint64_t& run,
                                            int ms = 5000) {
    run = host.runAction("p", "itemAction", id, json{{"key", "ABCD1234"}});
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        PluginHostEvent ev;
        while (host.poll(ev))
            if ((ev.kind == Kind::ActionResult || ev.kind == Kind::ActionError) && ev.requestId == run) return ev;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

std::string actionText(const PluginHostEvent& ev) {
    if (ev.value.is_object() && ev.value.contains("text") && ev.value["text"].is_string())
        return ev.value["text"].get<std::string>();
    return {};
}

const char* kBridgeJs = R"JS(
zk.itemAction({ id: "available", title: "a", run: () => String(zk.bridge.available()) });
zk.itemAction({ id: "cite", title: "c", run: () => JSON.stringify(zk.bridge.sendCitation({
  id: "1732350012345-f0e1d2", created_at: 1732350012, item_key: "ABCD1234",
  attachment_key: "EFGH5678", short_cite: "Kuhn 1962, p. 42", title: "Structure",
  authors: "Kuhn", year: "1962", page: 42, locator: "", quote: "Normal science", note: "" })) });
zk.itemAction({ id: "target", title: "t", run: () => JSON.stringify(zk.bridge.getReturnTarget()) });
zk.itemAction({ id: "ret", title: "r", run: () => { zk.bridge.returnToGrimoire(); return "sent"; } });
zk.itemAction({ id: "refresh", title: "f", run: () => JSON.stringify(zk.bridge.refreshIndex()) });
zk.itemAction({ id: "blocked", title: "b", run: () => JSON.stringify(zk.bridge.sendCitation({ item_key: "ABCD1234" })) });
)JS";

std::unique_ptr<ReaderScreen> fakeReader(const ItemRow& row, Nav&) {
    return std::make_unique<ReaderScreen>(row, "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) {
            return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>());
        });
    }, 6);
}

bool waitReady(ReaderScreen& r, FakeNav& nav) {
    for (int i = 0; i < 400; ++i) {
        r.onTick(nav);
        if (r.ready() && r.pageCount() > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool waitPage(ReaderScreen& r, FakeNav& nav, int page) {
    for (int i = 0; i < 400; ++i) {
        r.onTick(nav);
        if (r.page() == page && r.ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

TEST_CASE("bridge: validators enforce the contract's field bounds and UTF-8") {
    CHECK(bridge::validKey("ABCD-_123", 32));
    CHECK(bridge::validKey("a", 1));
    CHECK_FALSE(bridge::validKey("", 32));
    CHECK_FALSE(bridge::validKey("a/b", 32));
    CHECK_FALSE(bridge::validKey("a b", 32));
    CHECK_FALSE(bridge::validKey("..", 32));
    CHECK_FALSE(bridge::validKey(std::string(33, 'x'), 32));
    CHECK(bridge::validNotebookId(std::string(63, 'n')));
    CHECK_FALSE(bridge::validNotebookId(std::string(64, 'n')));
    CHECK(bridge::validNonceOrId(std::string(64, 'x')));
    CHECK_FALSE(bridge::validNonceOrId(std::string(65, 'x')));
    CHECK(bridge::validPage(0));
    CHECK(bridge::validPage(1));
    CHECK(bridge::validPage(1'000'000));
    CHECK_FALSE(bridge::validPage(1'000'001));
    CHECK(bridge::validLocator("p. 42"));
    CHECK_FALSE(bridge::validLocator("a\nb"));
    CHECK_FALSE(bridge::validLocator(std::string(513, 'x')));
    CHECK(bridge::validBoundedText("héllo", 256));
    CHECK_FALSE(bridge::validBoundedText(std::string("a\0b", 3), 256));
    CHECK_FALSE(bridge::validBoundedText("\xFF", 256));
    CHECK(bridge::validUtf8("hello 🦉"));
    CHECK_FALSE(bridge::validUtf8("\xC0\x80"));          // overlong
    CHECK_FALSE(bridge::validUtf8("\xED\xA0\x80"));      // surrogate
    CHECK_FALSE(bridge::validUtf8("\xF5\x80\x80\x80"));  // beyond U+10FFFF
    CHECK(bridge::sanitiseText(std::string("a\nb") + "\xFF" + "c", 256) ==
          std::string("a b") + "\xEF\xBF\xBD" + "c");
    CHECK(bridge::sanitiseText(std::string(300, 'x'), 16).size() == 16);
}

TEST_CASE("bridge: atomic writes and reads reject symlinks, oversize and bad UTF-8") {
    BridgeDir dir;
    std::string why;
    const std::string path = dir.file("doc.json");
    REQUIRE(bridge::writeFileAtomic(path, R"({"x":1})", &why));
    CHECK(bridge::pathIsRegularFile(path));
    std::string text;
    REQUIRE(bridge::readFileSafe(path, 256, &text, &why));
    CHECK(text == R"({"x":1})");
    CHECK_FALSE(bridge::readFileSafe(dir.file("missing.json"), 256, &text, &why));
    CHECK(why == "missing");

    fs::create_symlink(path, dir.file("link.json"));
    CHECK_FALSE(bridge::writeFileAtomic(dir.file("link.json"), "x", &why));
    CHECK_FALSE(bridge::readFileSafe(dir.file("link.json"), 256, &text, &why));
    fs::create_directory(dir.file("real"));
    fs::create_directory_symlink(dir.file("real"), dir.file("linkdir"));
    CHECK_FALSE(bridge::writeFileAtomic(dir.file("linkdir/x.json"), "x", &why));

    BridgeDir::write(dir.file("big.json"), std::string(300'000, 'a'));
    CHECK_FALSE(bridge::readFileSafe(dir.file("big.json"), bridge::kMaxJsonBytes, &text, &why));
    BridgeDir::write(dir.file("bad.json"), std::string("{\"x\":\"\xFF\"}"));
    CHECK_FALSE(bridge::readFileSafe(dir.file("bad.json"), bridge::kMaxJsonBytes, &text, &why));
}

TEST_CASE("bridge: a zotero handoff is consumed once and its return_to is persisted") {
    BridgeDir dir;
    std::string why;
    BridgeDir::write(dir.file("handoff.json"), zoteroHandoff().dump());
    auto h = bridge::consumeZoteroHandoff(dir.path, &why);
    REQUIRE(h);
    CHECK(h->target == "zotero");
    CHECK(h->itemKey == "ABCD1234");
    CHECK(h->attachmentKey == "EFGH5678");
    CHECK(h->page == 42);
    CHECK(h->hasReturnTo);
    CHECK_FALSE(fs::exists(dir.file("handoff.json")));
    auto ctx = bridge::readReturnContext(dir.path, &why);
    REQUIRE(ctx);
    CHECK(ctx->notebookId == "NB1");
    CHECK(ctx->pageId == 7);
    CHECK(ctx->pageIndex == 3);
    CHECK_FALSE(bridge::consumeZoteroHandoff(dir.path, &why));

    json other = {{"schema_version", 1}, {"nonce", "n2"}, {"created_at", 1}, {"source", "zotero"},
                  {"target", "grimoire"}, {"action", "return_essay"},
                  {"grimoire", {{"notebook_id", "NB2"}, {"page_id", 9}, {"page_index", 4}}}};
    BridgeDir::write(dir.file("handoff.json"), other.dump());
    auto left = bridge::consumeZoteroHandoff(dir.path, &why);
    REQUIRE(left);
    CHECK(left->target == "grimoire");
    CHECK(fs::exists(dir.file("handoff.json")));  // its consumer, not Zotero, takes it

    BridgeDir::write(dir.file("handoff.json"), "{not json");
    CHECK_FALSE(bridge::consumeZoteroHandoff(dir.path, &why));
    CHECK_FALSE(fs::exists(dir.file("handoff.json")));
    CHECK(why.find("discarded") != std::string::npos);
    BridgeDir::write(dir.file("handoff.json"), std::string(300'000, 'x'));
    CHECK_FALSE(bridge::consumeZoteroHandoff(dir.path, &why));
    CHECK_FALSE(fs::exists(dir.file("handoff.json")));
    json badSchema = zoteroHandoff("n3");
    badSchema["schema_version"] = 2;
    BridgeDir::write(dir.file("handoff.json"), badSchema.dump());
    CHECK_FALSE(bridge::consumeZoteroHandoff(dir.path, &why));
    CHECK_FALSE(fs::exists(dir.file("handoff.json")));
}

TEST_CASE("bridge: return contexts round-trip and exit-43 handoffs name grimoire") {
    BridgeDir dir;
    std::string why;
    const bridge::ReturnContext ctx{"NB-9", 11, 5};
    REQUIRE(bridge::writeReturnContext(ctx, dir.path, &why));
    auto read = bridge::readReturnContext(dir.path, &why);
    REQUIRE(read);
    CHECK(read->notebookId == "NB-9");
    CHECK(read->pageId == 11);
    CHECK(read->pageIndex == 5);
    REQUIRE(bridge::writeReturnToGrimoireHandoff(ctx, dir.path, &why));
    const auto handoff = bridge::parseHandoff(BridgeDir::read(dir.file("handoff.json")), &why);
    REQUIRE(handoff);
    CHECK(handoff->target == "grimoire");
    CHECK(handoff->action == "return_essay");
    CHECK(handoff->pageId == 11);
    CHECK(handoff->hasReturnTo);
    CHECK(BridgeDir::read(dir.file("handoff.json")).find("api_key") == std::string::npos);

    // Regression: returning to Grimoire without a prior return context omits return_to cleanly.
    const bridge::ReturnContext emptyCtx{};
    CHECK_FALSE(bridge::writeReturnContext(emptyCtx, dir.path, &why));
    REQUIRE(bridge::writeReturnToGrimoireHandoff(emptyCtx, dir.path, &why));
    const std::string rawEmpty = BridgeDir::read(dir.file("handoff.json"));
    CHECK(rawEmpty.find("\"return_to\"") == std::string::npos);
    const auto emptyHandoff = bridge::parseHandoff(rawEmpty, &why);
    REQUIRE(emptyHandoff);
    CHECK(emptyHandoff->target == "grimoire");
    CHECK(emptyHandoff->notebookId.empty());
    CHECK_FALSE(emptyHandoff->hasReturnTo);

    // Regression: writeHandoff rejects unknown sources instead of writing an unreadable document.
    bridge::HandoffRequest badSource;
    badSource.source = "unknown_app";
    badSource.target = "grimoire";
    badSource.action = "return_essay";
    CHECK_FALSE(bridge::writeHandoff(badSource, dir.path, &why));
}

TEST_CASE("bridge: citations queue as individually named schema-v1 files") {
    BridgeDir dir;
    std::string why, id;
    bridge::CitationPayload c;
    c.id = "1732350012345-f0e1d2";
    c.createdAt = 1732350012;
    c.itemKey = "ABCD1234";
    c.attachmentKey = "EFGH5678";
    c.shortCite = "Kuhn 1962, p. 42";
    c.title = "The Structure of Scientific Revolutions";
    c.authors = "Kuhn";
    c.year = "1962";
    c.page = 42;
    c.quote = "Normal science 🦉";
    REQUIRE(bridge::queueCitation(c, dir.path, &id, &why));
    CHECK(id == c.id);
    const std::string path = dir.file("inbox/" + id + ".json");
    REQUIRE(bridge::pathIsRegularFile(path));
    auto parsed = bridge::parseCitation(BridgeDir::read(path), &why);
    REQUIRE(parsed);
    CHECK(parsed->id == c.id);
    CHECK(parsed->itemKey == c.itemKey);
    CHECK(parsed->page == 42);
    CHECK(parsed->quote == c.quote);
    CHECK(BridgeDir::read(path).find("schema_version") != std::string::npos);

    c.itemKey = "bad/key";
    CHECK_FALSE(bridge::queueCitation(c, dir.path, nullptr, &why));
    c.itemKey = "ABCD1234";
    c.locator = "line\nbreak";
    CHECK_FALSE(bridge::queueCitation(c, dir.path, nullptr, &why));
}

TEST_CASE("bridge: the library index exports bibliographic fields only, and only when absent") {
    BridgeDir dir;
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 6);
    std::string why;
    REQUIRE(bridge::exportLibraryIndex(lib, dir.path, &why));
    std::string text;
    REQUIRE(bridge::readFileSafe(dir.file("library_index.json"), bridge::kMaxIndexBytes, &text, &why));
    const json j = json::parse(text);
    CHECK(j["schema_version"] == 1);
    CHECK(j["item_count"] == 7);
    CHECK(j["items"].size() == 7);
    CHECK(j["items"][0]["item_key"] == "IT000000");
    CHECK(j["items"][0].contains("citation_key"));
    CHECK(j["items"][0].contains("has_pdf"));
    CHECK(j["items"][0].contains("tags"));
    for (const char* secret : {"api_key", "password", "WebDAV", "config.json", "accounts.json", "token"})
        CHECK(text.find(secret) == std::string::npos);
    BridgeDir::write(dir.file("library_index.json"), "sentinel");
    CHECK(bridge::exportLibraryIndexIfAbsent(lib, dir.path, &why));
    CHECK(BridgeDir::read(dir.file("library_index.json")) == "sentinel");
}

TEST_CASE("bridge bindings: zk.bridge is permission-gated and writes only bridge files") {
    BridgeDir dir;
    REQUIRE(bridge::writeReturnContext({"NB3", 11, 5}, dir.path));
    const std::string dbPath = dir.file("library.db");
    {
        Db db = Db::open(dbPath);
        Library lib(db);
        fillFixtureLibrary(lib, 3);
    }
    FakePlugins fp;
    fp.add("p", kBridgeJs, {"library:read", "grimoire:bridge"});
    PluginHostDeps deps = fp.deps();
    deps.account.dbPath = dbPath;
    PluginHost host(deps);
    host.setEnabled("p", true);
    host.start();

    uint64_t run = 0;
    auto ev = actionResult(host, "available", run);
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionResult);
    CHECK(actionText(*ev) == "true");

    ev = actionResult(host, "cite", run);
    REQUIRE(ev);
    REQUIRE(ev->kind == Kind::ActionResult);
    const json citeRes = json::parse(actionText(*ev));
    CHECK(citeRes["ok"] == true);
    CHECK(citeRes["id"] == "1732350012345-f0e1d2");
    CHECK(bridge::pathIsRegularFile(dir.file("inbox/1732350012345-f0e1d2.json")));

    ev = actionResult(host, "target", run);
    REQUIRE(ev);
    REQUIRE(ev->kind == Kind::ActionResult);
    const json targetRes = json::parse(actionText(*ev));
    CHECK(targetRes["notebook_id"] == "NB3");
    CHECK(targetRes["page_id"] == 11);
    CHECK(targetRes["page_index"] == 5);

    ev = actionResult(host, "refresh", run);
    REQUIRE(ev);
    REQUIRE(ev->kind == Kind::ActionResult);
    const json refRes = json::parse(actionText(*ev));
    CHECK(refRes["item_count"] == 4);
    CHECK(bridge::pathIsRegularFile(dir.file("library_index.json")));

    ev = actionResult(host, "ret", run);
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionResult);
    CHECK(host.bridgeReturnRequested());
    const auto handoff = bridge::parseHandoff(BridgeDir::read(dir.file("handoff.json")));
    REQUIRE(handoff);
    CHECK(handoff->target == "grimoire");
    CHECK(handoff->pageId == 11);
    host.stop();

    FakePlugins denied;
    denied.add("p", kBridgeJs);  // no grimoire:bridge permission
    PluginHost noPerm(denied.deps());
    noPerm.setEnabled("p", true);
    noPerm.start();
    ev = actionResult(noPerm, "blocked", run);
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionError);
    CHECK(ev->text.find("grimoire:bridge") != std::string::npos);
    CHECK_FALSE(fs::exists(dir.file("inbox/1732350012345-nope.json")));
    noPerm.stop();
}

TEST_CASE("bridge plugin: bundled grimoire-bridge/main.js executes cleanly inside QuickJS-ng") {
    BridgeDir dir;
    REQUIRE(bridge::writeReturnContext({"NB-LIVE", 4, 2}, dir.path));
    const std::string dbPath = dir.file("library.db");
    {
        Db db = Db::open(dbPath);
        Library lib(db);
        fillFixtureLibrary(lib, 2);
    }
    const std::string jsCode = BridgeDir::read("plugins/grimoire-bridge/main.js");
    REQUIRE_FALSE(jsCode.empty());

    FakePlugins fp;
    fp.add("grimoire-bridge", jsCode, {"library:read", "grimoire:bridge"});
    PluginHostDeps deps = fp.deps();
    deps.account.dbPath = dbPath;
    PluginHost host(deps);
    host.setEnabled("grimoire-bridge", true);
    host.start();

    const json selPayload = {
        {"text", "Normal science is puzzle-solving."},
        {"pageLabel", "42"},
        {"file", {{"key", "ATT00001"}}},
        {"item",
         {{"key", "IT000001"},
          {"title", "The Structure of Scientific Revolutions"},
          {"year", 1962},
          {"creators", json::array({{{"first", "Thomas"}, {"last", "Kuhn"}, {"type", "author"}}})}}},
    };
    const uint64_t req = host.runAction("grimoire-bridge", "selectionAction", "cite-to-grimoire", selPayload);
    std::optional<PluginHostEvent> ev;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end && !ev) {
        PluginHostEvent e;
        while (host.poll(e))
            if ((e.kind == Kind::ActionResult || e.kind == Kind::ActionError) && e.requestId == req) ev = e;
        if (!ev) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(ev);
    REQUIRE(ev->kind == Kind::ActionResult);
    CHECK(actionText(*ev).find("Kuhn 1962, p. 42") != std::string::npos);

    // Verify an inbox event JSON was written and parses cleanly as schema v1.
    size_t inboxFiles = 0;
    for (const auto& entry : fs::directory_iterator(dir.file("inbox"))) {
        if (entry.path().extension() == ".json") {
            ++inboxFiles;
            const auto parsed = bridge::parseCitation(BridgeDir::read(entry.path().string()));
            REQUIRE(parsed);
            CHECK(parsed->itemKey == "IT000001");
            CHECK(parsed->attachmentKey == "ATT00001");
            CHECK(parsed->shortCite == "Kuhn 1962, p. 42");
            CHECK(parsed->year == "1962");
            CHECK(parsed->page == 42);
        }
    }
    CHECK(inboxFiles == 1);
    host.stop();
}

TEST_CASE("bridge deep links: a handoff picks its file and applies the reader page on Opened") {
    BridgeDir dir;  // the bridge needn't be used for this test, just be configured
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    OpenFlow flow([] { return 1'000'000; });
    flow.setReaderFactory(fakeReader);
    const auto pdf = lib.rowFor("IT000001");
    REQUIRE(pdf);
    lib.setFileState(pdf->attachmentKey, AttachmentRecord::State::Local, "/tmp/paper.pdf");

    bridge::HandoffRequest h;
    h.target = "zotero";
    h.action = "open_item";
    h.itemKey = "IT000001";
    h.page = 7;
    REQUIRE(openZoteroHandoff(h, lib, flow, nav));
    REQUIRE(nav.pushed.size() == 1);
    auto* reader = dynamic_cast<ReaderScreen*>(nav.pushed[0].get());
    REQUIRE(reader);
    reader->onShow(nav);
    REQUIRE(waitPage(*reader, nav, 6));  // the handoff's 1-based page 7

    bridge::HandoffRequest missing = h;
    missing.itemKey = "NOPE0000";
    CHECK_FALSE(openZoteroHandoff(missing, lib, flow, nav));
    CHECK(nav.pushed.size() == 1);
    bridge::HandoffRequest wrongFile = h;
    wrongFile.attachmentKey = "NOTTHATFILE";
    CHECK_FALSE(openZoteroHandoff(wrongFile, lib, flow, nav));

    auto again = fakeReader(*pdf, nav);
    again->jumpToBridgeTarget(1'000'000, "");  // clamped to the last page
    again->onShow(nav);
    REQUIRE(waitReady(*again, nav));  // the page count arrives with Opened, asynchronously
    REQUIRE(waitPage(*again, nav, again->pageCount() - 1));
}

TEST_CASE("bridge: normalizeQuoteText preserves word boundaries and safely truncates UTF-8") {
    // 1. Multi-line, carriage returns, tabs: words never glued together
    const std::string raw = "  The \r\n structure  \t of \n\n scientific \r revolutions.  ";
    const std::string normalized = bridge::normalizeQuoteText(raw);
    CHECK(normalized == "The structure of scientific revolutions.");

    // 2. Control characters stripped
    const std::string withControls = "Word1\x01\x02\x05 Word2\x1f\x7f Word3";
    CHECK(bridge::normalizeQuoteText(withControls) == "Word1 Word2 Word3");

    // 3. UTF-8 boundary safe truncation
    // "Hello " (6 bytes) + "€" (3 bytes: \xE2\x82\xAC) + " world"
    const std::string euroStr = "Hello \xE2\x82\xAC world";
    // Truncate at 7 bytes (inside the 3-byte euro character): must cut back to "Hello" (5 bytes, trailing space trimmed)
    CHECK(bridge::normalizeQuoteText(euroStr, 7) == "Hello");
    // Truncate at 9 bytes (exact end of euro): keeps "Hello €"
    CHECK(bridge::normalizeQuoteText(euroStr, 9) == "Hello \xE2\x82\xAC");

    // 4-byte emoji: "Hi " (3 bytes) + 😀 (\xF0\x9F\x98\x80, 4 bytes)
    const std::string emojiStr = "Hi \xF0\x9F\x98\x80 there";
    // Truncate at 4, 5, or 6 bytes (in middle of emoji): must truncate cleanly before emoji
    CHECK(bridge::normalizeQuoteText(emojiStr, 4) == "Hi");
    CHECK(bridge::normalizeQuoteText(emojiStr, 5) == "Hi");
    CHECK(bridge::normalizeQuoteText(emojiStr, 6) == "Hi");
    // Truncate at 7 bytes: keeps emoji
    CHECK(bridge::normalizeQuoteText(emojiStr, 7) == "Hi \xF0\x9F\x98\x80");

    // Enforce 4096-byte cap
    const std::string huge(5000, 'x');
    CHECK(bridge::normalizeQuoteText(huge).size() == 4096);
}

TEST_CASE("bridge: formatAuthors and formatShortCite cleanly match main.js format") {
    // 1 author
    std::vector<Creator> c1 = {{"Thomas", "Kuhn", "", "author"}};
    CHECK(bridge::formatAuthors(c1) == "Kuhn");
    CHECK(bridge::formatShortCite("Kuhn", "1962", "The Structure of Scientific Revolutions", 42) == "Kuhn 1962, p. 42");

    // 2 authors
    std::vector<Creator> c2 = {{"Jerome", "Adda", "", "author"}, {"Russell", "Cooper", "", "author"}};
    CHECK(bridge::formatAuthors(c2) == "Adda & Cooper");
    CHECK(bridge::formatShortCite("Adda & Cooper", "2003", "Dynamic Economics", 15) == "Adda & Cooper 2003, p. 15");

    // 3+ authors
    std::vector<Creator> c3 = {{"M.", "Raissi", "", "author"}, {"P.", "Perdikaris", "", "author"}, {"G.", "Karniadakis", "", "author"}};
    CHECK(bridge::formatAuthors(c3) == "Raissi et al.");
    CHECK(bridge::formatShortCite("Raissi et al.", "2019", "Physics-informed neural networks", 0) == "Raissi et al. 2019");

    // Single-field author (name only)
    std::vector<Creator> cSingle = {{"", "", "Plato", "author"}};
    CHECK(bridge::formatAuthors(cSingle) == "Plato");
    CHECK(bridge::formatShortCite("Plato", "", "The Republic", 10) == "The Republic, p. 10");

    // Non-author creators ignored
    std::vector<Creator> cEditor = {{"John", "Editor", "", "editor"}};
    CHECK(bridge::formatAuthors(cEditor) == "");
    CHECK(bridge::formatShortCite("", "", "Some Collected Works", 5) == "Some Collected Works, p. 5");

    // Completely empty
    CHECK(bridge::formatShortCite("", "", "", 0) == "Untitled");
}

TEST_CASE("bridge: 1-tap Grimoire jump buttons in Reader, Library, and Home") {
    SUBCASE("active bridge surfaces buttons and exits with 43") {
        BridgeDir dir;
        REQUIRE(bridge::bridgeActive());

        // Set up return context
        bridge::ReturnContext ctx{"NB_ACTIVE", 12, 4};
        REQUIRE(bridge::writeReturnContext(ctx));

        Db db = Db::open(":memory:");
        Library lib(db);
        fillFixtureLibrary(lib, 5);
        FakeNav nav(lib);

        // 1. Reader top bar Grimoire button
        const auto pdf = lib.rowFor("IT000001");
        REQUIRE(pdf);
        auto reader = fakeReader(*pdf, nav);
        reader->onShow(nav);
        ReaderMenus menus(*reader, nullptr);
        const Rect rGrimoire = ReaderMenus::grimoireRect();
        CHECK_FALSE(rGrimoire.empty());
        CHECK(rGrimoire.x + rGrimoire.w < ReaderMenus::searchRect().x);

        // Tap Grimoire button
        menus.onGesture(tapAt({rGrimoire.x + 10, rGrimoire.y + 10}), nav);
        CHECK(nav.quitted);
        CHECK(nav.exitCode == 43);

        // Verify handoff was written with target grimoire and return context
        std::string why;
        std::string handoffStr;
        REQUIRE(bridge::readFileSafe(dir.file("handoff.json"), bridge::kMaxJsonBytes, &handoffStr, &why));
        const auto h = bridge::parseHandoff(handoffStr, &why);
        REQUIRE(h);
        CHECK(h->target == "grimoire");
        CHECK(h->notebookId == "NB_ACTIVE");
        CHECK(h->pageId == 12);
        CHECK(h->pageIndex == 4);

        // 2. Library header Grimoire button
        FakeNav navLib(lib);
        LibraryScreen libScreen([](const ItemRow&) {}, [](const std::string&) { return ""; });
        const Rect lGrimoire = LibraryScreen::grimoireButtonRect();
        CHECK_FALSE(lGrimoire.empty());
        CHECK(lGrimoire.x + lGrimoire.w < LibraryScreen::filterButtonRect().x);

        libScreen.onGesture(tapAt({lGrimoire.x + 10, lGrimoire.y + 10}), navLib);
        CHECK(navLib.quitted);
        CHECK(navLib.exitCode == 43);

        // 3. Home header Grimoire button
        FakeNav navHome(lib);
        SyncStatus syncStatus;
        HomeScreen homeScreen(syncStatus, {}, [] { return 1'000'000; });
        const Rect hGrimoire = HomeScreen::grimoireRect();
        CHECK_FALSE(hGrimoire.empty());
        CHECK(hGrimoire.x + hGrimoire.w < HomeScreen::pluginsRect().x);

        homeScreen.onGesture(tapAt({hGrimoire.x + 10, hGrimoire.y + 10}), navHome);
        CHECK(navHome.quitted);
        CHECK(navHome.exitCode == 43);
    }

    SUBCASE("absent bridge hides buttons completely (zero UI regressions)") {
        ::unsetenv(bridge::kEnvVar);
        REQUIRE_FALSE(bridge::bridgeActive());

        CHECK(ReaderMenus::grimoireRect().empty());
        CHECK(LibraryScreen::grimoireButtonRect().empty());
        CHECK(HomeScreen::grimoireRect().empty());
    }
}

TEST_CASE("bridge: 1-tap Grimoire Cite-and-Jump on Selection & Existing Highlight") {
    BridgeDir dir;
    REQUIRE(bridge::bridgeActive());

    bridge::ReturnContext ctx{"ESSAY_NB", 99, 1};
    REQUIRE(bridge::writeReturnContext(ctx));

    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 5);
    FakeNav nav(lib);

    const auto pdf = lib.rowFor("IT000001");
    REQUIRE(pdf);
    lib.setFileState(pdf->attachmentKey, AttachmentRecord::State::Local, "/tmp/paper.pdf");
    auto reader = fakeReader(*pdf, nav);
    reader->onShow(nav);
    REQUIRE(waitReady(*reader, nav));

    // 1. Text Selection Bar: 5 buttons when bridgeActive()
    AnnotationBar bar(AnnotationBar::Mode::Create, {100, 200, 400, 50}, "#ffd400", true, {}, true);
    CHECK(bar.buttonRect(3).w > 100);
    // Button 3 is Grimoire
    const auto hitGrimoire = bar.hit({bar.buttonRect(3).x + 10, bar.buttonRect(3).y + 10});
    CHECK(hitGrimoire.action == AnnotationBar::Action::Grimoire);

    // Test Reader citeAndJumpToGrimoire
    reader->citeAndJumpToGrimoire(nav, "Multi-line\r\nquote\tacross lines", "User note", "42");
    CHECK(nav.quitted);
    CHECK(nav.exitCode == 43);

    // Verify inbox event
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir.file("inbox"))) {
        if (entry.path().extension() == ".json") {
            ++count;
            const auto parsed = bridge::parseCitation(BridgeDir::read(entry.path().string()));
            REQUIRE(parsed);
            CHECK(parsed->quote == "Multi-line quote across lines");
            CHECK(parsed->note == "User note");
            CHECK(parsed->page == 42);
            CHECK(parsed->locator == "42");
            CHECK(parsed->shortCite.find("p. 42") != std::string::npos);
        }
    }
    CHECK(count >= 1);

    // 2. Existing highlight edit bar: 3 buttons when bridgeActive()
    AnnotationRecord ann;
    ann.key = "HL0001";
    ann.parentKey = pdf->attachmentKey;
    ann.type = "highlight";
    ann.text = "Existing highlight text\r\nwith newline";
    ann.comment = "Thoughtful reflection";
    ann.color = "#ffd400";
    ann.pageLabel = "15";
    lib.createAnnotation(ann);

    AnnotationEditor editor(ann, {100, 200, 400, 50});
    CHECK(editor.bar().buttons() == 3);
    // Button 1 is Grimoire
    const Rect btnGrimoire = editor.bar().buttonRect(1);
    const auto hitEditGrimoire = editor.bar().hit({btnGrimoire.x + 5, btnGrimoire.y + 5});
    CHECK(hitEditGrimoire.action == AnnotationBar::Action::Grimoire);

    // Tapping it via AnnotationEditor::onTap yields Result::Grimoire
    FakeNav navEdit(lib);
    const auto editRes = editor.onTap({btnGrimoire.x + 5, btnGrimoire.y + 5}, navEdit);
    CHECK(editRes == AnnotationEditor::Result::Grimoire);

    // 3. Standalone mode (bridge inactive)
    ::unsetenv(bridge::kEnvVar);
    REQUIRE_FALSE(bridge::bridgeActive());
    AnnotationBar barStandalone(AnnotationBar::Mode::Create, {100, 200, 400, 50}, "#ffd400", true, {}, false);
    CHECK(barStandalone.buttons() == 4); // Highlight, Underline, Note, More... (no Grimoire)

    AnnotationBar editStandalone(AnnotationBar::Mode::Edit, {100, 200, 400, 50}, "#ffd400", false, {}, false);
    CHECK(editStandalone.buttons() == 2); // Comment, Delete (no Grimoire)
}
