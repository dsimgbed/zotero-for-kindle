// Plugin screen blocks: layout with paging, drawing in the app's style, hit-testing
// (src/screens/plugin_blocks.h). Goldens on PW5; KT4 and Scribe get fit assertions.

#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "golden.h"
#include "screens/item_source.h"
#include "screens/plugin_blocks.h"
#include "ui/canvas.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
using K = Block::Kind;

namespace {

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

Block tree(const json& j) {
    std::string why;
    auto t = parseTree(j, &why);
    INFO(why);
    REQUIRE(t);
    return *t;
}

const char* kSummary =
    "We construct Gaussian random fields on compact Riemannian manifolds whose covariance is a function of the "
    "Laplace-Beltrami operator, and study the regularity of their sample paths.\n\nAs an application we obtain sharp "
    "Hölder exponents for solutions of stochastic partial differential equations driven by such noise.";

json deckScreen(const std::string& body = kSummary) {
    json j = json::parse(R"({"kind":"column","children":[
      {"kind":"row","children":[{"kind":"text","text":"math.PR · 12 left","style":"label"},{"kind":"button","id":"feeds","title":"Feeds"}]},
      {"kind":"deck","id":"card","card":{"kind":"card","title":"Gaussian random fields on manifolds via the Laplace–Beltrami operator",
        "meta":"Adler, Taylor, Lang et al. · 2024-01-03","badge":"In library"}},
      {"kind":"text","text":"File into","style":"label"},
      {"kind":"chips","id":"recent","select":"none","chips":[{"id":"k1","title":"Probability / SPDEs"},{"id":"k2","title":"Reading"}]},
      {"kind":"buttons","buttons":[{"id":"discard","title":"Discard"},{"id":"skip","title":"Skip"},{"id":"keep","title":"Keep","primary":true}]},
      {"kind":"buttons","buttons":[{"id":"undo","title":"Undo"},{"id":"done","title":"Done"}]},
      {"kind":"notice","text":"Discarded “Brownian loops and the central charge”"}
    ]})");
    j["children"][1]["card"]["body"] = body;
    return j;
}

json fileScreen(int collections = 5) {
    json rows = json::array();
    const char* names[] = {"Probability / SPDEs", "Probability / Random fields", "Reading", "Geometry / Manifolds", "Teaching"};
    for (int i = 0; i < collections; ++i)
        rows.push_back({{"id", "c" + std::to_string(i)},
                        {"title", i < 5 ? std::string(names[i]) : "Collection " + std::to_string(i)},
                        {"selected", i == 0}});
    rows[1]["badge"] = "already in";
    json j = json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"Gaussian random fields on manifolds via the Laplace–Beltrami operator","style":"title","lines":2},
      {"kind":"notice","text":"Already in your library — filing adds a second item. The old one holds 2 notes."},
      {"kind":"chips","id":"replace","chips":[{"id":"trash","title":"Move the old one to the trash"}],"selected":[]},
      {"kind":"search","id":"find","placeholder":"Collection","focus":true},
      {"kind":"list","id":"collections","rows":[],"empty":"No collection matches"},
      {"kind":"text","text":"Tags","style":"label"},
      {"kind":"chips","id":"tags","chips":["to-read","spde","random-fields"],"selected":["to-read","spde"]},
      {"kind":"field","id":"tag","placeholder":"Add a tag","value":""},
      {"kind":"field","id":"note","label":"Note","lines":3},
      {"kind":"button","id":"file","title":"File into Probability / SPDEs","primary":true}
    ]})");
    j["children"][4]["rows"] = rows;
    return j;
}

json gallery() {
    return json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"Every block","style":"title"},
      {"kind":"text","text":["Body text with ",{"text":"bold","bold":true}," and ",{"text":"italic","italic":true}," stretches; it wraps at word boundaries like the rest of the app's text."]},
      {"kind":"text","text":"A label","style":"label"},
      {"kind":"card","title":"A card alone","meta":"Its meta line","body":"A short body.","badge":"New"},
      {"kind":"list","id":"l","rows":[{"id":"a","title":"All feeds","meta":"42","selected":true},{"id":"b","title":"math.PR","meta":"12","badge":"new"}]},
      {"kind":"row","children":[{"kind":"text","text":"A row: text and a button","style":"meta"},{"kind":"button","id":"go","title":"Go"}]},
      {"kind":"search","id":"s","placeholder":"Search…"},
      {"kind":"chips","id":"c","chips":["one","two","three"],"selected":["two"]},
      {"kind":"divider"},
      {"kind":"button","id":"b1","title":"A button"},
      {"kind":"space","size":"small"},
      {"kind":"notice","text":"A quiet notice."}
    ]})");
}

void collectRects(const Laid& l, std::vector<Rect>& out) {
    out.push_back(l.rect);
    for (const auto& p : l.parts) out.push_back(p.rect);
    for (const auto& c : l.children) collectRects(c, out);
}

bool inside(Rect r, Rect area) { return r.empty() || (r.x >= area.x && r.y >= area.y && r.x + r.w <= area.x + area.w && r.y + r.h <= area.y + area.h); }

// Nothing outside the area, in the layout or in pixels, on any page; and every unit of every
// top-level block shown on some page.
void checkFits(const Block& root, ItemSource* src = nullptr) {
    const Rect area = blocksArea();
    const Laid first = layoutTree(root, area, 0, src);
    std::map<const Block*, std::set<int>> shown;
    std::set<const Block*> seen;
    Canvas c(metrics().width, metrics().height);
    for (int p = 0; p < first.pages; ++p) {
        const Laid l = layoutTree(root, area, p, src);
        CHECK(l.page == p);
        CHECK(l.pages == first.pages);
        std::vector<Rect> rects;
        collectRects(l, rects);
        for (Rect r : rects) {
            INFO(r.x << "," << r.y << " " << r.w << "x" << r.h << " page " << p);
            CHECK(inside(r, area));
        }
        for (const auto& k : l.children) {
            seen.insert(k.block);
            for (int u = k.first; u < k.first + k.count; ++u) shown[k.block].insert(u);
        }
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 128);
        drawTree(c.cr(), l, {}, src);
        const auto g = grayOf(c);
        size_t outside = 0;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                if (!area.contains({x, y}) && g[(size_t)y * c.width() + x] != 128) ++outside;
        CHECK(outside == 0);
    }
    REQUIRE(root.kind == K::Column);
    for (const auto& k : root.children) {
        CHECK(seen.count(&k));
        // count the units the block has: the most any page showed when laid alone, tall
        const Laid alone = layoutTree(k, {0, 0, area.w, 1 << 28}, 0, src);
        REQUIRE(alone.children.size() == 1);
        const int units = alone.children[0].count;
        CHECK(static_cast<int>(shown[&k].size()) == units);
    }
}

// Items by key from a table, as the library has them; keys not in it (deleted, a switched-off group) are skipped.
struct FakeItems : ItemSource {
    std::map<std::string, ItemRow> rows;
    std::map<std::string, std::string> download;
    int resolved = 0;
    std::optional<ItemRow> row(const ItemRef& r) override {
        ++resolved;
        auto it = rows.find(r.key);
        if (it == rows.end() || (r.library && it->second.lib != *r.library)) return std::nullopt;
        return it->second;
    }
    std::string cardMeta(const ItemRow& r) override {
        return r.progress > 0 ? std::to_string(static_cast<int>(r.progress * 100 + 0.5)) + "% · 3 highlights · opened today" : "new";
    }
    std::string status(const std::string& a) override { return download.count(a) ? download[a] : ""; }
    ItemRow& add(const std::string& key, const std::string& title, double progress = -1, const std::string& badge = "EPUB") {
        ItemRow& r = rows[key];
        r.key = key, r.title = title, r.byline = "Adler · 2024", r.badge = badge, r.attachmentKey = "A" + key;
        r.fileState = AttachmentRecord::State::Local, r.progress = progress, r.itemType = "book", r.files = 1;
        return r;
    }
};

json itemsScreen() {
    return json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"Reading","style":"title"},
      {"kind":"items","id":"now","style":"card","items":[{"key":"B1","label":"BOOK · CONTINUE"}]},
      {"kind":"text","text":"Recent","style":"label"},
      {"kind":"items","id":"recent","style":"row","items":["P1","GONE","P2",{"key":"P3","library":12}]},
      {"kind":"items","id":"none","style":"row","items":["GONE","ALSO"],"empty":"Nothing opened yet"}
    ]})");
}

void fillItems(FakeItems& f) {
    f.add("B1", "The Brothers Karamazov", 0.42);
    f.add("P1", "Gaussian random fields on compact Riemannian manifolds", 0.13, "PDF");
    f.add("P2", "Physics-informed neural networks for the Hamilton–Jacobi–Bellman equation", -1, "PDF");
    ItemRow& p3 = f.add("P3", "A group paper", 1.0, "PDF");
    p3.lib = 12, p3.read = true;
}

}  // namespace

TEST_CASE("plugin blocks: goldens on PW5 — every block, the deck, the filing screen") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    const auto shot = [&](const Block& t, const char* title, const char* path, int page = 0, FocusState f = {}) {
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        drawHeader(c.cr(), title, true);
        drawTree(c.cr(), layoutTree(t, blocksArea(), page), f);
        checkGolden(c, path);
    };
    shot(tree(gallery()), "Every block", "tests/golden/plugin-blocks-gallery.png");
    shot(tree(deckScreen()), "Feed Riffle", "tests/golden/plugin-blocks-deck.png");
    FocusState typing;
    typing.focused = "find";
    typing.texts["find"] = "prob";
    shot(tree(fileScreen()), "Keep", "tests/golden/plugin-blocks-file.png", 0, typing);
    shot(tree(json::parse(R"({"kind":"deck","id":"d","empty":"Nothing left to riffle"})")), "Feed Riffle",
         "tests/golden/plugin-blocks-empty-deck.png");
    std::string longBody;
    for (int i = 0; i < 40; ++i) longBody += std::string(kSummary) + (i % 3 == 2 ? "\n\n" : " ");
    shot(tree(deckScreen(longBody)), "Feed Riffle", "tests/golden/plugin-blocks-deck-page2.png", 1);
}

