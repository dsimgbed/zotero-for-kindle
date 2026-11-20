#include <doctest/doctest.h>

#include "fixture_library.h"
#include "store/db.h"
#include "store/library.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <set>
#include <vector>
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

TEST_CASE("a cover never breaks a word: it shrinks, then cuts a word that still doesn't fit with …") {
    // Each word of what is set: a word of the title whole, or the start of one cut with "…"; and it fits.
    const auto check = [](const ItemRow& r, int w) {
        const CoverTitle t = coverTitle(r, w);
        std::vector<std::string> words, given;
        const auto split = [](const std::string& s, std::vector<std::string>& out) {
            for (size_t at = 0; at < s.size();) {
                const size_t end = std::min(s.find(' ', at), s.size());
                if (end > at) out.push_back(s.substr(at, end - at));
                at = end + 1;
            }
        };
        split(t.text, words);
        split(r.title, given);
        REQUIRE(words.size() == given.size());
        int cut = 0;
        for (size_t i = 0; i < words.size(); ++i) {
            CAPTURE(words[i]);
            CHECK(measureText(words[i], {theme::kSerif, t.pt, 700}, 10000, 1).width <= w - 2 * t.pad);
            if (words[i] == given[i]) continue;
            ++cut;
            const std::string ell = "…";
            REQUIRE(words[i].size() > ell.size());
            CHECK(words[i].compare(words[i].size() - ell.size(), ell.size(), ell) == 0);
            const std::string head = words[i].substr(0, words[i].size() - ell.size());
            CHECK(given[i].rfind(head, 0) == 0);
            CHECK(t.pt == 6);  // cut only once the title is as small as it gets
        }
        return cut;
    };
    ItemRow r = rowOf("FRANK001", "book", "EPUB");
    r.title = "Frankenstein; or, The Modern Prometheus";
    CHECK(check(r, 290) == 0);  // Home's card: every word whole
    check(r, 194);              // a pinned tile on a Paperwhite
    CHECK(check(r, 100) >= 1);  // too narrow for "Frankenstein;" even at 6 pt: cut, not broken
    ItemRow p = rowOf("TURING01", "journalArticle", "PDF");
    p.title = "On Computable Numbers, with an Application to the Entscheidungsproblem";
    CHECK(check(p, 120) >= 1);
}

TEST_CASE("a spaceless title (a file name, a URL) is cut with a bounded number of layouts") {
    // Covers are drawn at paint time, on every redraw: the cut must not lay out the word once per character.
    ItemRow r = rowOf("FILE0001", "attachment", "PDF");
    for (int i = 0; r.title.size() < 200; ++i) r.title += "2301.12345v" + std::to_string(i) + "_arxiv.org-abs-";
    for (const int w : {100, 120, 194, 290}) {
        CAPTURE(w);
        const long before = layoutsMade();
        const CoverTitle t = coverTitle(r, w);
        const long made = layoutsMade() - before;
        CHECK(made <= 16);
        CHECK(t.pt == 6);
        const std::string ell = "…";
        REQUIRE(t.text.size() > ell.size());
        CHECK(t.text.find(' ') == std::string::npos);
        CHECK(t.text.compare(t.text.size() - ell.size(), ell.size(), ell) == 0);
        const std::string head = t.text.substr(0, t.text.size() - ell.size());
        CHECK(r.title.rfind(head, 0) == 0);
        CHECK(measureText(t.text, {theme::kSerif, t.pt, 700}, 10000, 1).width <= w - 2 * t.pad);
        // the longest cut: one more character would not fit
        const std::string more = r.title.substr(0, head.size() + 1) + ell;
        CHECK(measureText(more, {theme::kSerif, t.pt, 700}, 10000, 1).width > w - 2 * t.pad);
    }
    // A title of many words measures each word a bounded number of times, too.
    ItemRow p = rowOf("PAPER002", "journalArticle", "PDF");
    p.title = "Attention Is All You Need: Transformers for Sequence Transduction Without Recurrence or Convolutions";
    const long before = layoutsMade();
    coverTitle(p, 120);
    CHECK(layoutsMade() - before <= 3 * 14);
}
