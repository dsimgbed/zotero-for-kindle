// Plugin screen trees: parsing, validation and caps (src/plugins/ui_tree.h).

#include <doctest/doctest.h>

#include "plugins/ui_tree.h"

using namespace zk;
using json = nlohmann::json;
using K = Block::Kind;

namespace {

std::string whyNot(const json& j) {
    std::string why;
    auto t = parseTree(j, &why);
    CHECK_FALSE(t);
    return why;
}

Block ok(const json& j) {
    std::string why;
    auto t = parseTree(j, &why);
    INFO(why);
    REQUIRE(t);
    return *t;
}

}  // namespace

TEST_CASE("ui tree: every block kind parses as the zk.ui helpers write it") {
    const json j = json::parse(R"({"kind":"column","gap":"medium","children":[
      {"kind":"row","children":[{"kind":"text","text":"math.PR · 12 left","style":"label"},{"kind":"button","id":"feeds","title":"Feeds"}]},
      {"kind":"text","text":["Filed into ",{"text":"Probability / SPDEs","bold":true},{"text":" now","italic":true}],"style":"meta","lines":2},
      {"kind":"deck","id":"card","card":{"kind":"card","title":"Gaussian fields","meta":"Adler · 2024","body":"One.\n\nTwo.","badge":"In library"},"empty":"Nothing left"},
      {"kind":"card","title":"Alone"},
      {"kind":"list","id":"cols","rows":[{"id":"a","title":"A","subtitle":"s","meta":"3","badge":"already in","selected":true},{"id":"b","title":"B"}],"empty":"None"},
      {"kind":"search","id":"find","placeholder":"Collection","focus":true},
      {"kind":"field","id":"note","label":"Note","lines":3,"value":"x"},
      {"kind":"chips","id":"tags","chips":["to-read",{"id":"k1","title":"Probability"}],"selected":["to-read"],"select":"one"},
      {"kind":"buttons","buttons":[{"id":"discard","title":"Discard"},null,{"id":"keep","title":"Keep","primary":true}]},
      {"kind":"divider"},{"kind":"space","size":"large"},{"kind":"space"},
      {"kind":"notice","text":"Discarded."},
      null, false, "", 0
    ]})");
    Block t = ok(j);
    CHECK(t.kind == K::Column);
    CHECK(t.gap == Block::Gap::Medium);
    REQUIRE(t.children.size() == 13);
    CHECK(t.children[0].kind == K::Row);
    CHECK(t.children[0].children[1].id == "feeds");
    const Block& text = t.children[1];
    REQUIRE(text.spans.size() == 3);
    CHECK(text.spans[0].text == "Filed into ");
    CHECK(text.spans[1].bold);
    CHECK(text.spans[2].italic);
    CHECK(text.style == Block::Style::Meta);
    CHECK(text.lines == 2);
    CHECK(plainText(text) == "Filed into Probability / SPDEs now");
    const Block& deck = t.children[2];
    REQUIRE(deck.children.size() == 1);
    CHECK(deck.children[0].body == "One.\n\nTwo.");
    CHECK(deck.children[0].badge == "In library");
    CHECK(deck.empty == "Nothing left");
    CHECK(t.children[3].kind == K::Card);
    CHECK(t.children[4].rows.size() == 2);
    CHECK(t.children[4].rows[0].selected);
    CHECK(t.children[5].focus);
    CHECK_FALSE(t.children[5].value);
    CHECK(t.children[6].lines == 3);
    CHECK(t.children[6].value == std::optional<std::string>("x"));
    CHECK(t.children[7].chips[0].title == "to-read");
    CHECK(t.children[7].chips[1].title == "Probability");
    CHECK(t.children[7].select == Block::Select::One);
    CHECK(t.children[8].children.size() == 2);
    CHECK(t.children[8].children[1].primary);
    CHECK(t.children[10].gap == Block::Gap::Large);
    CHECK(t.children[11].gap == Block::Gap::Medium);
    CHECK(t.children[12].text == "Discarded.");
    // column, row, text, button, text, deck, card, card, list + 2 rows, search, field, chips + 2, buttons + 2,
    // divider, 2 spaces, notice
    CHECK(blockCount(t) == 23);
}