TEST_CASE("plugin blocks: goldens on PW5 — items as a card and rows, and the empty text") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    FakeItems items;
    fillItems(items);
    items.download["AP2"] = "Downloading 42%";
    const auto shot = [&](const json& j, const char* path) {
        const Block t = tree(j);
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        drawHeader(c.cr(), "Reading Desk", true);
        drawTree(c.cr(), layoutTree(t, blocksArea(), 0, &items), {}, &items);
        checkGolden(c, path);
    };
    shot(itemsScreen(), "tests/golden/plugin-items-card-rows.png");
    shot(json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"Pinned","style":"title"},
      {"kind":"items","id":"pins","style":"card","items":["GONE"],"empty":"Nothing pinned yet. Long-press an item to pin it."}]})"),
         "tests/golden/plugin-items-empty.png");
}

TEST_CASE("plugin items: unknown and switched-off keys are skipped; no source draws nothing") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items;
    fillItems(items);
    const Block t = tree(itemsScreen());
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    REQUIRE(l.children.size() == 5);
    const Laid& recent = l.children[3];
    REQUIRE(recent.items.size() == 3);  // GONE skipped
    CHECK(recent.items[0].key == "P1");
    CHECK(recent.items[1].key == "P2");
    CHECK(recent.items[2].key == "P3");
    CHECK(recent.count == 3);
    CHECK(l.children[1].items.size() == 1);
    CHECK(l.children[1].itemLabels[0] == "BOOK · CONTINUE");
    CHECK(l.children[4].items.empty());
    CHECK(l.children[4].rect.h == 112);  // its `empty` text
    // a ref naming the wrong library is skipped too
    FakeItems other;
    fillItems(other);
    other.rows["P3"].lib = 0;
    CHECK(layoutTree(t, blocksArea(), 0, &other).children[3].items.size() == 2);
    // each block resolved once per layout
    items.resolved = 0;
    layoutTree(t, blocksArea(), 0, &items);
    CHECK(items.resolved == 1 + 4 + 2);
    // no source: nothing takes room, nothing is drawn
    const Laid bare = layoutTree(t, blocksArea(), 0);
    for (int i : {1, 3, 4}) {
        CHECK(bare.children[i].rect.h == 0);
        CHECK(bare.children[i].parts.empty());
    }
    Canvas c(kPw5.width, kPw5.height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawTree(c.cr(), bare, {});
}

TEST_CASE("plugin items: 300 refs page, nothing outside the area, on KT4") {
    if (!fonts()) return;
    for (const char* style : {"row", "card"}) {
        ScopedMetrics m(kKt4);
        FakeItems items;
        json refs = json::array();
        for (int i = 0; i < 300; ++i) {
            const std::string k = "K" + std::to_string(i);
            items.add(k, "Item number " + std::to_string(i) + " with a title long enough to wrap onto a second line in the row", i / 300.0);
            refs.push_back(i % 7 == 3 ? json("MISSING" + std::to_string(i)) : json(k));
        }
        json j = json::parse(R"({"kind":"column","children":[{"kind":"items","id":"all","style":"row","items":[]},
          {"kind":"button","id":"more","title":"More"}]})");
        j["children"][0]["items"] = refs;
        j["children"][0]["style"] = style;
        const Block t = tree(j);
        const Laid l = layoutTree(t, blocksArea(), 3, &items);
        INFO(style);
        CHECK(l.pages > 10);
        CHECK(l.children[0].count > 0);
        CHECK(l.children[0].items.size() == 300 - 43);
        checkFits(t, &items);
    }
}

TEST_CASE("plugin items: hit-testing a row or a card returns the ItemRow") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items;
    fillItems(items);
    const Block t = tree(itemsScreen());
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    const Laid& card = l.children[1];
    REQUIRE(card.parts.size() == 1);
    auto h = hitTest(l, card.parts[0].rect.center());
    REQUIRE(h);
    CHECK(h->kind == K::Items);
    CHECK(h->id == "now");
    CHECK(h->part == "B1");
    REQUIRE(h->item);
    CHECK(h->item->title == "The Brothers Karamazov");
    const Laid& recent = l.children[3];
    REQUIRE(recent.parts.size() == 3);
    h = hitTest(l, recent.parts[2].rect.center());
    REQUIRE(h);
    CHECK(h->id == "recent");
    REQUIRE(h->item);
    CHECK(h->item->key == "P3");
    CHECK(h->item->lib == 12);
    CHECK(h->rect == recent.parts[2].rect);
    CHECK_FALSE(hitTest(l, l.children[2].rect.center()));  // a label
}

TEST_CASE("plugin blocks: everything fits and is reachable on PW5, KT4 and Scribe") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        checkFits(tree(gallery()));
        checkFits(tree(deckScreen()));
        checkFits(tree(fileScreen()));
        checkFits(tree(fileScreen(40)));
    }
}

TEST_CASE("plugin blocks: a 10,000-character card body pages with the buttons kept on KT4") {
    if (!fonts()) return;
    ScopedMetrics m(kKt4);
    std::string body;
    while (body.size() < 10000) body += std::string(kSummary) + "\n\n";
    const Block t = tree(deckScreen(body));
    const Laid l0 = layoutTree(t, blocksArea(), 0);
    CHECK(l0.pages > 3);
    for (int p = 0; p < l0.pages; ++p) {
        const Laid l = layoutTree(t, blocksArea(), p);
        REQUIRE(l.children.size() == 7);  // elastic: every block on every page
        CHECK(hitTest(l, l.children[4].parts[2].rect.center())->id == "keep");
    }
    checkFits(t);
    // Past the end: the last page.
    CHECK(layoutTree(t, blocksArea(), 999).page == l0.pages - 1);
}

TEST_CASE("plugin blocks: a 300-row list pages; rows hit-test to their ids") {
    if (!fonts()) return;
    ScopedMetrics m(kKt4);
    json rows = json::array();
    for (int i = 0; i < 300; ++i) rows.push_back({{"id", "r" + std::to_string(i)}, {"title", "Row " + std::to_string(i)}});
    json j = json::parse(R"({"kind":"column","children":[{"kind":"list","id":"rows","rows":[]},
      {"kind":"button","id":"fetch","title":"Fetch now"}]})");
    j["children"][0]["rows"] = rows;
    const Block t = tree(j);
    const Laid l = layoutTree(t, blocksArea(), 2);
    CHECK(l.pages > 10);
    const Laid& list = l.children[0];
    REQUIRE(list.count > 0);
    const auto hit = hitTest(l, list.parts[1].rect.center());
    REQUIRE(hit);
    CHECK(hit->id == "rows");
    CHECK(hit->part == "r" + std::to_string(list.first + 1));
    CHECK(hitTest(l, l.children[1].rect.center())->id == "fetch");
    CHECK(pageIndicatorRect(l).y + pageIndicatorRect(l).h <= blocksArea().y + blocksArea().h);
    checkFits(t);
}

TEST_CASE("plugin blocks: hit-testing buttons, chips, search and the deck") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = tree(deckScreen());
    const Laid l = layoutTree(t, blocksArea(), 0);
    REQUIRE(l.pages == 1);
    CHECK(hitTest(l, l.children[0].children[1].rect.center())->id == "feeds");
    CHECK(hitTest(l, l.children[1].rect.center())->id == "card");
    CHECK(hitTest(l, l.children[1].rect.center())->kind == K::Deck);
    const auto chip = hitTest(l, l.children[3].parts[1].rect.center());
    REQUIRE(chip);
    CHECK(chip->id == "recent");
    CHECK(chip->part == "k2");
    CHECK(hitTest(l, l.children[4].parts[0].rect.center())->id == "discard");
    CHECK(hitTest(l, l.children[5].parts[1].rect.center())->id == "done");
    CHECK_FALSE(hitTest(l, l.children[6].rect.center()));  // a notice
    const Block f = tree(fileScreen());
    const Laid lf = layoutTree(f, blocksArea(), 0);
    REQUIRE(findLaid(lf, "find"));
    CHECK(hitTest(lf, findLaid(lf, "find")->rect.center())->kind == K::Search);
    CHECK(findLaid(lf, "nope") == nullptr);
}

TEST_CASE("plugin blocks: a row of wide buttons leaves its text room to read") {
    if (!fonts()) return;
    ScopedMetrics m(kKt4);
    const Block t = tree(json::parse(R"({"kind":"row","children":[
      {"kind":"text","text":"Some words that should wrap by words"},
      {"kind":"button","id":"a","title":"A very long button title"},{"kind":"button","id":"b","title":"Another long button title"},
      {"kind":"button","id":"c","title":"A third long button title"},{"kind":"button","id":"d","title":"A fourth long button"},
      {"kind":"button","id":"e","title":"A fifth long button title"}]})"));
    const Laid l = layoutTree(t, blocksArea(), 0);
    REQUIRE(l.children.size() == 1);
    const Laid& row = l.children[0];
    REQUIRE(row.children.size() == 6);
    CHECK(row.children[0].rect.w >= std::min(240, blocksArea().w / 6));
    for (const auto& c : row.children) CHECK(c.rect.x + c.rect.w <= blocksArea().x + blocksArea().w);
}

// ---- 0.7 fix: a text block is laid out once, not on every draw ----------------------------------

namespace {

json longText(const std::string& word, const char* style = "body") {
    std::string text;
    for (int i = 0; i < 3000; ++i) text += word + " " + std::to_string(i) + (i % 40 == 39 ? "\n\n" : " ");
    return json{{"kind", "column"},
                {"children", json::array({{{"kind", "text"}, {"text", text}, {"style", style}},
                                          {{"kind", "button"}, {"id", "ok"}, {"title", "OK"}}})}};
}


}  // namespace

TEST_CASE("plugin blocks fix: a long text block is laid out once; redraws, page turns and an unchanged new tree reuse it") {
    fonts();
    const Rect area = blocksArea();
    const Block t1 = tree(longText("alpha"));
    const int64_t start = textBlockLayouts();
    const Laid a = layoutTree(t1, area, 0);
    CHECK(textBlockLayouts() - start == 1);  // measured several times while laying out, laid out once
    REQUIRE(a.pages > 2);
    Canvas c(metrics().width, metrics().height);
    drawTree(c.cr(), a, {});
    drawTree(c.cr(), a, {});
    CHECK(textBlockLayouts() - start == 1);  // drawing doesn't lay it out again
    const Laid p2 = layoutTree(t1, area, 2);
    drawTree(c.cr(), p2, {});
    CHECK(textBlockLayouts() - start == 1);  // nor does a page turn
    const Block same = tree(longText("alpha"));  // the plugin rendered again, nothing changed
    const Laid b = layoutTree(same, area, 0);
    drawTree(c.cr(), b, {});
    CHECK(textBlockLayouts() - start == 1);

    const Block changed = tree(longText("beta"));
    const Laid d = layoutTree(changed, area, 0);
    CHECK(textBlockLayouts() - start == 2);  // the text changed
    const Block restyled = tree(longText("beta", "meta"));
    const Laid e = layoutTree(restyled, area, 0);
    CHECK(textBlockLayouts() - start == 3);  // the style changed
    const Laid f = layoutTree(restyled, {area.x, area.y, area.w - 100, area.h}, 0);
    CHECK(textBlockLayouts() - start == 4);  // the width changed
    drawTree(c.cr(), d, {});
    drawTree(c.cr(), e, {});
    drawTree(c.cr(), f, {});
    CHECK(textBlockLayouts() - start == 4);
}

