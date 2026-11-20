// Redrawing only what a plugin changed (src/screens/plugin_diff.h).

#include <doctest/doctest.h>

#include <filesystem>

#include "screens/item_source.h"
#include "screens/plugin_diff.h"
#include "ui/metrics.h"
#include "ui/text.h"

using namespace zk;
using json = nlohmann::json;

namespace {

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

Block tree(const std::string& s) {
    std::string why;
    auto t = parseTree(json::parse(s), &why);
    INFO(why);
    REQUIRE(t);
    return *t;
}

const char* kDeck = R"({"kind":"column","children":[
  {"kind":"text","text":"math.PR · 12 left","style":"label"},
  {"kind":"deck","id":"card","card":{"kind":"card","title":"First card","meta":"Adler","body":"Short."}},
  {"kind":"buttons","buttons":[{"id":"discard","title":"Discard"},{"id":"keep","title":"Keep","primary":true}]},
  {"kind":"notice","text":"Kept"}
]})";

std::string with(std::string s, const std::string& from, const std::string& to) {
    s.replace(s.find(from), from.size(), to);
    return s;
}

}  // namespace

TEST_CASE("plugin diff: identical trees need no redraw") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(kDeck);
    CHECK(diffTrees(layoutTree(a, blocksArea(), 0), layoutTree(b, blocksArea(), 0)).empty());
}

TEST_CASE("plugin diff: a button's label redraws that block with Ui; a notice's text with Fast") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(with(kDeck, "\"Discard\"", "\"Drop\""));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lb.children[2].rect);
    CHECK(d[0].wave == Waveform::Ui);

    const Block c = tree(with(kDeck, "\"Kept\"", "\"Discarded\""));
    d = diffTrees(la, layoutTree(c, blocksArea(), 0));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == la.children[3].rect);
    CHECK(d[0].wave == Waveform::Fast);
}

TEST_CASE("plugin diff: another card in the deck redraws the deck with Quality") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(with(kDeck, "First card", "Second"));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    const auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == la.children[1].rect);
    CHECK(d[0].wave == Waveform::Quality);
}

TEST_CASE("plugin diff: a list's page turn redraws the list and the indicator with Quality") {
    if (!fonts()) return;
    ScopedMetrics m(kKt4);
    json rows = json::array();
    for (int i = 0; i < 50; ++i) rows.push_back({{"id", std::to_string(i)}, {"title", "Row " + std::to_string(i)}});
    json j = json::parse(R"({"kind":"column","children":[{"kind":"list","id":"l","rows":[]},{"kind":"button","id":"b","title":"Fetch now"}]})");
    j["children"][0]["rows"] = rows;
    const Block t = tree(j.dump());
    const Laid p0 = layoutTree(t, blocksArea(), 0), p1 = layoutTree(t, blocksArea(), 1);
    REQUIRE(p0.pages > 1);
    const auto d = diffTrees(p0, p1);
    REQUIRE(d.size() == 2);  // the list; the indicator (the button between them is unchanged)
    CHECK(d[0].rect == p0.children[0].rect);
    CHECK(d[0].wave == Waveform::Quality);
    CHECK(d[1].rect == pageIndicatorRect(p1));
    CHECK(d[1].wave == Waveform::Quality);
}

TEST_CASE("plugin diff: a block inserted above redraws the region that moved") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(with(kDeck, R"({"kind":"deck")", R"({"kind":"notice","text":"New cards"},{"kind":"deck")"));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    const auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lb.children[1].rect.united(lb.children.back().rect).united(la.children[1].rect));
    CHECK(d[0].rect.y == la.children[1].rect.y);  // the label above is not redrawn
    CHECK(d[0].wave == Waveform::Ui);  // blocks moved, nothing turned
}

TEST_CASE("plugin diff: a short card after a long one redraws only the deck (the blocks under it stay)") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    std::string body;
    for (int i = 0; i < 40; ++i) body += "A long abstract that goes on and on about random fields. ";
    const Block a = tree(with(kDeck, "\"Short.\"", "\"" + body + "\"")), b = tree(with(kDeck, "First card", "Second"));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    REQUIRE(la.pages > 1);
    REQUIRE(lb.pages == 1);
    const auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == la.children[1].rect);  // the card; its page indicator is inside it
    CHECK(d[0].wave == Waveform::Quality);    // a partial refresh, never a full flash
}

namespace {

struct Rows : ItemSource {
    std::map<std::string, ItemRow> rows;
    std::optional<ItemRow> row(const ItemRef& r) override {
        auto it = rows.find(r.key);
        if (it == rows.end()) return std::nullopt;
        return it->second;
    }
    std::string meta;
    std::string cardMeta(const ItemRow&) override { return meta; }
    std::string status(const std::string&) override { return ""; }
    Rows() {
        for (const char* k : {"A", "B", "C"}) {
            ItemRow& r = rows[k];
            r.key = k, r.title = std::string("Item ") + k, r.badge = "PDF", r.attachmentKey = std::string("F") + k;
        }
    }
};

const char* kItems = R"({"kind":"column","children":[
  {"kind":"text","text":"Reading","style":"label"},
  {"kind":"items","id":"recent","style":"row","items":["A","B","MISSING"]},
  {"kind":"buttons","buttons":[{"id":"more","title":"More"}]}
]})";

}  // namespace

TEST_CASE("plugin diff: an items block with the same tree and the same rows needs no redraw") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Rows rows;
    const Block a = tree(kItems), b = tree(kItems);
    CHECK(diffTrees(layoutTree(a, blocksArea(), 0, &rows), layoutTree(b, blocksArea(), 0, &rows)).empty());
}