TEST_CASE("ui tree: a deck with no card, a lone block as the root, null optional fields") {
    Block d = ok(json::parse(R"({"kind":"deck","id":"d","card":null,"empty":"Nothing left to riffle"})"));
    CHECK(d.children.empty());
    ok(json::parse(R"({"kind":"deck","id":"d"})"));
    Block t = ok(json::parse(R"({"kind":"text","text":"Gone","style":null})"));
    CHECK(t.style == Block::Style::Body);
}

TEST_CASE("ui tree: unknown kinds and fields and wrong types are refused, naming the path") {
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"text","text":"a"},{"kind":"toggle","id":"t"}]})")) ==
          "column[1]: unknown block kind \"toggle\"");
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"row","children":[{"kind":"text","text":"a","colour":"red"}]}]})")) ==
          "column[0].row[0].text: unknown field \"colour\"");
    CHECK(whyNot(json::parse(R"({"kind":"numbered","id":"n"})")) == "the screen: unknown block kind \"numbered\"");
    CHECK(whyNot(json::parse(R"("hello")")) == "the screen: a block must be an object");
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"a","style":"huge"})")) ==
          "text: \"style\" must be one of \"title\", \"body\", \"meta\", \"label\"");
    CHECK(whyNot(json::parse(R"({"kind":"list","id":"l","rows":[{"id":"a","title":"A"},{"title":"B"}]})")) ==
          "list.rows[1]: \"id\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"button","title":"x"})")) == "button: \"id\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"a","lines":0})")) ==
          "text: \"lines\" must be a whole number, 1 or more");
    CHECK(whyNot(json::parse(R"({"kind":"column"})")) == "column: \"children\" is missing");
}

TEST_CASE("ui tree: at most 3 buttons in a row") {
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"text","text":"a"},{"kind":"space"},
        {"kind":"buttons","buttons":[{"id":"a","title":"A"},{"id":"b","title":"B"},{"id":"c","title":"C"},{"id":"d","title":"D"}]}]})")) ==
          "column[2].buttons: at most 3");
    // A skipped falsy entry doesn't count.
    ok(json::parse(R"({"kind":"buttons","buttons":[{"id":"a","title":"A"},false,{"id":"b","title":"B"},{"id":"c","title":"C"}]})"));
}

TEST_CASE("ui tree: interactive ids are unique in a screen; rows and chips within their block") {
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"button","id":"go","title":"Go"},{"kind":"search","id":"go"}]})")) ==
          "column[1].search: another block already has the id \"go\"");
    CHECK(whyNot(json::parse(R"({"kind":"list","id":"l","rows":[{"id":"a","title":"A"},{"id":"a","title":"B"}]})")) ==
          "list.rows[1]: another row already has the id \"a\"");
    CHECK(whyNot(json::parse(R"({"kind":"chips","id":"c","chips":["x","x"]})")) ==
          "chips.chips[1]: another chip already has the id \"x\"");
    // Row ids may repeat other blocks' ids (events name both).
    ok(json::parse(R"({"kind":"column","children":[{"kind":"button","id":"a","title":"A"},{"kind":"list","id":"l","rows":[{"id":"a","title":"A"}]}]})"));
}

TEST_CASE("ui tree: caps of 2,000 blocks and 256 KB of text") {
    json rows = json::array();
    for (int i = 0; i < 1999; ++i) rows.push_back({{"id", std::to_string(i)}, {"title", "r"}});
    ok(json{{"kind", "list"}, {"id", "l"}, {"rows", rows}});  // 1 + 1999
    rows.push_back({{"id", "x"}, {"title", "r"}});
    CHECK(whyNot(json{{"kind", "list"}, {"id", "l"}, {"rows", rows}}) == "list.rows[1999]: the tree has more than 2,000 blocks");

    json chips = json::array();
    for (int i = 0; i < 2000; ++i) chips.push_back("c" + std::to_string(i));
    CHECK(whyNot(json{{"kind", "chips"}, {"id", "c"}, {"chips", chips}}).find("more than 2,000 blocks") != std::string::npos);

    const std::string big(200 * 1024, 'a');
    ok(json{{"kind", "card"}, {"title", "t"}, {"body", big}});
    CHECK(whyNot(json{{"kind", "column"},
                      {"children", {{{"kind", "card"}, {"title", "t"}, {"body", big}}, {{"kind", "text"}, {"text", big}}}}}) ==
          "column[1].text: the tree has more than 256 KB of text");
}