TEST_CASE("plugin blocks fix: kept text draws exactly as text laid out for the canvas") {
    if (!fonts()) return;
    const TextStyle st{"Bookerly", 9, 400, false, 0, true};
    const std::string text = "Gaussian <i>random</i> fields <b>on manifolds</b>, " + std::string(kSummary);
    const Rect box{40, 60, 900, 1200};
    Canvas want(1000, 1400), got(1000, 1400);
    drawTextLines(want.cr(), text, st, box, 2, 5);
    KeptText(text, st, box.w).drawLines(got.cr(), box, 2, 5);
    CHECK(grayOf(want) == grayOf(got));
    Canvas want3(1000, 1400), got3(1000, 1400);  // a block with `lines`: ellipsized
    drawText(want3.cr(), text, st, box, 3);
    KeptText(text, st, box.w, 3).drawLines(got3.cr(), box, 0, 3);
    CHECK(grayOf(want3) == grayOf(got3));
    CHECK(KeptText(text, st, box.w).tops() == lineTops(text, st, box.w));
}

// ---- 0.9: a deck fills the screen ----------------------------------------------------------------

namespace {

std::string longBody(int chars) {
    std::string body;
    while (static_cast<int>(body.size()) < chars) body += std::string(kSummary) + "\n\n";
    return body;
}

json withoutDeckCard(json j) {
    j["children"][1] = json::parse(R"({"kind":"deck","id":"card","empty":"Nothing left to riffle"})");
    return j;
}

int bottomOf(const Laid& l) { return l.rect.y + l.rect.h; }

}  // namespace

TEST_CASE("plugin blocks: a deck takes the height left; a short card, a long card and no card have the same size") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        const Rect area = blocksArea();
        const Block shortCard = tree(deckScreen("A short summary.")), noBody = tree(deckScreen(""));
        const Block longCard = tree(deckScreen(longBody(6000))), none = tree(withoutDeckCard(deckScreen()));
        const Laid s = layoutTree(shortCard, area, 0), n = layoutTree(noBody, area, 0);
        const Laid l = layoutTree(longCard, area, 0), e = layoutTree(none, area, 0);
        for (const Laid* x : {&s, &n, &l, &e}) REQUIRE(x->children.size() == 7);
        // The deck reaches down to the blocks after it, which sit at the bottom of the area.
        CHECK(bottomOf(s.children.back()) == area.y + area.h);
        CHECK(s.children[1].rect.h > area.h / 3);
        for (const Laid* x : {&n, &l, &e}) {
            CHECK(x->children[1].rect == s.children[1].rect);
            for (size_t i = 0; i < 7; ++i) CHECK(x->children[i].rect == s.children[i].rect);  // nothing moves
            CHECK(x->children[4].parts == s.children[4].parts);  // the buttons never move
        }
        CHECK(s.pages == 1);
        CHECK(l.pages > 1);
        CHECK(pageIndicatorRect(s).empty());
        // The page indicator sits in the card's bottom corner, not under the blocks.
        const Rect corner = pageIndicatorRect(l);
        CHECK(!corner.empty());
        CHECK(l.children[1].rect.contains(corner.center()));
        CHECK(corner.x > l.children[1].rect.x + l.children[1].rect.w / 2);
        checkFits(longCard);
        checkFits(shortCard);
        checkFits(none);
    }
}

TEST_CASE("plugin blocks: a long card pages inside the card; every page keeps every block in place") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Rect area = blocksArea();
    const Block t = tree(deckScreen(longBody(8000)));
    const Laid first = layoutTree(t, area, 0);
    REQUIRE(first.pages > 2);
    int shown = 0;
    for (int p = 0; p < first.pages; ++p) {
        const Laid l = layoutTree(t, area, p);
        REQUIRE(l.children.size() == 7);
        for (size_t i = 0; i < 7; ++i) CHECK(l.children[i].rect == first.children[i].rect);
        CHECK(l.children[1].first == shown);  // the card's lines, page after page
        shown += l.children[1].count;
        CHECK(hitTest(l, l.children[1].rect.center())->kind == K::Deck);
    }
    const Laid alone = layoutTree(t.children[1], {0, 0, area.w, 1 << 28}, 0);
    CHECK(shown == alone.children[0].count);
}

TEST_CASE("plugin blocks: screens without a deck are laid out as before") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = tree(json::parse(R"({"kind":"column","children":[
      {"kind":"card","title":"A card alone","body":"Short."},
      {"kind":"button","id":"b","title":"OK"}]})"));
    const Laid l = layoutTree(t, blocksArea(), 0);
    REQUIRE(l.children.size() == 2);
    CHECK(l.children[0].rect.h < blocksArea().h / 3);  // a card takes its own height
    CHECK(bottomOf(l.children[1]) < blocksArea().y + blocksArea().h / 2);
}

TEST_CASE("plugin items fix: a card shows its download's status, as a row does") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    FakeItems items;
    fillItems(items);
    const Block t = tree(itemsScreen());
    const auto shot = [&] {
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        const Laid l = layoutTree(t, blocksArea(), 0, &items);
        drawTree(c.cr(), l, {}, &items);
        const Rect card = l.children[1].parts.at(0).rect;
        std::vector<uint8_t> px(static_cast<size_t>(c.width()) * static_cast<size_t>(c.height()));
        c.toGray(px.data(), c.width(), card);
        return px;
    };
    const auto plain = shot();
    items.download["AB1"] = "Downloading 42%";
    const bool same = shot() == plain;
    CHECK_FALSE(same);
}

// ---- quote (0.11) ----------------------------------------------------------------------------------

namespace {

const char* kPride =
    "It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife. "
    "However little known the feelings or views of such a man may be on his first entering a neighbourhood, this truth is so "
    "well fixed in the minds of the surrounding families, that he is considered as the rightful property of some one or other "
    "of their daughters. “My dear Mr. Bennet,” said his lady to him one day, “have you heard that Netherfield Park is let at last?”";

json quoteScreen() {
    json j = json::parse(R"({"kind":"column","gap":"medium","children":[
      {"kind":"text","text":"A passage","style":"label"},
      {"kind":"quote","id":"passage","text":"It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife.","source":"Pride and Prejudice · p. 1"},
      {"kind":"quote","text":"Call me Ishmael."},
      {"kind":"quote","id":"long","text":"","source":"Pride and Prejudice · Austen · p. 1"},
      {"kind":"button","id":"more","title":"Another"}]})");
    j["children"][3]["text"] = kPride;
    return j;
}

}  // namespace

TEST_CASE("plugin blocks: golden on PW5 — quotes with and without a source, a long one ellipsized at 5 lines") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawHeader(c.cr(), "Reading Desk", true);
    const Block t = tree(quoteScreen());
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0), {});
    checkGolden(c, "tests/golden/plugin-blocks-quote.png");
}

TEST_CASE("plugin blocks: a quote is at most 5 lines and its source tall; a short one only as tall as its text") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        const Block t = tree(quoteScreen());
        const Laid l = layoutTree(t, blocksArea(), 0);
        REQUIRE(l.pages == 1);
        REQUIRE(l.children.size() == 5);
        const int shortH = l.children[2].rect.h, longH = l.children[3].rect.h;
        CHECK(shortH > 0);
        CHECK(longH > shortH);
        // the long one's text alone would be far more than 5 lines: the block stops at 5 and its source
        json more = quoteScreen();
        more["children"][3]["text"] = std::string(kPride) + " " + kPride + " " + kPride;
        const Block t2 = tree(more);
        CHECK(layoutTree(t2, blocksArea(), 0).children[3].rect.h == longH);
        checkFits(t);
        checkFits(t2);
    }
}

TEST_CASE("plugin blocks: a quote with an id is tapped as itself; one without is not") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = tree(quoteScreen());
    const Laid l = layoutTree(t, blocksArea(), 0);
    const auto hit = hitTest(l, l.children[1].rect.center());
    REQUIRE(hit);
    CHECK(hit->id == "passage");
    CHECK(hit->kind == K::Quote);
    CHECK(hit->rect == l.children[1].rect);
    CHECK_FALSE(hitTest(l, l.children[2].rect.center()));
}

// ---- shelf (0.11) ----------------------------------------------------------------------------------

namespace {

// `n` books: some opened, one read, one new, a PDF and a web page among them.
FakeItems shelfItems(int n) {
    FakeItems f;
    const char* titles[] = {"Pride and Prejudice", "Moby-Dick; or, The Whale", "The Brothers Karamazov",
                            "Gaussian random fields on compact Riemannian manifolds", "Middlemarch", "Anna Karenina",
                            "A web page about the Laplace–Beltrami operator", "Bleak House"};
    for (int i = 0; i < n; ++i) {
        const std::string k = "S" + std::to_string(i);
        const int t = i % 8;
        ItemRow& r = f.add(k, titles[t], t == 0 ? 0.13 : t == 1 ? 0.62 : t == 2 ? 0.05 : -1, t == 3 ? "PDF" : t == 6 ? "WEB" : "EPUB");
        r.byline = t == 0 ? "Jane Austen · 1813" : t == 1 ? "Herman Melville · 1851" : "Adler · 2024";
        if (t == 4) r.read = true, r.progress = 1.0;
        if (t == 3) r.itemType = "journalArticle", r.publication = "Probab. Theory Relat. Fields", r.year = 2024;  // a paper
        if (t == 6) r.itemType = "webpage";  // a paper without a venue: its type's name
    }
    return f;
}

json shelfScreen(int n) {
    json j = json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"On the shelf","style":"label"},
      {"kind":"items","id":"shelf","style":"shelf","items":[]},
      {"kind":"button","id":"more","title":"More"}]})");
    for (int i = 0; i < n; ++i) j["children"][1]["items"].push_back("S" + std::to_string(i));
    return j;
}

