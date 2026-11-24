// Phase 1 tests: headless zk-sync JSON subcommands, ABI-safe PDF geometry inspection
// (primary C export + fallback parity), Library::createAnnotationWithKey, WriteGuard +
// SyncEngine + FakeZotero ink round-trips, and subprocess CLI execution.

#include <doctest/doctest.h>

#include "app/zk_sync_json.h"
#include "docs/engine_factory.h"
#include "fake_zotero.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "test_paths.h"
#include "zotero/api.h"

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace zk;
using zk::test::FakeZotero;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& stem) {
        static int seq = 0;
        path = test::uniqueTempPath(stem + "-" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeTextFile(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    f << content;
}

std::string readTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f.good());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Build a minimal valid 1-page PDF with custom MediaBox/CropBox and /Rotate.
std::string makeSyntheticPdf(double x0, double y0, double x1, double y1, int rotate) {
    auto fmt = [](double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", v);
        return std::string(b);
    };
    const std::string box = "[" + fmt(x0) + " " + fmt(y0) + " " + fmt(x1) + " " + fmt(y1) + "]";
    const std::string content = "q Q";
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox " + box + " /CropBox " + box + " /Rotate " + std::to_string(rotate) +
            " /Contents 4 0 R >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objs.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

void checkGeometryClose(const PdfPageGeometry& a, const PdfPageGeometry& b) {
    CHECK(a.pageIndex == b.pageIndex);
    CHECK(a.rotation == b.rotation);
    CHECK(a.pageLabel == b.pageLabel);
    CHECK(a.widthPt == doctest::Approx(b.widthPt).epsilon(0.02));
    CHECK(a.heightPt == doctest::Approx(b.heightPt).epsilon(0.02));
    for (size_t i = 0; i < 4; ++i) CHECK(a.cropBox[i] == doctest::Approx(b.cropBox[i]).epsilon(0.02));
    for (size_t i = 0; i < 6; ++i) CHECK(a.userToPage[i] == doctest::Approx(b.userToPage[i]).epsilon(0.02));
}

struct CmdResult {
    int exitCode = -1;
    std::string stdoutStr;
    std::string stderrStr;
};

CmdResult runProcess(const std::vector<std::string>& argv) {
    int outPipe[2], errPipe[2];
    REQUIRE(::pipe(outPipe) == 0);
    REQUIRE(::pipe(errPipe) == 0);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        std::vector<char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (const auto& s : argv) cargs.push_back(const_cast<char*>(s.c_str()));
        cargs.push_back(nullptr);
        ::execv(cargs[0], cargs.data());
        ::_exit(127);
    }
    ::close(outPipe[1]);
    ::close(errPipe[1]);

    auto readAll = [](int fd) {
        std::string s;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0) break;
            s.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);
        return s;
    };

    CmdResult res;
    res.stdoutStr = readAll(outPipe[0]);
    res.stderrStr = readAll(errPipe[0]);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    if (WIFEXITED(status)) res.exitCode = WEXITSTATUS(status);
    return res;
}

}  // namespace

