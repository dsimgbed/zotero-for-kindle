#include <doctest/doctest.h>

#include "fixture_library.h"
#include "store/db.h"
#include "store/library.h"
#include "ui/widgets.h"

#include <set>
#include <string>

using namespace zk;

namespace {

ItemRow rowOf(std::string key, std::string type, std::string badge) {
    ItemRow r;
    r.key = std::move(key);
    r.itemType = std::move(type);
    r.badge = std::move(badge);
    return r;
}

}  // namespace

TEST_CASE("cover looks") {
    CHECK(isBookRow(rowOf("A", "book", "PDF")));
    CHECK(isBookRow(rowOf("A", "bookSection", "")));
    CHECK(isBookRow(rowOf("A", "journalArticle", "EPUB")));
    CHECK(isBookRow(rowOf("A", "attachment", "EPUB")));
    CHECK_FALSE(isBookRow(rowOf("A", "journalArticle", "PDF")));
    CHECK_FALSE(isBookRow(rowOf("A", "attachment", "PDF")));

    CHECK(coverLook(rowOf("PAPER001", "journalArticle", "PDF")) == CoverLook::Paper);

    std::set<CoverLook> seen;
    for (int i = 0; i < 30; ++i) {
        const ItemRow r = rowOf("BK" + std::to_string(1000 + i * 37), "book", "EPUB");
        const CoverLook look = coverLook(r);
        CHECK(look != CoverLook::Paper);
        CHECK(coverLook(r) == look);
        seen.insert(look);
    }
    CHECK(seen.size() == 3);
}

TEST_CASE("library rows carry publication and year") {
    Db db = Db::open(":memory:");
    Library lib(db);
    zk::test::fillFixtureLibrary(lib, 12);
    ItemRecord r;
    r.key = "PUBART01";
    r.version = 1;
    r.itemType = "journalArticle";
    r.title = "Attention Is All You Need";
    r.publication = "NeurIPS";
    r.year = "2017";
    r.date = "2017-06-12";
    r.dateAdded = r.dateModified = "2024-03-05T10:20:30Z";
    r.json = "{}";
    lib.upsertItem(r);

    auto row = lib.rowFor("PUBART01");
    REQUIRE(row);
    CHECK(row->publication == "NeurIPS");
    CHECK(row->year == 2017);

    auto fixture = lib.rowFor(zk::test::fixtureKey("IT", 0));  // a journal article from the fixture
    REQUIRE(fixture);
    CHECK(fixture->itemType == "journalArticle");
    CHECK(fixture->year == 1962);
}