// The tiles of a laid shelf in its first row.
std::vector<Rect> firstRow(const Laid& shelf) {
    std::vector<Rect> out;
    for (const auto& p : shelf.parts)
        if (p.rect.y == shelf.parts.at(0).rect.y) out.push_back(p.rect);
    return out;
}

}  // namespace

TEST_CASE("plugin shelf: tiles per row from the width — PW5 4, KT4 4, Scribe 6; ~240 px (200 at least), gaps of 32 px or more") {
    if (!fonts()) return;
    CHECK(shelfColumns(kPw5.width - 2 * kMargin) == 4);
    CHECK(shelfColumns(kKt4.width - 2 * kMargin) == 4);
    CHECK(shelfColumns(400) == 1);  // two tiles would be under 200 px
    CHECK(shelfColumns(kScribe.width - 2 * kMargin) == 6);
    CHECK(shelfColumns(200) == 1);  // never none
    for (const auto& [dev, cols] : {std::pair{&kPw5, 4}, std::pair{&kKt4, 4}, std::pair{&kScribe, 6}}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        FakeItems items = shelfItems(30);
        const Block t = tree(shelfScreen(30));
        const Laid l = layoutTree(t, blocksArea(), 0, &items);
        const Laid& shelf = l.children[1];
        const auto row = firstRow(shelf);
        REQUIRE(static_cast<int>(row.size()) == cols);
        for (size_t i = 0; i < row.size(); ++i) {
            CHECK(row[i].w == row[0].w);
            CHECK(row[i].w >= 200);
            CHECK(row[i].w <= 320);
            if (i) CHECK(row[i].x - (row[i - 1].x + row[i - 1].w) >= 32);
        }
        CHECK(row.front().x == shelf.rect.x);  // the row spans the content width
        CHECK(row.back().x + row.back().w == shelf.rect.x + shelf.rect.w);
        CHECK(row[0].h > row[0].w * 3 / 2);  // a 2:3 cover and the lines under it
    }
}

TEST_CASE("plugin shelf: 30 items page by rows; every item shown once, the button kept, nothing outside") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        FakeItems items = shelfItems(30);
        const Block t = tree(shelfScreen(30));
        const Laid first = layoutTree(t, blocksArea(), 0, &items);
        CHECK(first.pages > 1);
        const int cols = shelfColumns(blocksArea().w);
        CHECK(first.children[1].first == 0);
        std::map<std::string, int> seen;
        Rect button = first.children[2].rect;
        for (int p = 0; p < first.pages; ++p) {
            const Laid l = layoutTree(t, blocksArea(), p, &items);
            const Laid& shelf = l.children[1];
            CHECK(shelf.parts.size() <= static_cast<size_t>(shelf.count * cols));
            CHECK(shelf.parts.at(0).index == shelf.first * cols);  // a page starts on a row
            for (const auto& part : shelf.parts) {
                REQUIRE(part.item);
                CHECK(part.item->key == shelf.items[part.index].key);
                ++seen[part.item->key];
            }
            CHECK(l.children[2].rect == button);
        }
        CHECK(seen.size() == 30);
        for (const auto& [k, n] : seen) CHECK(n == 1);
        checkFits(t, &items);
    }
}

TEST_CASE("plugin shelf: hit-testing a tile returns its ItemRow; the gap between tiles is no item") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(6);
    const Block t = tree(shelfScreen(6));
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    const Laid& shelf = l.children[1];
    REQUIRE(shelf.parts.size() == 6);
    const auto h = hitTest(l, shelf.parts[5].rect.center());
    REQUIRE(h);
    CHECK(h->kind == K::Items);
    CHECK(h->id == "shelf");
    CHECK(h->part == "S5");
    REQUIRE(h->item);
    CHECK(h->item->title == "Anna Karenina");
    CHECK(h->rect == shelf.parts[5].rect);
    const Rect a = shelf.parts[0].rect, b = shelf.parts[1].rect;
    const auto gap = hitTest(l, {(a.x + a.w + b.x) / 2, a.y + a.h / 2});
    CHECK_FALSE(gap);
}

TEST_CASE("plugin shelf: a tile's line — the progress, New, Read; a download's status in its place") {
    ItemRow r;
    r.progress = 0.13;
    CHECK(shelfMeta(r) == "13%");
    r.progress = 0;
    CHECK(shelfMeta(r) == "0%");
    r.progress = -1;
    CHECK(shelfMeta(r) == "New");
    r.read = true;
    CHECK(shelfMeta(r) == "Read");
    r.progress = 0.4;
    CHECK(shelfMeta(r) == "Read");

    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    FakeItems items = shelfItems(4);
    const Block t = tree(shelfScreen(4));
    const auto shot = [&] {
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        const Laid l = layoutTree(t, blocksArea(), 0, &items);
        drawTree(c.cr(), l, {}, &items);
        std::vector<uint8_t> px(static_cast<size_t>(c.width()) * static_cast<size_t>(c.height()));
        c.toGray(px.data(), c.width(), l.children[1].parts.at(1).rect);
        return px;
    };
    const auto plain = shot();
    items.download["AS1"] = "Downloading 42%";
    const bool same = shot() == plain;
    CHECK_FALSE(same);
    CHECK(layoutTree(t, blocksArea(), 0, &items).children[1].itemMetas.at(1) == "62%");
}

TEST_CASE("plugin shelf: golden on PW5 — covers, bars, New / Read / %, a PDF and a web mark, a download") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    FakeItems items = shelfItems(8);
    items.download["AS5"] = "Downloading 42%";
    const Block t = tree(shelfScreen(8));
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawHeader(c.cr(), "Reading Desk", true);
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0, &items), {}, &items);
    checkGolden(c, "tests/golden/plugin-items-shelf.png");
}

// ---- primitives (0.12): widths, alignment, spacer, box, styled text --------------------------------

namespace {

// `children` in a row (`opts` merged into it), the row alone in a column.
Block rowTree(const json& children, const json& opts = json::object()) {
    json row = {{"kind", "row"}, {"children", children}};
    row.update(opts);
    return tree({{"kind", "column"}, {"children", json::array({row})}});
}

const TextStyle kBodyText{"Bookerly", 9, 400, false, 0, true};

// A box as tall as a cover beside it (a column of lines inside it, padded), 35% of the row.
json coverBox() {
    return json::parse(R"({"kind":"box","width":"35%","fill":"ink","pad":"large",
      "child":{"kind":"text","text":"Pride and Prejudice\n\n\n\n\n\n\n\nJane Austen","style":"title"}})");
}

// A canvas with only `t` drawn in it.
std::vector<uint8_t> drawn(const Block& t) {
    Canvas c(kPw5.width, kPw5.height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0), {});
    return grayOf(c);
}

}  // namespace

TEST_CASE("plugin primitives: a row's widths — a percent of the row, auto its natural width, fill the rest") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = rowTree(json::parse(R"([{"kind":"text","text":"Thirty","width":"30%"},{"kind":"text","text":"The rest of the row"},
                                           {"kind":"text","text":"All 9 ›","width":"auto"}])"));
    const Rect area = blocksArea();
    const Laid l = layoutTree(t, area, 0);
    const auto& kids = l.children.at(0).children;
    REQUIRE(kids.size() == 3);
    const int room = area.w - 2 * 18;  // the row less its two small gaps
    CHECK(kids[0].rect.w == room * 30 / 100);
    const int natural = measureText("All 9 ›", kBodyText, area.w, 0).width;
    CHECK(kids[2].rect.w >= natural);
    CHECK(kids[2].rect.w <= natural + 4);
    CHECK(kids[1].rect.w == room - kids[0].rect.w - kids[2].rect.w);
    CHECK(kids[0].rect.x == area.x);
    CHECK(kids[1].rect.x == kids[0].rect.x + kids[0].rect.w + 18);
    CHECK(kids[2].rect.x + kids[2].rect.w == area.x + area.w);
    // auto text is laid out on one line at that width
    CHECK(kids[2].rect.h == measureText("All 9 ›", kBodyText, area.w, 0).height);
}

TEST_CASE("plugin primitives: percents over 100% scale down to fit; an auto keeps its width") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = rowTree(json::parse(R"([{"kind":"text","text":"A","width":"100%"},{"kind":"text","text":"B","width":"50%"},
                                           {"kind":"text","text":"All ›","width":"auto"}])"));
    const Rect area = blocksArea();
    const Laid l = layoutTree(t, area, 0);
    const auto& kids = l.children.at(0).children;
    REQUIRE(kids.size() == 3);
    const int natural = measureText("All ›", kBodyText, area.w, 0).width;
    CHECK(kids[2].rect.w >= natural);
    CHECK(std::abs(kids[0].rect.w - 2 * kids[1].rect.w) <= 1);
    CHECK(kids[0].rect.w + kids[1].rect.w + kids[2].rect.w + 2 * 18 <= area.w);
    CHECK(kids[0].rect.w + kids[1].rect.w + kids[2].rect.w + 2 * 18 >= area.w - 2);
    CHECK(kids[2].rect.x + kids[2].rect.w <= area.x + area.w);
}

TEST_CASE("plugin primitives: a stretched row — every child the row's height; a spacer pushes the last text to the bottom") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    json children = json::array({coverBox(), json::parse(R"({"kind":"column","children":[
        {"kind":"text","text":"Pride and Prejudice","style":"title"},{"kind":"spacer"},{"kind":"text","text":"13% · today","style":"meta"}]})")});
    const Block t = rowTree(children, {{"align", "stretch"}, {"gap", "medium"}});
    const Laid l = layoutTree(t, blocksArea(), 0);
    const Laid& row = l.children.at(0);
    const Laid& box = row.children.at(0);
    const Laid& col = row.children.at(1);
    CHECK(box.rect.h == row.rect.h);
    CHECK(col.rect.h == row.rect.h);
    REQUIRE(col.children.size() == 3);
    CHECK(col.children[0].rect.y == row.rect.y);
    CHECK(col.children[1].rect.h > 200);  // the spacer took what was left
    const Rect last = col.children[2].rect;
    CHECK(last.y + last.h == row.rect.y + row.rect.h);
    CHECK(last.h > 0);

    // the same column elsewhere: the spacer takes no room
    const Block plain = tree(json::parse(R"({"kind":"column","children":[
        {"kind":"text","text":"Pride and Prejudice","style":"title"},{"kind":"spacer"},{"kind":"text","text":"13% · today","style":"meta"}]})"));
    const Laid p = layoutTree(plain, blocksArea(), 0);
    REQUIRE(p.children.size() == 3);
    CHECK(p.children[1].rect.h == 0);
}