TEST_CASE("zk-sync export-index: full catalog, attachment priority, disk check, atomic write, symlink safety") {
    TempDir tmp("zk-sync-export");
    Db db{Db::open(":memory:")};
    Library lib{db};

    lib.upsertCollection({"COL23456", "Philosophy of Science", "", 1});
    lib.upsertCollection({"COL34567", "Epistemology", "COL23456", 2});

    // Item 1: has EPUB, snapshot, and a cached PDF -> prefers PDF.
    ItemRecord item1;
    item1.key = "ABCD2345";
    item1.version = 10;
    item1.itemType = "book";
    item1.title = "The Structure of Scientific Revolutions";
    item1.creators = {{"Thomas", "Kuhn", "", "author"}};
    item1.year = "1962";
    item1.collections = {"COL23456", "COL34567"};
    item1.tags = {"epistemology", "classics"};
    lib.upsertItem(item1);

    const fs::path pdfPath = tmp.path / "kuhn1962.pdf";
    fs::copy_file("tests/data/pdf/sample.pdf", pdfPath);

    AttachmentRecord epub1;
    epub1.key = "EPUB2345";
    epub1.parentKey = "ABCD2345";
    epub1.contentType = "application/epub+zip";
    epub1.filename = "kuhn1962.epub";
    lib.upsertAttachment(epub1);

    AttachmentRecord snap1;
    snap1.key = "SNAP2345";
    snap1.parentKey = "ABCD2345";
    snap1.contentType = "text/html";
    snap1.filename = "kuhn1962.html";
    lib.upsertAttachment(snap1);

    AttachmentRecord pdf1;
    pdf1.key = "EFGH5678";
    pdf1.parentKey = "ABCD2345";
    pdf1.contentType = "application/pdf";
    pdf1.filename = "kuhn1962.pdf";
    pdf1.state = AttachmentRecord::State::Local;
    pdf1.localPath = pdfPath.string();
    lib.upsertAttachment(pdf1);
    lib.markOpened("ABCD2345", "EFGH5678", 1732349000000LL);

    // Item 2: PDF marked Local in DB, but missing on disk -> pdf_cached must be false!
    ItemRecord item2;
    item2.key = "BCDE3456";
    item2.version = 11;
    item2.itemType = "journalArticle";
    item2.title = "Missing Local File";
    item2.creators = {{"", "", "CERN Collaboration", "author"}};
    item2.year = "2024";
    lib.upsertItem(item2);

    AttachmentRecord pdf2;
    pdf2.key = "MPDF3456";
    pdf2.parentKey = "BCDE3456";
    pdf2.contentType = "application/pdf";
    pdf2.filename = "missing.pdf";
    pdf2.state = AttachmentRecord::State::Local;
    pdf2.localPath = (tmp.path / "does-not-exist.pdf").string();
    lib.upsertAttachment(pdf2);

    // Item 3: standalone PDF attachment.
    ItemRecord standalone;
    standalone.key = "STND4567";
    standalone.version = 12;
    standalone.itemType = "attachment";
    standalone.title = "Standalone Notes";
    lib.upsertItem(standalone);

    AttachmentRecord standaloneAtt;
    standaloneAtt.key = "STND4567";
    standaloneAtt.parentKey = "";
    standaloneAtt.contentType = "application/pdf";
    standaloneAtt.filename = "standalone.pdf";
    lib.upsertAttachment(standaloneAtt);

    const fs::path outPath = tmp.path / "index.json";
    json summary;
    std::string err;
    REQUIRE_MESSAGE(exportIndexJson(lib, outPath.string(), summary, &err), err);
    CHECK(summary["schema_version"] == 1);
    CHECK(summary["out_path"] == outPath.string());
    CHECK(summary["item_count"] == 3);
    CHECK(summary["exported_at"].get<int64_t>() > 0);

    const std::string raw = readTextFile(outPath);
    CHECK(raw.find("apiKey") == std::string::npos);
    CHECK(raw.find("webdavPassword") == std::string::npos);

    const json doc = json::parse(raw);
    CHECK(doc["schema_version"] == 1);
    CHECK(doc["item_count"] == 3);
    REQUIRE(doc["collections"].size() == 2);
    CHECK(doc["collections"][0]["key"] == "COL23456");
    CHECK(doc["collections"][0]["name"] == "Philosophy of Science");
    CHECK(doc["collections"][0]["parent_key"] == "");
    CHECK(doc["collections"][1]["key"] == "COL34567");
    CHECK(doc["collections"][1]["parent_key"] == "COL23456");

    REQUIRE(doc["items"].size() == 3);
    // Sorted deterministically by item_key: ABCD2345, BCDE3456, STND4567.
    const auto& j1 = doc["items"][0];
    CHECK(j1["item_key"] == "ABCD2345");
    CHECK(j1["attachment_key"] == "EFGH5678");
    CHECK(j1["citation_key"] == "Kuhn 1962");
    CHECK(j1["title"] == "The Structure of Scientific Revolutions");
    CHECK(j1["authors"] == "Kuhn");
    CHECK(j1["year"] == "1962");
    CHECK(j1["item_type"] == "book");
    CHECK(j1["collections"] == json::array({"Philosophy of Science", "Epistemology"}));
    CHECK(j1["collection_keys"] == json::array({"COL23456", "COL34567"}));
    CHECK(j1["tags"] == json::array({"epistemology", "classics"}));
    CHECK(j1["has_pdf"] == true);
    CHECK(j1["has_epub"] == true);
    CHECK(j1["has_snapshot"] == true);
    CHECK(j1["pdf_cached"] == true);
    CHECK(j1["pdf_local_path"] == pdfPath.string());
    CHECK(j1["last_opened"] == 1732349000000LL);

    const auto& j2 = doc["items"][1];
    CHECK(j2["item_key"] == "BCDE3456");
    CHECK(j2["attachment_key"] == "MPDF3456");
    CHECK(j2["has_pdf"] == true);
    CHECK(j2["pdf_cached"] == false);
    CHECK(j2["pdf_local_path"] == "");
    CHECK(j2["last_opened"] == 0);

    const auto& j3 = doc["items"][2];
    CHECK(j3["item_key"] == "STND4567");
    CHECK(j3["attachment_key"] == "STND4567");
    CHECK(j3["has_pdf"] == true);

    // Symlink target must be refused.
    const fs::path symTarget = tmp.path / "sym-index.json";
    fs::create_symlink(outPath, symTarget);
    CHECK_FALSE(exportIndexJson(lib, symTarget.string(), summary, &err));

    // Symlink parent directory must be refused.
    const fs::path realSub = tmp.path / "real-sub";
    fs::create_directories(realSub);
    const fs::path symSub = tmp.path / "sym-sub";
    fs::create_directory_symlink(realSub, symSub);
    CHECK_FALSE(exportIndexJson(lib, (symSub / "index.json").string(), summary, &err));
}