TEST_CASE("ui tree: titles and labels on one line; text and bodies keep their line breaks") {
    Block t = ok(json::parse(R"({"kind":"column","children":[
      {"kind":"button","id":"b","title":"  File\ninto\t Probability​ "},
      {"kind":"card","title":"A\nB","meta":"x\r\ny","body":"Para one\nline two\n\nPara\u0007 two"},
      {"kind":"text","text":["Bold ",{"text":"word","bold":true}," end\nnext"]},
      {"kind":"list","id":"l","rows":[{"id":"r","title":"T\n2","subtitle":"s\u0000s"}]},
      {"kind":"notice","text":"a\n\nb"}
    ]})"));
    CHECK(t.children[0].title == "File into Probability");
    CHECK(t.children[1].title == "A B");
    CHECK(t.children[1].meta == "x y");
    CHECK(t.children[1].body == "Para one\nline two\n\nPara two");
    CHECK(plainText(t.children[2]) == "Bold word end\nnext");
    CHECK(t.children[3].rows[0].title == "T 2");
    CHECK(t.children[3].rows[0].subtitle == "s s");
    CHECK(t.children[4].text == "a b");
}

TEST_CASE("ui tree: sameBlock is deep equality") {
    const json j = json::parse(R"({"kind":"column","children":[{"kind":"list","id":"l","rows":[{"id":"a","title":"A"}]}]})");
    Block a = ok(j), b = ok(j);
    CHECK(sameBlock(a, b));
    b.children[0].rows[0].selected = true;
    CHECK_FALSE(sameBlock(a, b));
}

TEST_CASE("ui tree: blocks nest at most 16 deep; a 2,000-deep chain is refused at once") {
    const auto chain = [](int depth) {
        json j = {{"kind", "text"}, {"text", "leaf"}};
        for (int i = 1; i < depth; ++i) j = {{"kind", "column"}, {"children", json::array({j})}};
        return j;
    };
    Block t = ok(chain(16));
    CHECK(blockCount(t) == 16);
    const std::string why = whyNot(chain(17));
    CHECK(why.find("blocks nested more than 16 deep") != std::string::npos);
    CHECK(why.rfind("column[0].", 0) == 0);  // named by its path
    CHECK(whyNot(chain(2000)).find("nested more than 16 deep") != std::string::npos);
}

TEST_CASE("ui tree: a row holds at most 12 blocks") {
    json kids = json::array();
    for (int i = 0; i < 12; ++i) kids.push_back({{"kind", "text"}, {"text", "t"}});
    ok(json{{"kind", "row"}, {"children", kids}});
    kids.push_back({{"kind", "text"}, {"text", "t"}});
    CHECK(whyNot(json{{"kind", "row"}, {"children", kids}}) == "row: a row holds at most 12 blocks");
}

TEST_CASE("ui tree: an items block takes keys, refs with a library and a label; each ref counts one block") {
    const Block t = ok(json::parse(R"({"kind":"column","children":[
      {"kind":"items","id":"now","style":"card","items":[{"key":"ABCD2345","label":"BOOK · CONTINUE"},"EFGH6789"]},
      {"kind":"items","id":"pick","style":"row","items":[{"key":"QRST2345","library":12}],"empty":"Nothing yet"}]})"));
    REQUIRE(t.children.size() == 2);
    const Block& card = t.children[0];
    CHECK(card.kind == K::Items);
    CHECK(card.id == "now");
    CHECK(card.itemStyle == Block::ItemStyle::Card);
    REQUIRE(card.items.size() == 2);
    CHECK(card.items[0].key == "ABCD2345");
    CHECK(card.items[0].label == "BOOK · CONTINUE");
    CHECK_FALSE(card.items[0].library);
    CHECK(card.items[1].key == "EFGH6789");
    CHECK(card.items[1].label.empty());
    const Block& row = t.children[1];
    CHECK(row.itemStyle == Block::ItemStyle::Row);
    CHECK(row.items[0].library == 12);
    CHECK(row.empty == "Nothing yet");
    CHECK(blockCount(t) == 1 + 1 + 2 + 1 + 1);

    json refs = json::array();
    for (int i = 0; i < 2000; ++i) refs.push_back("K" + std::to_string(i));
    CHECK(whyNot(json{{"kind", "items"}, {"id", "x"}, {"style", "row"}, {"items", refs}}).find("more than 2,000 blocks") != std::string::npos);
}