TEST_CASE("plugin primitives: a row's align puts shorter children at the top, centre or bottom") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    for (const char* a : {"top", "center", "bottom"}) {
        INFO(a);
        const Block t = rowTree(json::array({coverBox(), json{{"kind", "text"}, {"text", "Jane Austen"}}}), {{"align", a}});
        const Laid l = layoutTree(t, blocksArea(), 0);
        const Laid& row = l.children.at(0);
        const Rect box = row.children.at(0).rect, text = row.children.at(1).rect;
        CHECK(box.h == row.rect.h);
        CHECK(text.h < row.rect.h / 2);
        if (std::string(a) == "top") CHECK(text.y == row.rect.y);
        if (std::string(a) == "center") CHECK(std::abs((text.y - row.rect.y) - (row.rect.y + row.rect.h - text.y - text.h)) <= 1);
        if (std::string(a) == "bottom") CHECK(text.y + text.h == row.rect.y + row.rect.h);
    }
}

TEST_CASE("plugin primitives: a box insets its child by its border and padding, and is as tall as both") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    struct Case {
        const char* border;
        const char* pad;
        int inset;
    };
    for (const Case& c : {Case{"thin", "medium", 2 + 36}, Case{"thick", "xsmall", 3 + 6}, Case{"none", "none", 0}, Case{"thin", "none", 2}}) {
        INFO(c.border << " " << c.pad);
        const Block t = tree({{"kind", "column"},
                              {"children", json::array({json{{"kind", "box"}, {"border", c.border}, {"pad", c.pad},
                                                             {"child", {{"kind", "text"}, {"text", "Inside a box"}}}}})}});
        const Laid l = layoutTree(t, blocksArea(), 0);
        const Laid& box = l.children.at(0);
        REQUIRE(box.children.size() == 1);
        const Rect in = box.children[0].rect;
        CHECK(in.x == box.rect.x + c.inset);
        CHECK(in.y == box.rect.y + c.inset);
        CHECK(in.w == box.rect.w - 2 * c.inset);
        CHECK(in.h == measureText("Inside a box", kBodyText, in.w, 0).height);
        CHECK(box.rect.h == in.h + 2 * c.inset);
    }
}

TEST_CASE("plugin primitives: caps text is drawn uppercase (and tracked); options change the text's size") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const auto label = [](const std::string& s, bool caps) {
        return tree({{"kind", "column"}, {"children", json::array({json{{"kind", "text"}, {"text", s}, {"style", "label"}, {"caps", caps}}})}});
    };
    const Block lower = label("reading desk", true), upper = label("READING DESK", true), plain = label("READING DESK", false);
    CHECK(drawn(lower) == drawn(upper));
    CHECK(drawn(upper) != drawn(plain));  // tracked

    const auto sized = [](const char* size) {
        return tree({{"kind", "column"}, {"children", json::array({json{{"kind", "text"}, {"text", "Middlemarch"}, {"size", size}}})}});
    };
    const Block xs = sized("xsmall"), ti = sized("title");
    CHECK(layoutTree(xs, blocksArea(), 0).children.at(0).rect.h < layoutTree(ti, blocksArea(), 0).children.at(0).rect.h);
}

TEST_CASE("plugin primitives: a 300-character text on one line stays in its rect, in a column and at auto width in a row") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        std::string longText;
        while (longText.size() < 300) longText += "Mastering the game of Go without human knowledge ";
        const json text = {{"kind", "text"}, {"text", longText}, {"lines", 1}, {"style", "title"}};
        json autoText = text;
        autoText["width"] = "auto";
        const Block t = tree({{"kind", "column"},
                              {"children", json::array({text, json{{"kind", "row"},
                                                                   {"children", json::array({autoText, json{{"kind", "text"},
                                                                                                             {"text", "today"},
                                                                                                             {"width", "auto"}}})}}})}});
        const Rect area = blocksArea();
        const Laid l = layoutTree(t, area, 0);
        const int oneLine = measureText("Xg", {"Bookerly", 11, 700, false, 0, true}, 1000, 1).height;
        CHECK(l.children.at(0).rect.h == oneLine);
        const Laid& row = l.children.at(1);
        CHECK(row.children.at(0).rect.h == oneLine);
        for (const Laid& c : row.children) {
            INFO(c.rect.x << " " << c.rect.w);
            CHECK(inside(c.rect, area));
        }
        checkFits(t);
    }
}

TEST_CASE("plugin primitives: a text or a box with an id is tapped as itself; text without an id is not") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = tree(json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"All 9 ›","id":"all-pinned"},
      {"kind":"text","text":"Just words"},
      {"kind":"box","id":"pinned","border":"thin","pad":"small","child":{"kind":"text","text":"Pinned"}},
      {"kind":"box","border":"thin","child":{"kind":"text","text":"A frame, not a button"}}]})"));
    const Laid l = layoutTree(t, blocksArea(), 0);
    REQUIRE(l.children.size() == 4);
    auto hit = hitTest(l, l.children[0].rect.center());
    REQUIRE(hit);
    CHECK(hit->id == "all-pinned");
    CHECK(hit->kind == K::Text);
    CHECK(hit->rect == l.children[0].rect);
    CHECK_FALSE(hitTest(l, l.children[1].rect.center()));
    hit = hitTest(l, l.children[2].rect.center());
    REQUIRE(hit);
    CHECK(hit->id == "pinned");
    CHECK(hit->kind == K::Box);
    CHECK(hit->rect == l.children[2].rect);
    CHECK_FALSE(hitTest(l, l.children[3].rect.center()));
}

TEST_CASE("plugin primitives: golden on PW5 — styled text") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawHeader(c.cr(), "Reading Desk", true);
    const Block t = tree(json::parse(R"({"kind":"column","gap":"small","children":[
      {"kind":"text","text":"Book","style":"label","caps":true,"size":"small"},
      {"kind":"text","text":"Pride and Prejudice","font":"serif","weight":"bold","size":"title"},
      {"kind":"text","text":"Jane Austen","font":"sans","tone":"secondary","size":"body"},
      {"kind":"text","text":"Inbox · 4 new","caps":true,"font":"sans","weight":"bold","size":"small","tone":"secondary"},
      {"kind":"text","text":"Deep Residual Learning for Image Recognition","font":"serif","weight":"bold","size":"large","lines":1},
      {"kind":"text","text":"He, Zhang, Ren, Sun · 2016","style":"meta"},
      {"kind":"text","text":"An italic aside, medium weight","italic":true,"weight":"medium"},
      {"kind":"text","text":"Centred","align":"center","size":"large"},
      {"kind":"text","text":"At the end","align":"end"},
      {"kind":"text","text":"xsmall · small · body","size":"xsmall"},
      {"kind":"text","text":"All 9 ›","id":"all","weight":"bold","align":"end","size":"large"},
      {"kind":"box","fill":"ink","pad":"small","child":{"kind":"text","text":"White on ink, tone auto","size":"large","weight":"bold"}},
      {"kind":"box","fill":"ink","pad":"small","child":{"kind":"text","text":"Secondary on ink, tone given","tone":"secondary"}},
      {"kind":"box","fill":"light","pad":"small","child":{"kind":"text","text":"Ink on light"}}]})"));
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0), {});
    checkGolden(c, "tests/golden/plugin-primitives-text.png");
}

TEST_CASE("plugin primitives: golden on PW5 — rows: widths, a heading with a link, a stretched pair side") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawHeader(c.cr(), "Reading Desk", true);
    json j = json::parse(R"({"kind":"column","gap":"medium","children":[
      {"kind":"row","children":[{"kind":"text","text":"Pinned","caps":true,"style":"label","size":"small"},
                                {"kind":"text","text":"All 9 ›","id":"all","width":"auto","weight":"bold","size":"large"}],"align":"center"},
      {"kind":"row","children":[
        {"kind":"box","width":"30%","border":"thin","pad":"xsmall","child":{"kind":"text","text":"30%"}},
        {"kind":"box","border":"thin","pad":"xsmall","child":{"kind":"text","text":"fill"}},
        {"kind":"box","width":"auto","border":"thin","pad":"xsmall","child":{"kind":"text","text":"auto"}}]},
      {"kind":"row","children":[
        {"kind":"box","width":"100%","border":"thin","pad":"xsmall","child":{"kind":"text","text":"100%, scaled"}},
        {"kind":"box","width":"50%","border":"thin","pad":"xsmall","child":{"kind":"text","text":"50%"}}]},
      {"kind":"row","align":"stretch","gap":"medium","children":[
        {"kind":"box","width":"35%","fill":"ink","pad":"medium","child":{"kind":"text","text":"Pride and Prejudice\n\n\n\n\n\nJane Austen","weight":"bold","size":"large"}},
        {"kind":"column","gap":"xsmall","children":[
          {"kind":"text","text":"Pride and Prejudice","font":"serif","weight":"bold","size":"title","lines":3},
          {"kind":"text","text":"Jane Austen","font":"sans","tone":"secondary","size":"large"},
          {"kind":"spacer"},
          {"kind":"text","text":"13% · today","font":"sans","tone":"secondary","size":"large"}]}]},
      {"kind":"row","align":"bottom","children":[
        {"kind":"box","width":"20%","fill":"light","pad":"large","child":{"kind":"text","text":"tall\n\n\nbox"}},
        {"kind":"text","text":"at the bottom"}]}]})");
    const Block t = tree(j);
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0), {});
    checkGolden(c, "tests/golden/plugin-primitives-row.png");
}