TEST_CASE("zk-sync pdf-info: normal, rotated, synthetic boxes, uncached, --fetch, and fallback parity") {
    TempDir tmp("zk-sync-pdf-info");

    // 1. Primary vs fallback parity on real and synthetic PDFs (0, 90, 180, 270, non-zero CropBox).
    const fs::path rot180 = tmp.path / "rot180.pdf";
    const fs::path rot270 = tmp.path / "rot270.pdf";
    const fs::path offsetBox = tmp.path / "offset_box.pdf";
    writeTextFile(rot180, makeSyntheticPdf(0, 0, 500, 700, 180));
    writeTextFile(rot270, makeSyntheticPdf(0, 0, 500, 700, 270));
    writeTextFile(offsetBox, makeSyntheticPdf(36, 72, 576, 720, 90));

    for (const std::string& p : {std::string("tests/data/pdf/sample.pdf"), std::string("tests/data/pdf/rotated.pdf"),
                                 rot180.string(), rot270.string(), offsetBox.string()}) {
        std::vector<PdfPageGeometry> primary, fallback;
        UserError uerr;
        REQUIRE_MESSAGE(inspectPdfPages(p, EngineEnv{}, primary, &uerr, /*forceFallback=*/false), uerr.details);
        REQUIRE_MESSAGE(inspectPdfPages(p, EngineEnv{}, fallback, &uerr, /*forceFallback=*/true), uerr.details);
        REQUIRE(primary.size() == fallback.size());
        REQUIRE_FALSE(primary.empty());
        for (size_t i = 0; i < primary.size(); ++i) checkGeometryClose(primary[i], fallback[i]);
    }

    // Check specific values on rotated.pdf (/Rotate 90 on US Letter 612x792 -> display 792x612).
    {
        std::vector<PdfPageGeometry> rotPages;
        UserError uerr;
        REQUIRE(inspectPdfPages("tests/data/pdf/rotated.pdf", EngineEnv{}, rotPages, &uerr));
        REQUIRE(rotPages.size() == 1);
        CHECK(rotPages[0].widthPt == doctest::Approx(792.0).epsilon(0.1));
        CHECK(rotPages[0].heightPt == doctest::Approx(612.0).epsilon(0.1));
        CHECK(rotPages[0].rotation == 90);
        CHECK(rotPages[0].cropBox[0] == doctest::Approx(0.0));
        CHECK(rotPages[0].cropBox[1] == doctest::Approx(0.0));
        CHECK(rotPages[0].cropBox[2] == doctest::Approx(612.0).epsilon(0.1));
        CHECK(rotPages[0].cropBox[3] == doctest::Approx(792.0).epsilon(0.1));
        // u=(0,0) -> q=(0,0), u=(0,792) -> q=(792,0), u=(612,0) -> q=(0,612)
        CHECK(rotPages[0].userToPage[0] == doctest::Approx(0.0));
        CHECK(rotPages[0].userToPage[1] == doctest::Approx(1.0));
        CHECK(rotPages[0].userToPage[2] == doctest::Approx(1.0));
        CHECK(rotPages[0].userToPage[3] == doctest::Approx(0.0));
    }

    // 2. pdfInfoJson: cached, uncached, --fetch, non-PDF error.
    Db db{Db::open(":memory:")};
    Library lib{db};
    AttachmentRecord remotePdf;
    remotePdf.key = "PDF23456";
    remotePdf.parentKey = "ITEM2345";
    remotePdf.contentType = "application/pdf";
    remotePdf.filename = "sample.pdf";
    remotePdf.state = AttachmentRecord::State::Remote;
    lib.upsertAttachment(remotePdf);

    AttachmentRecord epub;
    epub.key = "EPUB2345";
    epub.parentKey = "ITEM2345";
    epub.contentType = "application/epub+zip";
    epub.filename = "book.epub";
    lib.upsertAttachment(epub);

    json out;
    std::string err;
    // Uncached without --fetch -> exit ok with cached=false.
    REQUIRE_MESSAGE(pdfInfoJson(lib, "PDF23456", EngineEnv{}, /*fetch=*/false, nullptr, out, &err), err);
    CHECK(out["schema_version"] == 1);
    CHECK(out["attachment_key"] == "PDF23456");
    CHECK(out["parent_item_key"] == "ITEM2345");
    CHECK(out["filename"] == "sample.pdf");
    CHECK(out["cached"] == false);
    CHECK(out["fetched"] == false);
    CHECK(out["local_path"] == "");
    CHECK(out["page_count"] == 0);
    CHECK(out["pages"].empty());

    // --fetch downloads and updates SQLite state to Local.
    const fs::path fetchedPath = tmp.path / "PDF23456" / "sample.pdf";
    bool fetchCalled = false;
    PdfFetcher fetcher = [&](const AttachmentRecord& a, std::string& destOut, std::string*) {
        fetchCalled = true;
        CHECK(a.key == "PDF23456");
        fs::create_directories(fetchedPath.parent_path());
        fs::copy_file("tests/data/pdf/sample.pdf", fetchedPath);
        destOut = fetchedPath.string();
        return true;
    };
    REQUIRE_MESSAGE(pdfInfoJson(lib, "PDF23456", EngineEnv{}, /*fetch=*/true, fetcher, out, &err), err);
    CHECK(fetchCalled);
    CHECK(out["cached"] == true);
    CHECK(out["fetched"] == true);
    CHECK(out["local_path"] == fetchedPath.string());
    CHECK(out["page_count"] == 3);
    REQUIRE(out["pages"].size() == 3);
    CHECK(out["pages"][0]["page_index"] == 0);
    CHECK(out["pages"][0]["width_pt"] == doctest::Approx(612.0).epsilon(0.1));
    CHECK(out["pages"][0]["height_pt"] == doctest::Approx(792.0).epsilon(0.1));
    CHECK(out["pages"][0]["rotation"] == 0);
    CHECK(out["pages"][0]["user_to_page"].size() == 6);

    auto updatedAtt = lib.attachment("PDF23456");
    REQUIRE(updatedAtt);
    CHECK(updatedAtt->state == AttachmentRecord::State::Local);
    CHECK(updatedAtt->localPath == fetchedPath.string());

    // Calling again with --fetch does NOT re-fetch since already cached.
    fetchCalled = false;
    REQUIRE_MESSAGE(pdfInfoJson(lib, "PDF23456", EngineEnv{}, /*fetch=*/true, fetcher, out, &err), err);
    CHECK_FALSE(fetchCalled);
    CHECK(out["cached"] == true);
    CHECK(out["fetched"] == false);

    // Non-PDF, missing, invalid key rejected.
    CHECK_FALSE(pdfInfoJson(lib, "EPUB2345", EngineEnv{}, false, nullptr, out, &err));
    CHECK_FALSE(pdfInfoJson(lib, "ZZZZ2345", EngineEnv{}, false, nullptr, out, &err));
    CHECK_FALSE(pdfInfoJson(lib, "not-a-key", EngineEnv{}, false, nullptr, out, &err));
}