TEST_CASE("ui tree: an items block in the shelf style (0.11); a ref's label is kept, the shelf just doesn't draw it") {
    const Block t = ok(json::parse(R"({"kind":"items","id":"shelf","style":"shelf","items":["ABCD2345",{"key":"EFGH6789","label":"x"}],"empty":"No books"})"));
    CHECK(t.kind == K::Items);
    CHECK(t.itemStyle == Block::ItemStyle::Shelf);
    REQUIRE(t.items.size() == 2);
    CHECK(t.items[1].label == "x");
    CHECK(t.empty == "No books");
    // allowed in Home's body
    std::string why;
    CHECK(parseTree(json::parse(R"({"kind":"items","id":"shelf","style":"shelf","items":["A"]})"), &why, TreeUse::HomeBody));
    CHECK(why.empty());
}

TEST_CASE("ui tree: an items block with a bad style, ref or field is refused, naming where") {
    CHECK(whyNot(json::parse(R"({"kind":"items","id":"x","style":"grid","items":[]})")) ==
          "items: \"style\" must be one of \"card\", \"row\", \"shelf\"");
    CHECK(whyNot(json::parse(R"({"kind":"items","id":"x","items":[]})")) == "items: \"style\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"items","id":"x","style":"row"})")) == "items: \"items\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"items","id":"x","style":"row","items":[3]})")) ==
          "items.items[0]: an item must be a key or { key, library?, label? }");
    CHECK(whyNot(json::parse(R"({"kind":"items","id":"x","style":"row","items":[{"library":1}]})")) == "items.items[0]: \"key\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"items","id":"x","style":"row","items":[{"key":"A","library":-1}]})")).find("\"library\"") != std::string::npos);
    CHECK(whyNot(json::parse(R"({"kind":"items","id":"x","style":"row","items":[{"key":"A","color":"red"}]})")) ==
          "items.items[0]: unknown field \"color\"");
    CHECK(whyNot(json::parse(R"({"kind":"items","style":"row","items":[]})")) == "items: \"id\" is missing");
}

TEST_CASE("ui tree: Home's body refuses a search and a field, other blocks pass") {
    const json items = json::parse(R"({"kind":"items","id":"i","style":"row","items":["A"]})");
    std::string why;
    CHECK(parseTree(json{{"kind", "column"}, {"children", json::array({items})}}, &why, TreeUse::HomeBody));
    const json search = json::parse(R"({"kind":"search","id":"s"})"), field = json::parse(R"({"kind":"field","id":"f"})");
    why.clear();
    CHECK_FALSE(parseTree(json{{"kind", "column"}, {"children", json::array({items, search})}}, &why, TreeUse::HomeBody));
    CHECK(why == "column[1]: search can't be on Home");
    why.clear();
    CHECK_FALSE(parseTree(json{{"kind", "column"}, {"children", json::array({json{{"kind", "row"}, {"children", json::array({field})}}})}},
                          &why, TreeUse::HomeBody));
    CHECK(why == "column[0].row[0]: field can't be on Home");
    CHECK(parseTree(json{{"kind", "column"}, {"children", json::array({search})}}, &why));  // fine on a screen
}

TEST_CASE("ui tree: a quote takes text, an optional source and id; it counts one block and may be on Home") {
    const Block t = ok(json::parse(R"({"kind":"column","children":[
      {"kind":"quote","text":"It is a truth\nuniversally acknowledged","source":"Pride and Prejudice · p. 1","id":"passage"},
      {"kind":"quote","text":"  Alone  "}]})"));
    REQUIRE(t.children.size() == 2);
    const Block& q = t.children[0];
    CHECK(q.kind == K::Quote);
    CHECK(q.text == "It is a truth\nuniversally acknowledged");  // its line breaks kept, as a text block's
    CHECK(q.meta == "Pride and Prejudice · p. 1");
    CHECK(q.id == "passage");
    CHECK(t.children[1].text == "Alone");
    CHECK(t.children[1].meta.empty());
    CHECK(t.children[1].id.empty());
    CHECK(blockCount(t) == 3);
    std::string why;
    CHECK(parseTree(json::parse(R"({"kind":"quote","text":"x"})"), &why, TreeUse::HomeBody));
}