TEST_CASE("plugin primitives: golden on PW5 — boxes: borders, fills, radii, a segmented switch") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawHeader(c.cr(), "Your desk", true);
    const Block t = tree(json::parse(R"({"kind":"column","gap":"medium","children":[
      {"kind":"box","border":"thin","radius":"small","child":{"kind":"row","gap":"none","children":[
        {"kind":"box","id":"reading","pad":"small","child":{"kind":"text","text":"Reading","align":"center","font":"sans","weight":"medium","size":"large"}},
        {"kind":"box","id":"pinned","pad":"small","fill":"ink","child":{"kind":"text","text":"Pinned","align":"center","font":"sans","weight":"medium","size":"large"}},
        {"kind":"box","id":"inbox","pad":"small","child":{"kind":"text","text":"Inbox 4","align":"center","font":"sans","weight":"medium","size":"large"}}]}},
      {"kind":"box","border":"thin","pad":"medium","child":{"kind":"text","text":"A thin border, medium padding"}},
      {"kind":"box","border":"thick","pad":"medium","radius":"small","child":{"kind":"text","text":"A thick border, small radius"}},
      {"kind":"box","fill":"light","pad":"medium","radius":"small","child":{"kind":"text","text":"A light fill"}},
      {"kind":"box","fill":"ink","pad":"medium","radius":"round","child":{"kind":"text","text":"An ink pill","align":"center"}},
      {"kind":"row","children":[
        {"kind":"box","width":"auto","border":"thin","radius":"round","pad":"small","child":{"kind":"text","text":"round","font":"sans"}},
        {"kind":"box","width":"auto","fill":"light","radius":"round","pad":"small","child":{"kind":"text","text":"light pill","font":"sans"}},
        {"kind":"box","border":"thin","pad":"small","child":{"kind":"box","fill":"ink","pad":"small","child":{"kind":"box","fill":"light","pad":"small",
          "child":{"kind":"text","text":"nested"}}}}]}]})"));
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0), {});
    checkGolden(c, "tests/golden/plugin-primitives-box.png");
}

// ---- primitives (0.12): covers, progress, item targets, grids, images -----------------------------

namespace {

Block columnOf(const json& children) { return tree({{"kind", "column"}, {"children", children}}); }

// An item piece over `key`: its cover above a progress bar (the Home pinned row's cell).
json coverCell(const std::string& key, double progress = 0.3) {
    return {{"kind", "item"},
            {"item", key},
            {"child", {{"kind", "column"},
                       {"gap", "xsmall"},
                       {"children", json::array({json{{"kind", "cover"}, {"item", key}},
                                                 json{{"kind", "progress"}, {"value", progress}}})}}}};
}

json gridOf(int n, const json& opts = json::object()) {
    json g = {{"kind", "grid"}, {"children", json::array()}};
    g.update(opts);
    for (int i = 0; i < n; ++i) g["children"].push_back(coverCell("S" + std::to_string(i)));
    return g;
}

// A plugin's package in a temp folder, with a 200×100 PNG in it: red on the left, blue on the right.
struct Package {
    std::filesystem::path dir;
    Package() {
        static int n = 0;
        dir = std::filesystem::temp_directory_path() / ("zk-package-" + std::to_string(::getpid()) + "-" + std::to_string(++n));
        std::filesystem::create_directories(dir / "img");
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 100);
        cairo_t* cr = cairo_create(s);
        cairo_set_source_rgb(cr, 1, 0, 0);
        cairo_rectangle(cr, 0, 0, 100, 100);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0, 0, 1);
        cairo_rectangle(cr, 100, 0, 100, 100);
        cairo_fill(cr);
        cairo_destroy(cr);
        cairo_surface_write_to_png(s, (dir / "img" / "logo.png").c_str());
        cairo_surface_destroy(s);
    }
    ~Package() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("plugin primitives: an auto grid at minWidth 18% of 1116 px has 5 equal columns; a row is as tall as its tallest cell") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    REQUIRE(blocksArea().w == 1116);
    FakeItems items = shelfItems(9);
    json g = gridOf(9, {{"minWidth", "18%"}});
    g["children"][2]["child"]["children"].push_back({{"kind", "text"}, {"text", "A cell with a line under it"}});
    const Block t = columnOf(json::array({g}));
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    const Laid& grid = l.children.at(0);
    REQUIRE(grid.children.size() == 9);
    for (int i = 0; i < 5; ++i) {
        CHECK(grid.children[i].rect.y == grid.rect.y);
        CHECK(grid.children[i].rect.w == grid.children[0].rect.w);
    }
    CHECK(grid.children[0].rect.x == grid.rect.x);
    const Rect last = grid.children[4].rect;
    CHECK(last.x + last.w <= grid.rect.x + grid.rect.w);
    CHECK(last.x + last.w > grid.rect.x + grid.rect.w - 5);
    CHECK(grid.children[5].rect.x == grid.rect.x);  // the sixth starts the next row
    const int tallest = grid.children[2].rect.h;
    CHECK(tallest > grid.children[0].rect.h);
    CHECK(grid.children[5].rect.y == grid.rect.y + tallest + 18);  // the row's height, then the gap (small)

    // Given columns win over minWidth.
    json three = gridOf(9, {{"columns", 3}, {"minWidth", "18%"}});
    const Block t3 = columnOf(json::array({three}));
    const Laid l3 = layoutTree(t3, blocksArea(), 0, &items);
    CHECK(l3.children.at(0).children.at(3).rect.x == l3.children.at(0).rect.x);
    CHECK(l3.children.at(0).children.at(2).rect.y == l3.children.at(0).rect.y);
}

TEST_CASE("plugin primitives: a grid with rows: 1 shows one row of 9 cells (5) and never pages") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(9);
    const Block t = columnOf(json::array({gridOf(9, {{"minWidth", "18%"}, {"rows", 1}})}));
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    CHECK(l.pages == 1);
    const Laid& grid = l.children.at(0);
    CHECK(grid.children.size() == 5);
    for (const auto& c : grid.children) CHECK(c.rect.y == grid.rect.y);
    CHECK(grid.rect.h == grid.children[0].rect.h);
    // Even in an area too short for the row: cut, not paged.
    const Rect area = blocksArea();
    const Laid short_ = layoutTree(t, {area.x, area.y, area.w, 150}, 0, &items);
    CHECK(short_.pages == 1);
}

TEST_CASE("plugin primitives: a grid without rows pages by row, like a list; nothing outside the area") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        FakeItems items = shelfItems(40);
        const Block t = columnOf(json::array({json{{"kind", "text"}, {"text", "Pinned"}}, gridOf(40, {{"minWidth", "22%"}})}));
        const Laid first = layoutTree(t, blocksArea(), 0, &items);
        CHECK(first.pages > 1);
        std::set<std::string> seen;
        for (int p = 0; p < first.pages; ++p) {
            const Laid l = layoutTree(t, blocksArea(), p, &items);
            const Laid& grid = l.children.at(1);
            for (const auto& cell : grid.children) {
                CHECK(cell.rect.x >= grid.rect.x);
                REQUIRE(cell.items.size() == 1);
                CHECK(seen.insert(cell.items[0].key).second);
            }
        }
        CHECK(seen.size() == 40);
        checkFits(t, &items);
    }
}

TEST_CASE("plugin primitives: a cover is 2:3 at its width — a percent of its parent, the row's slot, or the whole width") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(2);
    const Block t = columnOf(json::parse(R"([
      {"kind":"cover","item":"S0","width":"17%"},
      {"kind":"row","children":[{"kind":"cover","item":"S1","width":"35%"},{"kind":"text","text":"Pride and Prejudice"}]},
      {"kind":"cover","item":"S1"}])"));
    const Laid l = layoutTree(t, {0, 0, 1116, 1 << 20}, 0, &items);
    const Rect c17 = l.children.at(0).rect;
    CHECK(c17.w == 1116 * 17 / 100);
    CHECK(c17.h == c17.w * 3 / 2);
    const Rect c35 = l.children.at(1).children.at(0).rect;
    CHECK(c35.w == (1116 - 18) * 35 / 100);
    CHECK(c35.h == c35.w * 3 / 2);
    const Rect whole = l.children.at(2).rect;
    CHECK(whole.w == 1116);
    CHECK(whole.h == 1116 * 3 / 2);
    CHECK(l.children.at(0).items.at(0).key == "S0");
}

TEST_CASE("plugin primitives: a progress bar is 6 px tall at its width") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = columnOf(json::parse(R"([{"kind":"progress","value":0.4,"width":"50%"},{"kind":"progress","value":1}])"));
    const Laid l = layoutTree(t, {0, 0, 1000, 1000}, 0);
    CHECK(l.children.at(0).rect == Rect{0, 0, 500, 6});
    CHECK(l.children.at(1).rect.w == 1000);
    CHECK(l.children.at(1).rect.h == 6);
    Canvas c(1000, 100);
    fillRect(c.cr(), {0, 0, 1000, 100}, 255);
    drawTree(c.cr(), l, {});
    const auto g = grayOf(c);
    CHECK(g[3 * 1000 + 10] < 80);    // filled from the left
    CHECK(g[3 * 1000 + 400] > 150);  // the track past 40%
    CHECK(g[3 * 1000 + 700] == 255); // nothing past its width
}

TEST_CASE("plugin primitives: an item piece hit-tests to its row; one whose ref doesn't resolve is drawn but inert") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(2);
    const Block t = columnOf(json::parse(R"([
      {"kind":"item","item":"S1","child":{"kind":"row","children":[{"kind":"cover","item":"S1","width":"20%"},{"kind":"text","text":"Moby-Dick"}]}},
      {"kind":"item","item":"GONE","child":{"kind":"row","children":[{"kind":"cover","item":"GONE","width":"20%"},{"kind":"text","text":"Gone"}]}}])"));
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    const Laid& piece = l.children.at(0);
    const auto h = hitTest(l, piece.rect.center());
    REQUIRE(h);
    CHECK(h->kind == K::Item);
    CHECK(h->part == "S1");
    CHECK(h->rect == piece.rect);
    REQUIRE(h->item);
    CHECK(h->item->title == "Moby-Dick; or, The Whale");
    // The text beside the cover is the item too.
    const Rect text = piece.children.at(0).children.at(1).rect;
    REQUIRE(hitTest(l, {text.x + 10, text.y + 10}));
    CHECK(hitTest(l, {text.x + 10, text.y + 10})->kind == K::Item);

    const Laid& gone = l.children.at(1);
    CHECK(gone.rect.h > 0);  // drawn
    CHECK_FALSE(hitTest(l, gone.rect.center()));
    CHECK(gone.items.empty());
    // Its cover: a blank light-grey rectangle.
    Canvas c(kPw5.width, kPw5.height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawTree(c.cr(), l, {}, &items);
    const auto g = grayOf(c);
    const Rect cover = gone.children.at(0).children.at(0).rect;
    CHECK(cover.h == cover.w * 3 / 2);
    const uint8_t mid = g[(size_t)cover.center().y * c.width() + cover.center().x];
    CHECK(mid > 150);
    CHECK(mid < 245);
}