TEST_CASE("zk-sync ink-pull: ink, highlights, other_counts, tombstones excluded, and FakeZotero sync") {
    FakeZotero server;
    Db db{Db::open(":memory:")};
    Library lib{db};
    WriteGuard guard(server, [&](const std::string& k) { return lib.annotation(k).has_value(); },
                     [&](const std::string& k) { return lib.attachment(k).has_value(); });
    ZoteroApi api(guard, 1, "secret-key");
    api.setSleeper([](int) {});
    SyncEngine sync(api, lib);

    server.put(FakeZotero::item("BOOK2345", "Test Book"));
    json pdfAtt = FakeZotero::attachment("PDF23456", "BOOK2345", "md5abc");
    pdfAtt["contentType"] = "application/pdf";
    pdfAtt["filename"] = "test.pdf";
    server.put(pdfAtt);

    // Add ink, highlight, underline, note, image, text annotations on server.
    server.put({{"key", "INK23456"},
                {"itemType", "annotation"},
                {"parentItem", "PDF23456"},
                {"annotationType", "ink"},
                {"annotationComment", "margin sketch"},
                {"annotationColor", "#000000"},
                {"annotationPageLabel", "1"},
                {"annotationSortIndex", "00000|000000|00091"},
                {"annotationPosition", R"({"pageIndex":0,"width":2.5,"paths":[[100.5,700.25,120,695.5]]})"},
                {"tags", json::array({{{"tag", "sketch"}}})}});
    server.put({{"key", "HLT23456"},
                {"itemType", "annotation"},
                {"parentItem", "PDF23456"},
                {"annotationType", "highlight"},
                {"annotationText", "Normal science"},
                {"annotationComment", "key quote"},
                {"annotationColor", "#ffd400"},
                {"annotationPageLabel", "1"},
                {"annotationSortIndex", "00000|000010|00200"},
                {"annotationPosition", R"({"pageIndex":0,"rects":[[72,500,300,514]]})"},
                {"tags", json::array({{{"tag", "epistemology"}}})}});
    server.put({{"key", "UND23456"},
                {"itemType", "annotation"},
                {"parentItem", "PDF23456"},
                {"annotationType", "underline"},
                {"annotationText", "Paradigm shift"},
                {"annotationComment", ""},
                {"annotationColor", "#2ea8e5"},
                {"annotationPageLabel", "2"},
                {"annotationSortIndex", "00001|000005|00100"},
                {"annotationPosition", R"({"pageIndex":1,"rects":[[80,600,250,612]]})"},
                {"tags", json::array()}});
    server.put({{"key", "NOT23456"},
                {"itemType", "annotation"},
                {"parentItem", "PDF23456"},
                {"annotationType", "note"},
                {"annotationComment", "sticky"},
                {"annotationColor", "#ffd400"},
                {"annotationSortIndex", "00000|000000|00300"},
                {"annotationPosition", R"({"pageIndex":0,"rects":[[100,100,122,122]]})"},
                {"tags", json::array()}});
    server.put({{"key", "IMG23456"},
                {"itemType", "annotation"},
                {"parentItem", "PDF23456"},
                {"annotationType", "image"},
                {"annotationComment", ""},
                {"annotationColor", "#ff6666"},
                {"annotationSortIndex", "00000|000000|00400"},
                {"annotationPosition", R"({"pageIndex":0,"rects":[[50,50,200,200]]})"},
                {"tags", json::array()}});
    server.put({{"key", "TXT23456"},
                {"itemType", "annotation"},
                {"parentItem", "PDF23456"},
                {"annotationType", "text"},
                {"annotationComment", "textbox"},
                {"annotationColor", "#000000"},
                {"annotationSortIndex", "00000|000000|00500"},
                {"annotationPosition", R"({"pageIndex":0,"rects":[[50,250,200,280]]})"},
                {"tags", json::array()}});
    server.put({{"key", "INK99999"},
                {"itemType", "annotation"},
                {"parentItem", "PDF23456"},
                {"annotationType", "ink"},
                {"annotationComment", "to be tombstoned"},
                {"annotationColor", "#000000"},
                {"annotationSortIndex", "00000|000000|00600"},
                {"annotationPosition", R"({"pageIndex":0,"width":2,"paths":[[10,10,20,20]]})"},
                {"tags", json::array()}});

    REQUIRE_FALSE(sync.pull([](const SyncProgress&) {}).error);
    REQUIRE_FALSE(sync.pullAnnotationsFor("PDF23456", nullptr).error);

    // Tombstone one synced ink annotation locally; ink-pull must exclude it.
    REQUIRE(lib.deleteAnnotation("INK99999"));

    json out;
    std::string err;
    REQUIRE_MESSAGE(inkPullJson(lib, "PDF23456", /*synced=*/true, out, &err), err);
    CHECK(out["schema_version"] == 1);
    CHECK(out["attachment_key"] == "PDF23456");
    CHECK(out["synced"] == true);

    REQUIRE(out["ink"].size() == 1);
    CHECK(out["ink"][0]["key"] == "INK23456");
    CHECK(out["ink"][0]["version"].get<int64_t>() > 0);
    CHECK(out["ink"][0]["pageIndex"] == 0);
    CHECK(out["ink"][0]["width"] == 2.5);
    CHECK(out["ink"][0]["paths"] == json::array({json::array({100.5, 700.25, 120.0, 695.5})}));
    CHECK(out["ink"][0]["color"] == "#000000");
    CHECK(out["ink"][0]["comment"] == "margin sketch");
    CHECK(out["ink"][0]["tags"] == json::array({"sketch"}));
    CHECK(out["ink"][0]["dirty"] == false);
    CHECK(out["ink"][0]["conflict"] == false);

    REQUIRE(out["highlights"].size() == 2);
    CHECK(out["highlights"][0]["key"] == "HLT23456");
    CHECK(out["highlights"][0]["type"] == "highlight");
    CHECK(out["highlights"][0]["text"] == "Normal science");
    CHECK(out["highlights"][0]["rects"] == json::array({json::array({72.0, 500.0, 300.0, 514.0})}));
    CHECK(out["highlights"][1]["key"] == "UND23456");
    CHECK(out["highlights"][1]["type"] == "underline");

    CHECK(out["other_counts"]["note"] == 1);
    CHECK(out["other_counts"]["image"] == 1);
    CHECK(out["other_counts"]["text"] == 1);
}