TEST_CASE("ui tree: a quote without text, with an unknown field or a duplicate id is refused, naming where") {
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"quote","source":"s"}]})")) == "column[0].quote: \"text\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"quote","text":"t","author":"a"})")) == "quote: unknown field \"author\"");
    CHECK(whyNot(json::parse(R"({"kind":"quote","text":7})")) == "quote: \"text\" must be a string");
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"button","id":"q","title":"B"},{"kind":"quote","id":"q","text":"t"}]})")) ==
          "column[1].quote: another block already has the id \"q\"");
}

// ── 0.12: layout primitives ──

namespace {
Block okHome(const json& j) {
    std::string why;
    auto t = parseTree(j, &why, TreeUse::HomeBody);
    INFO(why);
    REQUIRE(t);
    return *t;
}
std::string whyNotHome(const json& j) {
    std::string why;
    CHECK_FALSE(parseTree(j, &why, TreeUse::HomeBody));
    return why;
}
json col(json kid) { return {{"kind", "column"}, {"children", json::array({std::move(kid)})}}; }
}  // namespace

TEST_CASE("ui tree 0.12: widths, row alignment, the xsmall gap") {
    const Block t = ok(json::parse(R"({"kind":"row","align":"stretch","gap":"xsmall","children":[
      {"kind":"text","text":"a","width":"fill"},{"kind":"text","text":"b","width":"auto"},
      {"kind":"text","text":"c","width":"35%"},{"kind":"button","id":"x","title":"X","width":"100%"},{"kind":"text","text":"d"}]})"));
    CHECK(t.align == Block::Align::Stretch);
    CHECK(t.gap == Block::Gap::XSmall);
    CHECK(t.children[0].width == Block::Width::Fill);
    CHECK(t.children[1].width == Block::Width::Auto);
    CHECK(t.children[2].width == Block::Width::Percent);
    CHECK(t.children[2].widthPct == 35);
    CHECK(t.children[3].widthPct == 100);
    CHECK(t.children[4].width == Block::Width::Default);
    CHECK(ok(json::parse(R"({"kind":"row","children":[]})")).align == Block::Align::Center);  // as before 0.12
    for (const char* a : {"top", "center", "bottom"}) ok(json{{"kind", "row"}, {"align", a}, {"children", json::array()}});
    CHECK(whyNot(json::parse(R"({"kind":"row","align":"middle","children":[]})")) ==
          "row: \"align\" must be one of \"top\", \"center\", \"bottom\", \"stretch\"");
}

TEST_CASE("ui tree 0.12: a width that is not fill, auto or 1-100% is refused") {
    for (const char* w : {"0%", "101%", "-5%", "x%", "%", "50", "wide", "5 %", "1000%"})
        CHECK(whyNot(json{{"kind", "text"}, {"text", "t"}, {"width", w}}) ==
              "text: \"width\" must be \"fill\", \"auto\" or a percentage from 1% to 100%");
    CHECK_FALSE(whyNot(json{{"kind", "text"}, {"text", "t"}, {"width", 40}}).empty());
    // a width is only taken where a block can carry one: a deck's card can't
    CHECK(whyNot(json::parse(R"({"kind":"deck","id":"d","card":{"kind":"card","title":"t","width":"fill"}})")).find("width") != std::string::npos);
}

TEST_CASE("ui tree 0.12: fit only on a Home body's root column") {
    const json trim = {{"kind", "column"}, {"fit", "trim"}, {"children", json::array()}};
    CHECK(okHome(trim).fit == Block::Fit::Trim);
    CHECK(okHome(json{{"kind", "column"}, {"fit", "page"}, {"children", json::array()}}).fit == Block::Fit::Page);
    CHECK(okHome(json{{"kind", "column"}, {"children", json::array()}}).fit == Block::Fit::Page);
    CHECK(whyNot(trim) == "column.fit: only on a Home body's root");
    CHECK(whyNotHome(col(trim)) == "column[0].column.fit: only on a Home body's root");
    CHECK(whyNotHome(json{{"kind", "column"}, {"fit", "scroll"}, {"children", json::array()}}) ==
          "column: \"fit\" must be one of \"page\", \"trim\"");
    CHECK(whyNotHome(json{{"kind", "row"}, {"fit", "trim"}, {"children", json::array()}}) == "row: unknown field \"fit\"");
}