TEST_CASE("plugin primitives: a downloading item shows its status in a bar over the bottom of the piece") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(2);
    const Block t = columnOf(json::array({gridOf(2, {{"columns", 4}})}));
    const auto shot = [&](Rect r) {
        Canvas c(kPw5.width, kPw5.height);
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        drawTree(c.cr(), layoutTree(t, blocksArea(), 0, &items), {}, &items);
        const auto g = grayOf(c);
        std::vector<uint8_t> px;
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) px.push_back(g[static_cast<size_t>(y) * c.width() + x]);
        return px;
    };
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    const Rect piece = l.children.at(0).children.at(0).rect;
    const Rect bar{piece.x, piece.y + piece.h - 40, piece.w, 40}, above{piece.x, piece.y, piece.w, piece.h - 40};
    const Rect other = l.children.at(0).children.at(1).rect;
    const auto plainBar = shot(bar), plainAbove = shot(above), plainOther = shot(other);
    items.download["AS0"] = "Downloading 42%";
    CHECK(shot(bar) != plainBar);
    CHECK(shot(above) == plainAbove);
    CHECK(shot(other) == plainOther);
    // The bar is white with ink text.
    const auto b = shot(bar);
    size_t white = 0, ink = 0;
    for (uint8_t v : b) white += v == 255, ink += v < 60;
    CHECK(white > b.size() / 2);
    CHECK(ink > 0);
}

TEST_CASE("plugin primitives: an image from the plugin's package is drawn in greys at its width, aspect kept; a missing one takes no room") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Package pkg;
    const Block t = columnOf(json::parse(R"([
      {"kind":"image","src":"img/logo.png","width":"50%","id":"logo"},
      {"kind":"image","src":"img/missing.png"},
      {"kind":"text","text":"Under"}])"));
    const Laid l = layoutTree(t, {0, 0, 1000, 2000}, 0, nullptr, pkg.dir.string());
    const Rect img = l.children.at(0).rect;
    CHECK(img == Rect{0, 0, 500, 250});
    CHECK(l.children.at(1).rect.h == 0);
    CHECK(l.children.at(2).rect.y == 250 + 18 + 18);  // the missing image's gaps only
    Canvas c(1000, 400);
    fillRect(c.cr(), {0, 0, 1000, 400}, 255);
    drawTree(c.cr(), l, {});
    // Red and blue in greys: two different greys, neither white.
    const auto g = grayOf(c);
    const uint8_t left = g[125 * 1000 + 100], right = g[125 * 1000 + 400];
    CHECK(left < 200);
    CHECK(right < 200);
    CHECK(left != right);
    std::vector<uint32_t> rgb(1000 * 400);
    cairo_surface_flush(c.surface());
    const auto* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    const uint32_t px = *reinterpret_cast<const uint32_t*>(data + 125 * stride + 100 * 4);
    CHECK(((px >> 16) & 0xff) == ((px >> 8) & 0xff));  // grey: red == green == blue
    CHECK(((px >> 8) & 0xff) == (px & 0xff));
    // With an id it is tapped as itself.
    const auto h = hitTest(l, img.center());
    REQUIRE(h);
    CHECK(h->id == "logo");
    CHECK(h->kind == K::Image);
    // No package (or one without the file): no room.
    const Laid none = layoutTree(t, {0, 0, 1000, 2000}, 0);
    CHECK(none.children.at(0).rect.h == 0);
}

TEST_CASE("plugin primitives: golden on PW5 — covers, progress, item pieces (a download, an unresolved ref)") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    FakeItems items = shelfItems(8);
    items.download["AS3"] = "Downloading 42%";
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawHeader(c.cr(), "Reading Desk", false);
    const Block t = tree(json::parse(R"({"kind":"column","gap":"large","children":[
      {"kind":"row","gap":"medium","align":"stretch","children":[
        {"kind":"item","item":"S0","width":"50%","child":{"kind":"row","gap":"medium","align":"stretch","children":[
          {"kind":"cover","item":"S0","width":"35%"},
          {"kind":"column","gap":"xsmall","children":[
            {"kind":"text","text":"Book","caps":true,"font":"sans","weight":"bold","size":"small","tone":"secondary"},
            {"kind":"text","text":"Pride and Prejudice","font":"serif","weight":"bold","size":"large","lines":3},
            {"kind":"text","text":"Jane Austen","font":"sans","tone":"secondary"},
            {"kind":"spacer"},
            {"kind":"progress","value":0.13},
            {"kind":"text","text":"13% · today","font":"sans","tone":"secondary","size":"small"}]}]}},
        {"kind":"item","item":"S3","child":{"kind":"row","gap":"medium","align":"stretch","children":[
          {"kind":"cover","item":"S3","width":"35%"},
          {"kind":"column","gap":"xsmall","children":[
            {"kind":"text","text":"Paper","caps":true,"font":"sans","weight":"bold","size":"small","tone":"secondary"},
            {"kind":"text","text":"Gaussian random fields","font":"serif","weight":"bold","size":"large","lines":3},
            {"kind":"text","text":"Adler","font":"sans","tone":"secondary"},
            {"kind":"spacer"},
            {"kind":"progress","value":0.67},
            {"kind":"text","text":"67% · yesterday","font":"sans","tone":"secondary","size":"small"}]}]}}]},
      {"kind":"row","gap":"medium","children":[
        {"kind":"item","item":"S1","width":"17%","child":{"kind":"column","gap":"xsmall","children":[
          {"kind":"cover","item":"S1"},{"kind":"progress","value":0.62}]}},
        {"kind":"item","item":"GONE","width":"17%","child":{"kind":"column","gap":"xsmall","children":[
          {"kind":"cover","item":"GONE"},{"kind":"progress","value":0}]}},
        {"kind":"cover","item":"S4","width":"17%"},
        {"kind":"spacer"}]},
      {"kind":"item","item":"S2","child":{"kind":"column","gap":"none","children":[
        {"kind":"text","text":"The Brothers Karamazov","font":"serif","weight":"bold","lines":1},
        {"kind":"text","text":"Dostoevsky · 1880","font":"sans","tone":"secondary","size":"small"}]}}]})"));
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0, &items), {}, &items);
    checkGolden(c, "tests/golden/plugin-primitives-items.png");
}

TEST_CASE("plugin primitives: golden on PW5 — a desk grid (auto columns at 22%) and a one-row shelf (rows 1 at 17%)") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    FakeItems items = shelfItems(12);
    items.download["AS6"] = "Downloading 42%";
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawHeader(c.cr(), "Your desk", true);
    json shelf = gridOf(9, {{"minWidth", "17%"}, {"rows", 1}, {"gap", "medium"}});
    json desk = {{"kind", "grid"}, {"minWidth", "22%"}, {"gap", "medium"}, {"children", json::array()}};
    for (int i = 0; i < 8; ++i) {
        const std::string k = "S" + std::to_string(i);
        desk["children"].push_back({{"kind", "item"}, {"item", k}, {"child", {{"kind", "column"}, {"gap", "xsmall"}, {"children", json::array({
            json{{"kind", "cover"}, {"item", k}}, json{{"kind", "progress"}, {"value", 0.1 * (i + 1)}},
            json{{"kind", "text"}, {"text", items.rows[k].title}, {"font", "serif"}, {"weight", "bold"}, {"lines", 1}},
            json{{"kind", "text"}, {"text", "Author · " + std::to_string(10 * (i + 1)) + "%"}, {"font", "sans"}, {"tone", "secondary"}, {"size", "small"}}})}}}});
    }
    const Block t = columnOf(json::array({json{{"kind", "text"}, {"text", "Pinned"}, {"caps", true}, {"style", "label"}}, shelf,
                                          json{{"kind", "text"}, {"text", "Desk"}, {"caps", true}, {"style", "label"}}, desk}));
    drawTree(c.cr(), layoutTree(t, blocksArea(), 0, &items), {}, &items);
    checkGolden(c, "tests/golden/plugin-primitives-grid.png");
}

TEST_CASE("plugin primitives: a cover in a stretched row stays 2:3, at the top of its slot") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(1);
    const Block t = columnOf(json::parse(R"([{"kind":"row","align":"stretch","children":[
      {"kind":"cover","item":"S0","width":"20%"},
      {"kind":"text","text":"A\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\nZ"}]}])"));
    const Laid l = layoutTree(t, {0, 0, 1000, 1 << 20}, 0, &items);
    const Laid& row = l.children.at(0);
    const Rect cover = row.children.at(0).rect;
    REQUIRE(row.rect.h > cover.w * 3 / 2);  // the row is taller than the cover
    CHECK(cover.y == row.rect.y);
    CHECK(cover.h == cover.w * 3 / 2);
    // Nothing drawn under it.
    Canvas c(1000, row.rect.h + 40);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
    drawTree(c.cr(), l, {}, &items);
    const auto g = grayOf(c);
    CHECK(g[(size_t)(cover.y + cover.h + 20) * c.width() + cover.x + cover.w / 2] == 255);
}

TEST_CASE("plugin primitives: an image over 4 megapixels or 4096 px a side is refused before decoding (no room)") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Package pkg;
    // A PNG signature and an IHDR claiming the size, nothing more: refused on the header alone.
    const auto header = [&](const char* name, uint32_t w, uint32_t h) {
        std::string s("\x89PNG\r\n\x1a\n", 8);
        s += std::string("\0\0\0\x0d", 4) + "IHDR";
        for (uint32_t v : {w, h})
            for (int sh = 24; sh >= 0; sh -= 8) s += static_cast<char>((v >> sh) & 0xff);
        s += std::string("\x08\x02\0\0\0", 5) + std::string(4, '\0');
        std::ofstream(pkg.dir / name, std::ios::binary) << s;
    };
    header("huge.png", 30000, 20000);  // 600 MP: refused before any decoding
    header("wide.png", 5000, 10);      // a side over 4096
    // Real PNGs a few KB small (one colour compresses well) that would decode to 24 MB and over 4096 px.
    const auto real = [&](const char* name, int w, int h) {
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        REQUIRE(cairo_surface_write_to_png(s, (pkg.dir / name).c_str()) == CAIRO_STATUS_SUCCESS);
        cairo_surface_destroy(s);
        CHECK(std::filesystem::file_size(pkg.dir / name) < (2u << 20));
    };
    real("big.png", 3000, 2000);  // 6 MP
    real("long.png", 4100, 100);  // 0.4 MP, but a side over 4096
    real("edge.png", 4096, 1000);  // just inside: loads
    const Block t = columnOf(json::parse(R"([{"kind":"image","src":"huge.png"},{"kind":"image","src":"wide.png"},
      {"kind":"image","src":"big.png"},{"kind":"image","src":"long.png"},{"kind":"image","src":"edge.png"},
      {"kind":"image","src":"img/logo.png"}])"));
    const Laid l = layoutTree(t, {0, 0, 1000, 4000}, 0, nullptr, pkg.dir.string());
    for (int i = 0; i < 4; ++i) {
        INFO(i);
        CHECK(l.children.at(i).rect.h == 0);
    }
    CHECK(l.children.at(4).rect.h == 1000 * 1000 / 4096);
    CHECK(l.children.at(5).rect.h == 500);  // a small one still loads
}

