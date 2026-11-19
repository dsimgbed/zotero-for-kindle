// Plugin screen blocks: layout with paging, drawing in the app's style, hit-testing
// (src/screens/plugin_blocks.h). Goldens on PW5; KT4 and Scribe get fit assertions.

#include <doctest/doctest.h>

#include <filesystem>
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