TEST_CASE("ui tree 0.12: spacer, box, text options") {
    const Block t = ok(json::parse(R"({"kind":"column","children":[
      {"kind":"spacer"},
      {"kind":"box","id":"go","pad":"xsmall","border":"thick","fill":"ink","radius":"round","child":{"kind":"text","text":"Go","tone":"inverse"}},
      {"kind":"box","child":{"kind":"spacer"}},
      {"kind":"text","text":"Mon","id":"day","font":"sans","size":"xsmall","weight":"medium","tone":"secondary","italic":true,"caps":true,"align":"end","lines":1},
      {"kind":"text","text":"plain"}]})"));
    CHECK(t.children[0].kind == K::Spacer);
    const Block& box = t.children[1];
    CHECK(box.kind == K::Box);
    CHECK(box.id == "go");
    CHECK(box.pad == Block::Gap::XSmall);
    CHECK(box.border == Block::Border::Thick);
    CHECK(box.fill == Block::Fill::Ink);
    CHECK(box.radius == Block::Radius::Round);
    REQUIRE(box.children.size() == 1);
    CHECK(box.children[0].tone == Block::Tone::Inverse);
    const Block& bare = t.children[2];
    CHECK(bare.pad == Block::Gap::None);
    CHECK(bare.border == Block::Border::None);
    CHECK(bare.fill == Block::Fill::None);
    CHECK(bare.radius == Block::Radius::None);
    CHECK(bare.id.empty());
    const Block& tx = t.children[3];
    CHECK(tx.id == "day");
    CHECK(tx.font == Block::Font::Sans);
    CHECK(tx.size == Block::Size::XSmall);
    CHECK(tx.weight == Block::Weight::Medium);
    CHECK(tx.tone == Block::Tone::Secondary);
    CHECK(tx.textAlign == Block::TextAlign::End);
    CHECK(tx.italic);
    CHECK(tx.caps);
    const Block& pl = t.children[4];
    CHECK_FALSE(pl.font);
    CHECK_FALSE(pl.size);
    CHECK_FALSE(pl.weight);
    CHECK_FALSE(pl.tone);  // unset stays unset: the drawing decides
    CHECK_FALSE(pl.textAlign);
    CHECK_FALSE(pl.italic);
    CHECK_FALSE(pl.caps);
    CHECK(whyNot(json::parse(R"({"kind":"box"})")) == "box: \"child\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"box","child":null})")) == "box: \"child\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"box","child":{"kind":"text","text":"t"},"fill":"dark"})")) ==
          "box: \"fill\" must be one of \"none\", \"light\", \"ink\"");
    CHECK(whyNot(json::parse(R"({"kind":"box","child":{"kind":"text","text":"t"},"border":"x"})")).find("\"border\" must be one of") != std::string::npos);
    CHECK(whyNot(json::parse(R"({"kind":"box","child":{"kind":"text","text":"t"},"radius":"x"})")).find("\"radius\" must be one of") != std::string::npos);
    CHECK(whyNot(json::parse(R"({"kind":"box","child":{"kind":"text","text":"t"},"pad":"huge"})")).find("\"pad\" must be one of") != std::string::npos);
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"t","font":"mono"})")) == "text: \"font\" must be one of \"serif\", \"sans\"");
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"t","size":"huge"})")).find("\"size\" must be one of") != std::string::npos);
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"t","weight":"heavy"})")).find("\"weight\" must be one of") != std::string::npos);
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"t","tone":"auto"})")).find("\"tone\" must be one of") != std::string::npos);
    CHECK(whyNot(json::parse(R"({"kind":"text","text":"t","align":"left"})")).find("\"align\" must be one of") != std::string::npos);
    // ids are unique across boxes and texts
    CHECK(whyNot(json::parse(R"({"kind":"column","children":[{"kind":"text","text":"a","id":"x"},{"kind":"box","id":"x","child":{"kind":"spacer"}}]})"))
              .find("another block already has the id") != std::string::npos);
    // a box's child is named by its path
    CHECK(whyNot(json::parse(R"({"kind":"box","child":{"kind":"quote"}})")) == "box.quote: \"text\" is missing");
}