// ---- a trimming Home body (0.12): fit "trim" on its root never pages -------------------------------

namespace {

Block homeTree(const json& j) {
    std::string why;
    auto t = parseTree(j, &why, TreeUse::HomeBody);
    INFO(why);
    REQUIRE(t);
    return *t;
}

// A Home body like Reading Desk 3's: the pair (about 330 px), the pinned row (a grid of 1 row), the
// inbox's lines, the passage.
json deskHome(const std::string& fit = "trim", const json& grid = gridOf(8, {{"minWidth", "17%"}, {"rows", 1}})) {
    json pair = json::parse(R"({"kind":"row","children":[
      {"kind":"cover","item":"S0","width":"20%"},{"kind":"text","text":"Pride and Prejudice","style":"title"},
      {"kind":"cover","item":"S3","width":"20%"},{"kind":"text","text":"Gaussian random fields","style":"title"}]})");
    json pinned = {{"kind", "column"}, {"children", json::array({json{{"kind", "text"}, {"text", "Pinned"}, {"style", "label"}}, grid})}};
    json inbox = json::parse(R"({"kind":"column","children":[{"kind":"text","text":"Inbox","style":"label"},
      {"kind":"text","id":"in1","text":"Brownian loops and the central charge"},
      {"kind":"text","id":"in2","text":"Random walks on percolation clusters"},
      {"kind":"text","id":"in3","text":"Schramm–Loewner evolution, a survey"}]})");
    json quote = json::parse(R"({"kind":"quote","id":"passage","text":"It is a truth universally acknowledged.","source":"Austen · p. 1"})");
    return {{"kind", "column"}, {"fit", fit}, {"children", json::array({pair, pinned, inbox, quote})}};
}

void checkInside(const Laid& l, Rect area) {
    std::vector<Rect> rects;
    collectRects(l, rects);
    for (Rect r : rects) {
        INFO(r.x << "," << r.y << " " << r.w << "x" << r.h);
        CHECK(inside(r, area));
    }
}

}  // namespace

TEST_CASE("plugin trim: a short area keeps the pair, the pinned row and one inbox line; the rest is dropped, never paged") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(8);
    const Block t = homeTree(deskHome());
    const Rect tall{blocksArea().x, blocksArea().y, blocksArea().w, 5000};
    const Laid full = layoutTree(t, tall, 0, &items);
    REQUIRE(full.children.size() == 4);
    const Laid& inbox = full.children[2];
    REQUIRE(inbox.children.size() == 4);
    const Rect in2 = inbox.children[2].rect;
    CHECK(full.children[0].rect.h > 250);  // the pair, about 330 px

    // The area ends halfway down the second inbox line.
    const Rect area{tall.x, tall.y, tall.w, in2.y + in2.h / 2 - tall.y};
    const Laid l = layoutTree(t, area, 0, &items);
    CHECK(l.pages == 1);
    CHECK(pageIndicatorRect(l).empty());
    REQUIRE(l.children.size() == 3);
    CHECK(l.children[0].rect == full.children[0].rect);
    CHECK(l.children[1].rect == full.children[1].rect);
    CHECK(l.children[1].children.size() == full.children[1].children.size());
    const Laid& kept = l.children[2];
    REQUIRE(kept.children.size() == 2);  // the label and the first line
    CHECK(kept.children[1].block->id == "in1");
    CHECK(kept.rect.h == inbox.children[1].rect.y + inbox.children[1].rect.h - inbox.rect.y);
    CHECK(findLaid(l, "in2") == nullptr);
    CHECK(findLaid(l, "passage") == nullptr);
    checkInside(l, area);
    // Hit-testing sees only what's shown: the top of the dropped line, inside the area, is nothing.
    CHECK_FALSE(hitTest(l, {in2.x + 4, in2.y + 2}));
    CHECK(hitTest(l, inbox.children[1].rect.center()));
    // Drawn: nothing outside the area, no page indicator.
    Canvas c(metrics().width, metrics().height);
    fillRect(c.cr(), {0, 0, c.width(), c.height()}, 128);
    drawTree(c.cr(), l, {}, &items);
    const auto g = grayOf(c);
    size_t outside = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (!area.contains({x, y}) && g[(size_t)y * c.width() + x] != 128) ++outside;
    CHECK(outside == 0);

    // No orphaned heading: an area that holds the inbox's label but not its first line drops the inbox column.
    const Rect in1 = inbox.children[1].rect;
    const Laid orphan = layoutTree(t, {tall.x, tall.y, tall.w, in1.y + in1.h / 2 - tall.y}, 0, &items);
    CHECK(orphan.children.size() == 2);
    CHECK(findLaid(orphan, "in1") == nullptr);
    // Nor in the pinned section: its label without the grid's row is dropped with it.
    const Laid& pinned = full.children[1];
    const Laid noRow = layoutTree(t, {tall.x, tall.y, tall.w, pinned.children[1].rect.y + 20 - tall.y}, 0, &items);
    CHECK(noRow.children.size() == 1);

    // fit "page" (the default) is unchanged: the same blocks page there.
    const Block paged = homeTree(deskHome("page"));
    CHECK(layoutTree(paged, area, 0, &items).pages > 1);
    const Laid pagedFull = layoutTree(paged, tall, 0, &items);
    REQUIRE(pagedFull.children.size() == full.children.size());
    for (size_t i = 0; i < full.children.size(); ++i) CHECK(pagedFull.children[i].rect == full.children[i].rect);
}

TEST_CASE("plugin trim: a grid that doesn't fit is trimmed to its rows that do; a block that can't be trimmed is dropped") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    FakeItems items = shelfItems(24);
    for (const json& grid : {gridOf(24, {{"minWidth", "22%"}}), gridOf(24, {{"minWidth", "22%"}, {"rows", 3}})}) {
        INFO(grid.dump().substr(0, 60));
        const Block t = homeTree({{"kind", "column"}, {"fit", "trim"},
                                  {"children", json::array({json{{"kind", "text"}, {"text", "Pinned"}, {"style", "label"}}, grid,
                                                            json{{"kind", "text"}, {"text", "after"}}})}});
        const Rect tall{blocksArea().x, blocksArea().y, blocksArea().w, 5000};
        const Laid full = layoutTree(t, tall, 0, &items);
        const Laid& g = full.children.at(1);
        const int perRow = static_cast<int>(std::count_if(g.children.begin(), g.children.end(),
                                                          [&](const Laid& c) { return c.rect.y == g.children[0].rect.y; }));
        REQUIRE(static_cast<int>(g.children.size()) > 2 * perRow);
        const Rect second = g.children[perRow].rect;  // the second row's first cell
        const Rect area{tall.x, tall.y, tall.w, second.y + second.h / 2 - tall.y};
        const Laid l = layoutTree(t, area, 0, &items);
        CHECK(l.pages == 1);
        REQUIRE(l.children.size() == 2);  // "after" dropped
        const Laid& trimmed = l.children[1];
        CHECK(static_cast<int>(trimmed.children.size()) == perRow);  // one whole row, no cut cells
        for (int i = 0; i < perRow; ++i) CHECK(trimmed.children[i].rect == g.children[i].rect);
        CHECK(trimmed.rect.h == g.children[0].rect.h);
        checkInside(l, area);
        // Not even one row fits: the grid is dropped (and what follows).
        const Laid none = layoutTree(t, {tall.x, tall.y, tall.w, g.rect.y - tall.y + 20}, 0, &items);
        CHECK(none.children.size() == 1);
    }
    // A block that can't be trimmed (a quote, a row) is dropped whole.
    const Block q = homeTree(json::parse(R"({"kind":"column","fit":"trim","children":[{"kind":"text","text":"Desk"},
      {"kind":"quote","id":"passage","text":"It is a truth universally acknowledged.","source":"Austen · p. 1"}]})"));
    const Laid full = layoutTree(q, {0, 0, 1000, 5000}, 0);
    const Laid l = layoutTree(q, {0, 0, 1000, full.children[1].rect.y + 30}, 0);
    CHECK(l.children.size() == 1);
    CHECK_FALSE(hitTest(l, {full.children[1].rect.x + 10, full.children[1].rect.y + 10}));
}

TEST_CASE("plugin trim: on every Kindle, portrait and landscape, the pair and what fits under it; never a page") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        for (bool landscape : {false, true}) {
            Metrics mm = *dev;
            if (landscape) std::swap(mm.width, mm.height);
            ScopedMetrics m(mm);
            INFO(dev->device << (landscape ? " landscape" : ""));
            FakeItems items = shelfItems(8);
            const Block t = homeTree(deskHome());
            const Rect area = blocksArea();
            const Laid l = layoutTree(t, area, 0, &items);
            CHECK(l.pages == 1);
            REQUIRE_FALSE(l.children.empty());
            const Laid alone = layoutTree(t, {area.x, area.y, area.w, 5000}, 0, &items);
            CHECK(l.children[0].rect == alone.children[0].rect);  // the pair, whole
            for (size_t i = 0; i < l.children.size(); ++i) CHECK(l.children[i].block == &t.children[i]);  // in order
            checkInside(l, area);
        }
    }
    // PW5 landscape: the pair and the pinned row fit under it.
    Metrics land = kPw5;
    std::swap(land.width, land.height);
    ScopedMetrics m(land);
    FakeItems items = shelfItems(8);
    const Block t = homeTree(deskHome());
    const Laid l = layoutTree(t, blocksArea(), 0, &items);
    REQUIRE(l.children.size() >= 2);
    CHECK(l.children[1].children.size() == 2);
}