TEST_CASE("zk-sync ink-upsert: create, client-key, update, delete, idempotency, sortIndex, validation, atomicity") {
    TempDir tmp("zk-sync-upsert");
    Db db{Db::open(":memory:")};
    Library lib{db};

    AttachmentRecord pdf;
    pdf.key = "EFGH5678";
    pdf.parentKey = "ABCD2345";
    pdf.contentType = "application/pdf";
    pdf.filename = "sample.pdf";
    pdf.state = AttachmentRecord::State::Local;
    pdf.localPath = "tests/data/pdf/sample.pdf";
    lib.upsertAttachment(pdf);

    AttachmentRecord otherPdf;
    otherPdf.key = "OTHR5678";
    otherPdf.parentKey = "ABCD9999";
    otherPdf.contentType = "application/pdf";
    otherPdf.filename = "other.pdf";
    lib.upsertAttachment(otherPdf);

    // Pre-existing highlight on EFGH5678 and ink on OTHR5678.
    AnnotationRecord hl;
    hl.key = "HL234567";
    hl.parentKey = "EFGH5678";
    hl.type = "highlight";
    hl.text = "Cannot overwrite";
    hl.position = R"({"pageIndex":0,"rects":[[72,500,200,512]]})";
    lib.upsertAnnotation(hl);

    AnnotationRecord otherInk;
    otherInk.key = "OT234567";
    otherInk.parentKey = "OTHR5678";
    otherInk.type = "ink";
    otherInk.position = R"({"pageIndex":0,"width":2,"paths":[[10,10,20,20]]})";
    lib.upsertAnnotation(otherInk);

    // 1. Create with minted key + create with client-supplied key; auto sortIndex from cached PDF.
    json p1 = {
        {"schema_version", 1},
        {"annotations",
         {
             {
                 {"key", ""},
                 {"pageIndex", 0},
                 {"width", 2.004},
                 {"color", "#ABCDEF"},
                 {"comment", "minted"},
                 {"tags", {"handwritten"}},
                 {"paths", {{100.504, 700.254, 120.0, 695.5}}},
             },
             {
                 {"key", "K9M2P4Q7"},
                 {"pageIndex", 0},
                 {"width", 3.25},
                 {"paths", {{50.0, 600.0, 80.0, 610.0}}},
             },
         }},
    };
    json out;
    std::string err;
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "EFGH5678", p1, EngineEnv{}, out, &err), err);
    CHECK(out["created"] == 2);
    CHECK(out["updated"] == 0);
    CHECK(out["deleted"] == 0);
    CHECK(out["unchanged"] == 0);
    REQUIRE(out["results"].size() == 2);
    const std::string mintedKey = out["results"][0]["key"].get<std::string>();
    CHECK(isZoteroKey(mintedKey));
    CHECK(out["results"][0]["action"] == "created");
    CHECK(out["results"][1]["key"] == "K9M2P4Q7");
    CHECK(out["results"][1]["action"] == "created");

    auto mintedRow = lib.annotation(mintedKey);
    REQUIRE(mintedRow);
    CHECK(mintedRow->color == "#abcdef");
    CHECK(mintedRow->pageLabel == "1");
    // Letter page height 792; max y in user space is 700.25 -> min top in display space is 792 - 700.25 = 91.75 -> floor 91!
    CHECK(mintedRow->sortIndex == "00000|000000|00091");
    CHECK(mintedRow->position == R"({"pageIndex":0,"paths":[[100.5,700.25,120.0,695.5]],"width":2.0})");

    // 2. Keyless idempotency: re-running identical keyless entry returns existing key as unchanged, zero duplicates!
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "EFGH5678", p1, EngineEnv{}, out, &err), err);
    CHECK(out["created"] == 0);
    CHECK(out["unchanged"] == 2);
    CHECK(out["results"][0]["key"] == mintedKey);
    CHECK(out["results"][0]["action"] == "unchanged");
    CHECK(out["results"][1]["key"] == "K9M2P4Q7");
    CHECK(out["results"][1]["action"] == "unchanged");
    // Still only 2 ink + 1 highlight = 3 annotations on EFGH5678.
    CHECK(lib.annotationsFor("EFGH5678").size() == 3);

    // 3. Keyed idempotency on a clean synced row: must NOT re-dirty it!
    auto syncedRow = *lib.annotation("K9M2P4Q7");
    syncedRow.version = 42;
    syncedRow.dirty = false;
    syncedRow.createdHere = false;
    lib.upsertAnnotation(syncedRow);

    json sameKeyed = {
        {"schema_version", 1},
        {"annotations", {{{"key", "K9M2P4Q7"}, {"pageIndex", 0}, {"width", 3.25}, {"paths", {{50.0, 600.0, 80.0, 610.0}}}}}},
    };
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "EFGH5678", sameKeyed, EngineEnv{}, out, &err), err);
    CHECK(out["unchanged"] == 1);
    CHECK(out["results"][0]["dirty"] == false);
    CHECK(lib.annotation("K9M2P4Q7")->dirty == false);
    CHECK(lib.annotation("K9M2P4Q7")->version == 42);

    // 4. Update existing by key (dirty becomes true, version stays 42).
    json editKeyed = {
        {"schema_version", 1},
        {"annotations",
         {{{"key", "K9M2P4Q7"},
           {"pageIndex", 0},
           {"width", 4.0},
           {"color", "#ff0000"},
           {"comment", "updated"},
           {"tags", {"revised"}},
           {"paths", {{50.0, 600.0, 90.0, 620.0}}}}}},
    };
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "EFGH5678", editKeyed, EngineEnv{}, out, &err), err);
    CHECK(out["updated"] == 1);
    CHECK(out["results"][0]["action"] == "updated");
    CHECK(out["results"][0]["version"] == 42);
    CHECK(out["results"][0]["dirty"] == true);
    auto edited = lib.annotation("K9M2P4Q7");
    REQUIRE(edited);
    CHECK(edited->dirty == true);
    CHECK(edited->color == "#ff0000");
    CHECK(edited->comment == "updated");
    CHECK(edited->tags == std::vector<std::string>{"revised"});

    // 5. Delete via empty paths: unsynced (mintedKey) hard-deleted; synced (K9M2P4Q7) tombstoned!
    json eraseBoth = {
        {"schema_version", 1},
        {"annotations",
         {
             {{"key", mintedKey}, {"pageIndex", 0}, {"paths", json::array()}},
             {{"key", "K9M2P4Q7"}, {"pageIndex", 0}, {"paths", json::array()}},
         }},
    };
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "EFGH5678", eraseBoth, EngineEnv{}, out, &err), err);
    CHECK(out["deleted"] == 2);
    CHECK_FALSE(lib.annotation(mintedKey).has_value());  // unsynced -> hard deleted
    auto tomb = lib.annotation("K9M2P4Q7");
    REQUIRE(tomb);
    CHECK(tomb->deletedHere == true);  // synced -> tombstone
    CHECK(tomb->dirty == true);

    // Re-running erase on already deleted/tombstoned keys is idempotent (unchanged).
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "EFGH5678", eraseBoth, EngineEnv{}, out, &err), err);
    CHECK(out["deleted"] == 0);
    CHECK(out["unchanged"] == 2);

    // Attempting to update a tombstoned key with non-empty paths must fail!
    CHECK_FALSE(inkUpsertJsonPayload(lib, "EFGH5678", editKeyed, EngineEnv{}, out, &err));

    // 6. delete_keys support.
    json createForDel = {
        {"schema_version", 1},
        {"annotations", {{{"key", "D9M2P4Q7"}, {"pageIndex", 0}, {"width", 2.0}, {"paths", {{10, 10, 20, 20}}}}}},
    };
    REQUIRE(inkUpsertJsonPayload(lib, "EFGH5678", createForDel, EngineEnv{}, out, &err));
    REQUIRE(lib.annotation("D9M2P4Q7").has_value());

    json delKeys = {{"schema_version", 1}, {"annotations", json::array()}, {"delete_keys", {"D9M2P4Q7", "N9M2P4Q7"}}};
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "EFGH5678", delKeys, EngineEnv{}, out, &err), err);
    CHECK(out["deleted"] == 1);
    CHECK(out["unchanged"] == 1);
    CHECK_FALSE(lib.annotation("D9M2P4Q7").has_value());

    // 7. Validation & transactional atomicity: invalid payload must leave DB completely untouched!
    const size_t beforeCount = lib.annotationsFor("EFGH5678").size();
    const std::vector<json> badPayloads = {
        // Wrong schema_version
        {{"schema_version", 2}, {"annotations", json::array()}},
        {{"schema_version", true}, {"annotations", json::array()}},
        // Bad key (contains '0' or 'O')
        {{"schema_version", 1}, {"annotations", {{{"key", "00000000"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{1, 2}}}}}}},
        {{"schema_version", 1}, {"annotations", {{{"key", "OOOOOOOO"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{1, 2}}}}}}},
        // Duplicate explicit keys
        {{"schema_version", 1},
         {"annotations",
          {{{"key", "DUP23456"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{1, 2}}}},
           {{"key", "DUP23456"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{3, 4}}}}}}},
        // Non-ink target
        {{"schema_version", 1}, {"annotations", {{{"key", "HL234567"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{1, 2}}}}}}},
        // Wrong parent attachment
        {{"schema_version", 1}, {"annotations", {{{"key", "OT234567"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{1, 2}}}}}}},
        // Odd-length stroke
        {{"schema_version", 1}, {"annotations", {{{"pageIndex", 0}, {"width", 2}, {"paths", {{1, 2, 3}}}}}}},
        // Empty stroke inside paths
        {{"schema_version", 1}, {"annotations", {{{"pageIndex", 0}, {"width", 2}, {"paths", {json::array()}}}}}},
        // NaN / Inf coordinate
        {{"schema_version", 1},
         {"annotations", {{{"pageIndex", 0}, {"width", 2}, {"paths", {{std::numeric_limits<double>::quiet_NaN(), 2.0}}}}}}},
        {{"schema_version", 1},
         {"annotations", {{{"pageIndex", 0}, {"width", 2}, {"paths", {{std::numeric_limits<double>::infinity(), 2.0}}}}}}},
        // Invalid color
        {{"schema_version", 1},
         {"annotations", {{{"pageIndex", 0}, {"width", 2}, {"color", "red"}, {"paths", {{1, 2}}}}}}},
        {{"schema_version", 1},
         {"annotations", {{{"pageIndex", 0}, {"width", 2}, {"color", "#fff"}, {"paths", {{1, 2}}}}}}},
        // Negative pageIndex
        {{"schema_version", 1}, {"annotations", {{{"pageIndex", -1}, {"width", 2}, {"paths", {{1, 2}}}}}}},
        // Invalid width (0, negative, > 200)
        {{"schema_version", 1}, {"annotations", {{{"pageIndex", 0}, {"width", 0}, {"paths", {{1, 2}}}}}}},
        {{"schema_version", 1}, {"annotations", {{{"pageIndex", 0}, {"width", -1}, {"paths", {{1, 2}}}}}}},
        {{"schema_version", 1}, {"annotations", {{{"pageIndex", 0}, {"width", 201}, {"paths", {{1, 2}}}}}}},
        // Multi-entry batch where entry 0 is valid and entry 1 is invalid (atomicity check!)
        {{"schema_version", 1},
         {"annotations",
          {{{"key", "ATM23456"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{10, 20, 30, 40}}}},
           {{"key", "ATM34567"}, {"pageIndex", 0}, {"width", 2}, {"paths", {{10, 20, 30}}}}}}},
    };
    for (const auto& bad : badPayloads) {
        CHECK_FALSE(inkUpsertJsonPayload(lib, "EFGH5678", bad, EngineEnv{}, out, &err));
    }
    CHECK(lib.annotationsFor("EFGH5678").size() == beforeCount);
    CHECK_FALSE(lib.annotation("ATM23456").has_value());

    // Malformed JSON file and symlink input file must be rejected.
    const fs::path badJsonFile = tmp.path / "bad.json";
    writeTextFile(badJsonFile, "{not valid json");
    CHECK_FALSE(inkUpsertJson(lib, "EFGH5678", badJsonFile.string(), EngineEnv{}, out, &err));

    const fs::path validFile = tmp.path / "valid.json";
    writeTextFile(validFile, json{{"schema_version", 1}, {"annotations", json::array()}}.dump());
    const fs::path symFile = tmp.path / "sym.json";
    fs::create_symlink(validFile, symFile);
    CHECK_FALSE(inkUpsertJson(lib, "EFGH5678", symFile.string(), EngineEnv{}, out, &err));
}