TEST_CASE("plugin diff: one row's progress changing redraws only the items block") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Rows before, after;
    after.rows["B"].progress = 0.5;
    const Block a = tree(kItems), b = tree(kItems);
    const Laid la = layoutTree(a, blocksArea(), 0, &before), lb = layoutTree(b, blocksArea(), 0, &after);
    const auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lb.children[1].rect);
    // a key that turns readable adds a row: the block's rect grows and the buttons below move
    Rows more;
    more.rows["MISSING"] = more.rows["C"];
    more.rows["MISSING"].key = "MISSING";
    CHECK_FALSE(diffTrees(la, layoutTree(b, blocksArea(), 0, &more)).empty());
}

TEST_CASE("plugin diff: an items block nested in a column is diffed on its rows and a card on its meta line") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const char* nested = R"({"kind":"column","children":[
      {"kind":"text","text":"Reading","style":"label"},
      {"kind":"column","children":[{"kind":"items","id":"recent","style":"row","items":["A","B"]}]},
      {"kind":"buttons","buttons":[{"id":"more","title":"More"}]}]})";
    Rows before, after;
    after.rows["B"].progress = 0.5;
    const Block a = tree(nested), b = tree(nested);
    const Laid la = layoutTree(a, blocksArea(), 0, &before), lb = layoutTree(b, blocksArea(), 0, &after);
    const auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lb.children[1].rect);

    const char* cards = R"({"kind":"column","children":[
      {"kind":"items","id":"now","style":"card","items":["A"]},
      {"kind":"buttons","buttons":[{"id":"more","title":"More"}]}]})";
    Rows x, y;
    x.meta = "42% · 3 highlights · opened today";
    y.meta = "42% · 4 highlights · opened today";
    const Block c = tree(cards), e = tree(cards);
    CHECK(diffTrees(layoutTree(c, blocksArea(), 0, &x), layoutTree(e, blocksArea(), 0, &x)).empty());
    const Laid lc = layoutTree(c, blocksArea(), 0, &x), le = layoutTree(e, blocksArea(), 0, &y);
    const auto d2 = diffTrees(lc, le);
    REQUIRE(d2.size() == 1);
    CHECK(d2[0].rect == le.children[0].rect);
}

TEST_CASE("plugin diff: a shelf is diffed on its rows: one book's progress redraws the shelf only, with Quality") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const char* shelf = R"({"kind":"column","children":[
      {"kind":"text","text":"Shelf","style":"label"},
      {"kind":"items","id":"shelf","style":"shelf","items":["A","B","C"]},
      {"kind":"buttons","buttons":[{"id":"more","title":"More"}]}]})";
    Rows before, after;
    after.rows["B"].progress = 0.5;
    const Block a = tree(shelf), b = tree(shelf);
    const Laid la = layoutTree(a, blocksArea(), 0, &before), lb = layoutTree(b, blocksArea(), 0, &before);
    CHECK(diffTrees(la, lb).empty());
    const Laid lc = layoutTree(b, blocksArea(), 0, &after);
    const auto d = diffTrees(la, lc);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lc.children[1].rect);
    CHECK(d[0].wave == Waveform::Quality);  // dark covers: a partial refresh that leaves no ghost
}

TEST_CASE("plugin diff: a quote's new text redraws it with Fast in place, Ui when its size changed") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const char* kQuote = R"({"kind":"column","children":[
      {"kind":"text","text":"Desk","style":"title"},
      {"kind":"quote","id":"passage","text":"It is a truth universally acknowledged.","source":"Austen · p. 1"},
      {"kind":"button","id":"more","title":"More"}]})";
    const Block a = tree(kQuote), same = tree(kQuote), b = tree(with(kQuote, "It is a truth", "It is a fact"));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    CHECK(diffTrees(la, layoutTree(same, blocksArea(), 0)).empty());
    auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lb.children[1].rect);
    CHECK(d[0].wave == Waveform::Fast);
    const Block c = tree(with(kQuote, "It is a truth universally acknowledged.",
                              "It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in "
                              "want of a wife. However little known the feelings or views of such a man may be"));
    d = diffTrees(la, layoutTree(c, blocksArea(), 0));
    REQUIRE_FALSE(d.empty());
    CHECK(d[0].wave == Waveform::Ui);
}

TEST_CASE("plugin diff: a top-level cover, grid or item piece redraws with Quality, as a shelf does") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    for (const char* piece : {R"({"kind":"cover","item":"B","width":"20%"})",
                              R"({"kind":"grid","columns":3,"children":[{"kind":"cover","item":"A"},{"kind":"cover","item":"B"}]})",
                              R"({"kind":"item","item":"B","child":{"kind":"cover","item":"B","width":"20%"}})"}) {
        INFO(piece);
        const std::string j = std::string(R"({"kind":"column","children":[{"kind":"text","text":"Shelf","style":"label"},)") + piece + "]}";
        Rows before, after;
        after.rows["B"].progress = 0.5;
        const Block a = tree(j), b = tree(j);
        const Laid la = layoutTree(a, blocksArea(), 0, &before), lb = layoutTree(b, blocksArea(), 0, &after);
        CHECK(diffTrees(la, layoutTree(b, blocksArea(), 0, &before)).empty());
        const auto d = diffTrees(la, lb);
        REQUIRE(d.size() == 1);
        CHECK(d[0].rect == lb.children[1].rect);
        CHECK(d[0].wave == Waveform::Quality);
    }
}