TEST_CASE("ui tree 0.12: cover, progress, item") {
    const Block t = ok(json::parse(R"({"kind":"column","children":[
      {"kind":"cover","item":"ABCD1234","width":"30%"},
      {"kind":"cover","item":{"key":"K2","library":5,"label":"x"}},
      {"kind":"progress","value":0.4,"width":"fill"},{"kind":"progress","value":0},{"kind":"progress","value":1},
      {"kind":"item","item":"ITEM","child":{"kind":"row","children":[{"kind":"cover","item":"ITEM"},{"kind":"text","text":"t"}]}}]})"));
    const Block& cover = t.children[0];
    CHECK(cover.kind == K::Cover);
    REQUIRE(cover.items.size() == 1);
    CHECK(cover.items[0].key == "ABCD1234");
    CHECK(cover.width == Block::Width::Percent);
    CHECK(cover.widthPct == 30);
    CHECK(t.children[1].items[0].library == 5);
    CHECK(t.children[2].kind == K::Progress);
    CHECK(t.children[2].fraction == doctest::Approx(0.4));
    CHECK(t.children[2].width == Block::Width::Fill);
    CHECK(t.children[4].fraction == 1);
    const Block& item = t.children[5];
    CHECK(item.kind == K::Item);
    REQUIRE(item.items.size() == 1);
    CHECK(item.items[0].key == "ITEM");
    REQUIRE(item.children.size() == 1);
    CHECK(item.children[0].kind == K::Row);
    CHECK(whyNot(json::parse(R"({"kind":"cover"})")) == "cover: \"item\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"cover","item":""})")) == "cover.item: an item needs a key");
    CHECK(whyNot(json::parse(R"({"kind":"cover","item":3})")) == "cover.item: an item must be a key or { key, library?, label? }");
    CHECK(whyNot(json::parse(R"({"kind":"progress"})")) == "progress: \"value\" is missing");
    for (const char* v : {"-0.1", "1.01", "2", "\"0.5\"", "true"})
        CHECK(whyNot(json::parse(std::string(R"({"kind":"progress","value":)") + v + "}")) == "progress: \"value\" must be a number from 0 to 1");
    CHECK(whyNot(json::parse(R"({"kind":"item","item":"K"})")) == "item: \"child\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"item","child":{"kind":"spacer"}})")) == "item: \"item\" is missing");
}