TEST_CASE("zk-sync ink-upsert: full WriteGuard + SyncEngine + FakeZotero create/update/delete round-trip") {
    FakeZotero server;
    Db db{Db::open(":memory:")};
    Library lib{db};
    WriteGuard guard(server, [&](const std::string& k) { return lib.annotation(k).has_value(); },
                     [&](const std::string& k) { return lib.attachment(k).has_value(); });
    ZoteroApi api(guard, 1, "secret-key");
    api.setSleeper([](int) {});
    SyncEngine sync(api, lib);

    server.put(FakeZotero::item("BOOK2345", "Roundtrip Book"));
    json pdfAtt = FakeZotero::attachment("PDF23456", "BOOK2345", "md5rt");
    pdfAtt["contentType"] = "application/pdf";
    pdfAtt["filename"] = "sample.pdf";
    server.put(pdfAtt);
    REQUIRE_FALSE(sync.pull([](const SyncProgress&) {}).error);
    lib.setFileState("PDF23456", AttachmentRecord::State::Local, "tests/data/pdf/sample.pdf");

    // 1. Create ink via ink-upsert -> push through WriteGuard -> verify FakeZotero payload.
    json out;
    std::string err;
    json createPayload = {
        {"schema_version", 1},
        {"annotations",
         {{{"key", "RT234567"},
           {"pageIndex", 0},
           {"width", 2.0},
           {"color", "#000000"},
           {"comment", "first stroke"},
           {"tags", {"scribe"}},
           {"paths", {{100.5, 700.25, 120.0, 695.5}}}}}},
    };
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "PDF23456", createPayload, EngineEnv{}, out, &err), err);
    CHECK(out["created"] == 1);

    SyncReport rep1 = sync.push();
    REQUIRE_FALSE(rep1.error);
    CHECK(rep1.annotations == 1);
    REQUIRE(server.items.count("RT234567") == 1);
    const json& srvCreated = server.items["RT234567"]["data"];
    CHECK(srvCreated["itemType"] == "annotation");
    CHECK(srvCreated["parentItem"] == "PDF23456");
    CHECK(srvCreated["annotationType"] == "ink");
    CHECK_FALSE(srvCreated.contains("annotationText"));  // MUST be omitted for ink!
    CHECK(srvCreated["annotationColor"] == "#000000");
    CHECK(srvCreated["annotationComment"] == "first stroke");
    CHECK(srvCreated["annotationSortIndex"] == "00000|000000|00091");
    CHECK(srvCreated["annotationPosition"].is_string());
    const auto localAfterCreate = lib.annotation("RT234567");
    REQUIRE(localAfterCreate);
    CHECK(localAfterCreate->dirty == false);
    CHECK(localAfterCreate->version > 0);
    const int64_t v1 = localAfterCreate->version;

    // 2. Update strokes via ink-upsert -> push through WriteGuard -> verify server updated.
    json updatePayload = {
        {"schema_version", 1},
        {"annotations",
         {{{"key", "RT234567"},
           {"pageIndex", 0},
           {"width", 3.5},
           {"color", "#2ea8e5"},
           {"comment", "second stroke"},
           {"tags", {"scribe", "updated"}},
           {"paths", {{100.5, 700.25, 140.0, 680.0}, {200.0, 650.0, 210.0, 640.0}}}}}},
    };
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "PDF23456", updatePayload, EngineEnv{}, out, &err), err);
    CHECK(out["updated"] == 1);

    SyncReport rep2 = sync.push();
    REQUIRE_FALSE(rep2.error);
    CHECK(rep2.annotations == 1);
    const json& srvUpdated = server.items["RT234567"]["data"];
    CHECK(srvUpdated["version"].get<int64_t>() > v1);
    CHECK_FALSE(srvUpdated.contains("annotationText"));
    CHECK(srvUpdated["annotationColor"] == "#2ea8e5");
    CHECK(srvUpdated["annotationComment"] == "second stroke");
    const json posUpdated = json::parse(srvUpdated["annotationPosition"].get<std::string>());
    CHECK(posUpdated["width"] == 3.5);
    CHECK(posUpdated["paths"].size() == 2);

    // 3. Clear strokes (paths: []) -> push through WriteGuard -> verify server deletion!
    json clearPayload = {
        {"schema_version", 1},
        {"annotations", {{{"key", "RT234567"}, {"pageIndex", 0}, {"paths", json::array()}}}},
    };
    REQUIRE_MESSAGE(inkUpsertJsonPayload(lib, "PDF23456", clearPayload, EngineEnv{}, out, &err), err);
    CHECK(out["deleted"] == 1);
    REQUIRE(lib.pendingDeletions().size() == 1);

    SyncReport rep3 = sync.push();
    REQUIRE_FALSE(rep3.error);
    CHECK(rep3.annotations == 1);
    CHECK(server.items.count("RT234567") == 0);
    CHECK(server.deletedItems.count("RT234567") == 1);
    CHECK_FALSE(lib.annotation("RT234567").has_value());
}