TEST_CASE("ui tree 0.12: grid and image") {
    const Block g = ok(json::parse(R"({"kind":"grid","columns":"auto","minWidth":"20%","gap":"xsmall","rows":2,
      "children":[{"kind":"cover","item":"A"},null,{"kind":"cover","item":"B"}]})"));
    CHECK(g.kind == K::Grid);
    CHECK(g.columns == 0);
    CHECK(g.minWidthPct == 20);
    CHECK(g.gap == Block::Gap::XSmall);
    CHECK(g.maxRows == 2);
    CHECK(g.children.size() == 2);
    const Block d = ok(json::parse(R"({"kind":"grid","children":[]})"));
    CHECK(d.columns == 0);
    CHECK(d.minWidthPct == 25);
    CHECK(d.maxRows == 0);
    CHECK(d.gap == Block::Gap::Small);
    CHECK(ok(json::parse(R"({"kind":"grid","columns":12,"minWidth":"5%","rows":20,"children":[]})")).columns == 12);
    CHECK(ok(json::parse(R"({"kind":"grid","minWidth":"100%","rows":1,"children":[]})")).minWidthPct == 100);
    for (const char* c : {"0", "13", "\"many\"", "2.5"})
        CHECK(whyNot(json::parse(std::string(R"({"kind":"grid","children":[],"columns":)") + c + "}")) ==
              "grid: \"columns\" must be \"auto\" or a whole number from 1 to 12");
    for (const char* r : {"0", "21", "\"2\""})
        CHECK(whyNot(json::parse(std::string(R"({"kind":"grid","children":[],"rows":)") + r + "}")) ==
              "grid: \"rows\" must be a whole number from 1 to 20");
    for (const char* m : {"\"4%\"", "\"101%\"", "30", "\"x\""})
        CHECK(whyNot(json::parse(std::string(R"({"kind":"grid","children":[],"minWidth":)") + m + "}")) ==
              "grid: \"minWidth\" must be a percentage from 5% to 100%");
    CHECK(whyNot(json::parse(R"({"kind":"grid"})")) == "grid: \"children\" is missing");
    CHECK(whyNot(json::parse(R"({"kind":"grid","children":[{"kind":"quote"}]})")) == "grid[0].quote: \"text\" is missing");

    const Block im = ok(json::parse(R"({"kind":"image","src":"img/logo.png","width":"50%","id":"logo"})"));
    CHECK(im.kind == K::Image);
    CHECK(im.src == "img/logo.png");
    CHECK(im.widthPct == 50);
    CHECK(im.id == "logo");
    CHECK(ok(json::parse(R"({"kind":"image","src":"a..b/c.png"})")).src == "a..b/c.png");
    for (const char* src : {"/etc/x.png", "\\\\x.png", "C:/x.png", "../x.png", "a/../x.png", "a\\..\\x.png", "a/.."})
        CHECK_FALSE(whyNot(json{{"kind", "image"}, {"src", src}}).empty());
    CHECK(whyNot(json::parse(R"({"kind":"image","src":"/x.png"})")) == "image: \"src\" must be a path inside the plugin, not an absolute one");
    CHECK(whyNot(json::parse(R"({"kind":"image","src":"../x.png"})")) == "image: \"src\" must not contain \"..\"");
    CHECK(whyNot(json::parse(R"({"kind":"image","src":""})")) == "image: \"src\" must not be empty");
    CHECK(whyNot(json::parse(R"({"kind":"image"})")) == "image: \"src\" is missing");
}

TEST_CASE("ui tree 0.12: box, item and grid count as one block plus their children; a cover's ref is not extra") {
    const Block t = ok(json::parse(R"({"kind":"column","children":[
      {"kind":"box","child":{"kind":"text","text":"a"}},
      {"kind":"item","item":"K","child":{"kind":"cover","item":"K"}},
      {"kind":"grid","children":[{"kind":"cover","item":"A"},{"kind":"cover","item":"B"},{"kind":"spacer"}]},
      {"kind":"progress","value":0.5},{"kind":"image","src":"a.png"}]})"));
    // column 1 + box 2 + item 2 + grid 4 + progress 1 + image 1
    CHECK(blockCount(t) == 11);
}

TEST_CASE("ui tree 0.12: a tree of primitives is capped like any other") {
    json kids = json::array();
    for (int i = 0; i < 2000; ++i) kids.push_back({{"kind", "spacer"}});
    CHECK(whyNot(json{{"kind", "grid"}, {"children", kids}}).find("more than 2,000 blocks") != std::string::npos);
}

TEST_CASE("ui tree 0.12: a 0.11 tree parses to the same blocks it always did") {
    const json j = json::parse(R"({"kind":"column","gap":"large","children":[
      {"kind":"row","children":[{"kind":"text","text":"a","style":"label"},{"kind":"button","id":"b","title":"B"}]},
      {"kind":"items","id":"i","style":"shelf","items":["K1",{"key":"K2","library":3}]},
      {"kind":"quote","id":"q","text":"Q","source":"S"},{"kind":"space","size":"small"},{"kind":"notice","text":"n"}]})");
    const Block a = ok(j), b = ok(j);
    CHECK(sameBlock(a, b));
    CHECK(a.width == Block::Width::Default);
    CHECK(a.children[0].align == Block::Align::Center);
    CHECK(a.fit == Block::Fit::Page);
    CHECK(a.children[0].children[0].width == Block::Width::Default);
    CHECK_FALSE(a.children[0].children[0].tone);
    CHECK(a.children[0].children[1].widthPct == 0);
    CHECK(blockCount(a) == 1 + 3 + 3 + 1 + 1 + 1);  // column, row+text+button, items+2 refs, quote, space, notice
    // a primitive that differs is a different block (the diff)
    json other = j;
    other["children"][0]["children"][0]["tone"] = "secondary";
    CHECK_FALSE(sameBlock(a, ok(other)));
}