TEST_CASE("zk-sync CLI subprocess: offline --db execution, JSON stdout, exit codes 0/1/2") {
    const char* libDir = std::getenv("ZK_LIB_DIR");
    const fs::path bin = fs::path(libDir ? libDir : "build-desktop") / "zk-sync";
    REQUIRE_MESSAGE(fs::exists(bin), "zk-sync binary missing at " << bin.string());

    TempDir tmp("zk-sync-subproc");
    const fs::path dbPath = tmp.path / "test.db";
    const fs::path emptyCfg = tmp.path / "no-config";
    fs::create_directories(emptyCfg);

    {
        Db db = Db::open(dbPath.string());
        Library lib(db);
        ItemRecord it;
        it.key = "ABCD2345";
        it.itemType = "book";
        it.title = "CLI Test Book";
        it.year = "2026";
        lib.upsertItem(it);

        AttachmentRecord pdf;
        pdf.key = "EFGH5678";
        pdf.parentKey = "ABCD2345";
        pdf.contentType = "application/pdf";
        pdf.filename = "sample.pdf";
        pdf.state = AttachmentRecord::State::Local;
        pdf.localPath = fs::absolute("tests/data/pdf/sample.pdf").string();
        lib.upsertAttachment(pdf);
    }

    // 1. export-index -> exit 0, valid JSON stdout, no config/API key needed.
    const fs::path indexOut = tmp.path / "library_index.json";
    CmdResult rExport = runProcess(
        {bin.string(), "--config", emptyCfg.string(), "--db", dbPath.string(), "export-index", indexOut.string()});
    REQUIRE_MESSAGE(rExport.exitCode == 0, rExport.stderrStr);
    const json jExport = json::parse(rExport.stdoutStr);
    CHECK(jExport["schema_version"] == 1);
    CHECK(jExport["item_count"] == 1);
    CHECK(fs::exists(indexOut));

    // 2. pdf-info -> exit 0, valid JSON stdout with 3 pages.
    CmdResult rPdf =
        runProcess({bin.string(), "--config", emptyCfg.string(), "--db", dbPath.string(), "pdf-info", "EFGH5678"});
    REQUIRE_MESSAGE(rPdf.exitCode == 0, rPdf.stderrStr);
    const json jPdf = json::parse(rPdf.stdoutStr);
    CHECK(jPdf["schema_version"] == 1);
    CHECK(jPdf["cached"] == true);
    CHECK(jPdf["page_count"] == 3);

    // 3. ink-upsert -> exit 0, valid JSON stdout.
    const fs::path upsertIn = tmp.path / "upsert.json";
    writeTextFile(
        upsertIn,
        json{{"schema_version", 1},
             {"annotations",
              {{{"key", "CL234567"}, {"pageIndex", 0}, {"width", 2.0}, {"paths", {{100.0, 700.0, 150.0, 680.0}}}}}}}
            .dump());
    CmdResult rUpsert = runProcess({bin.string(), "--config", emptyCfg.string(), "--db", dbPath.string(), "ink-upsert",
                                    "EFGH5678", upsertIn.string()});
    REQUIRE_MESSAGE(rUpsert.exitCode == 0, rUpsert.stderrStr);
    const json jUpsert = json::parse(rUpsert.stdoutStr);
    CHECK(jUpsert["created"] == 1);
    CHECK(jUpsert["results"][0]["key"] == "CL234567");

    // 4. ink-pull --local-only AND ink-pull with --db (no creds) -> exit 0, valid JSON stdout.
    for (const std::vector<std::string>& cmd : {
             std::vector<std::string>{bin.string(), "--config", emptyCfg.string(), "--db", dbPath.string(), "ink-pull",
                                      "EFGH5678", "--local-only"},
             std::vector<std::string>{bin.string(), "--config", emptyCfg.string(), "--db", dbPath.string(), "ink-pull",
                                      "EFGH5678"},
         }) {
        CmdResult rPull = runProcess(cmd);
        REQUIRE_MESSAGE(rPull.exitCode == 0, rPull.stderrStr);
        const json jPull = json::parse(rPull.stdoutStr);
        CHECK(jPull["schema_version"] == 1);
        CHECK(jPull["synced"] == false);
        REQUIRE(jPull["ink"].size() == 1);
        CHECK(jPull["ink"][0]["key"] == "CL234567");
    }

    // 5. Runtime error -> exit 1, EMPTY stdout, error on stderr.
    CmdResult rErr =
        runProcess({bin.string(), "--config", emptyCfg.string(), "--db", dbPath.string(), "pdf-info", "ZZZZ2345"});
    CHECK(rErr.exitCode == 1);
    CHECK(rErr.stdoutStr.empty());
    CHECK_FALSE(rErr.stderrStr.empty());

    // 6. Usage error -> exit 2, EMPTY stdout, usage on stderr.
    CmdResult rUsage = runProcess({bin.string(), "--config", emptyCfg.string(), "--db", dbPath.string(), "export-index"});
    CHECK(rUsage.exitCode == 2);
    CHECK(rUsage.stdoutStr.empty());
    CHECK(rUsage.stderrStr.find("usage:") != std::string::npos);
}
