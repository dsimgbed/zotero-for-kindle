#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/book.h"
#include "screens/library.h"
#include "screens/notes.h"
#include "screens/open_flow.h"
#include "store/db.h"
#include "ui/text.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kWall = 1'762'500'000'000;

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    OpenFlow flow{[] { return kWall; }};
    std::unique_ptr<LibraryScreen> screen;
    Rig() {
        fillFixtureLibrary(lib, 40);
        screen = std::make_unique<LibraryScreen>([this](const ItemRow& r) { flow.open(r, nav); },
                                                 [this](const std::string& k) { return flow.statusFor(k); });
        nav.visible = screen.get();
        screen->onShow(nav);
        LibraryView v;
        v.type = TypeFilter::Epub;  // EPUBs only
        screen->setView(v, nav);
        nav.damages.clear();
    }
    const ItemRow& row(int i) { return screen->list().rows()[static_cast<size_t>(i)]; }
    Rect rowRect(int i) { return screen->list().rowRects(LibraryScreen::listArea())[static_cast<size_t>(i)]; }
    void event(SyncEvent e) {
        flow.onSyncEvent(e, nav);
        screen->onSyncEvent(e, nav);
    }
    SyncEvent download(SyncEvent::Kind k, const ItemRow& r) {
        SyncEvent e;
        e.kind = k;
        e.itemKey = r.key;
        e.attachmentKey = r.attachmentKey;
        return e;
    }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

}  // namespace

TEST_CASE("open flow: a remote book downloads with progress in its row") {
    Rig rig;
    const ItemRow r = rig.row(1);
    rig.screen->onGesture(tapAt(rig.rowRect(1).center()), rig.nav);
    REQUIRE(rig.nav.syncFake.opened.size() == 1);
    CHECK(rig.nav.syncFake.opened[0] == r.key + "/" + r.attachmentKey);
    CHECK(rig.flow.statusFor(r.attachmentKey) == "Downloading…");
    rig.nav.damages.clear();
    SyncEvent p = rig.download(SyncEvent::Kind::DownloadProgress, r);
    p.done = 42;
    p.total = 100;
    rig.event(p);
    CHECK(rig.flow.statusFor(r.attachmentKey) == "Downloading 42%");
    {  // Zotero asks the download to wait: the row says so
        SyncEvent w = rig.download(SyncEvent::Kind::DownloadProgress, r);
        w.error = UserError{ErrorKind::RateLimited, "Zotero asks to wait until 14:03.", "HTTP 429"};
        rig.event(w);
        CHECK(rig.flow.statusFor(r.attachmentKey) == "Zotero asks to wait until 14:03.");
        rig.nav.damages.clear();
        rig.event(p);
    }
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Ui);
    CHECK(rig.nav.damages[0].rect == rig.rowRect(1));
    rig.screen->onGesture(tapAt(rig.rowRect(1).center()), rig.nav);  // a second tap while downloading
    CHECK(rig.nav.syncFake.opened.size() == 1);
}

TEST_CASE("open flow: a failed download shows the error in the row; a second tap retries") {
    Rig rig;
    const ItemRow r = rig.row(0);
    rig.screen->onGesture(tapAt(rig.rowRect(0).center()), rig.nav);
    SyncEvent done = rig.download(SyncEvent::Kind::DownloadFinished, r);
    done.error = UserError{ErrorKind::NotOnServer, "This file isn't on the server yet — sync it from Zotero on your computer.",
                           "HTTP 404"};
    rig.event(done);
    CHECK(rig.flow.statusFor(r.attachmentKey) ==
          "This file isn't on the server yet — sync it from Zotero on your computer. (HTTP 404)");
    CHECK(rig.nav.pushed.empty());
    rig.screen->onGesture(tapAt(rig.rowRect(0).center()), rig.nav);
    CHECK(rig.nav.syncFake.opened.size() == 2);
}

TEST_CASE("open flow: a finished download opens the book if you are still there") {
    Rig rig;
    const ItemRow r = rig.row(2);
    rig.screen->onGesture(tapAt(rig.rowRect(2).center()), rig.nav);
    rig.lib.setFileState(r.attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    rig.event(rig.download(SyncEvent::Kind::DownloadFinished, r));
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<BookScreen*>(rig.nav.pushed[0].get()));
    CHECK(rig.flow.statusFor(r.attachmentKey).empty());
    auto last = rig.lib.lastOpened();
    REQUIRE(last);
    CHECK(last->itemKey == r.key);
    CHECK(last->openedMs == kWall);

    // Somewhere else by the time it finishes: no surprise screen change.
    Rig away;
    const ItemRow r2 = away.row(2);
    away.screen->onGesture(tapAt(away.rowRect(2).center()), away.nav);
    away.nav.visible = nullptr;
    away.event(away.download(SyncEvent::Kind::DownloadFinished, r2));
    CHECK(away.nav.pushed.empty());
}

TEST_CASE("open flow: a downloaded book opens at once and refreshes its highlights") {
    Rig rig;
    const ItemRow r = rig.row(3);
    rig.lib.setFileState(r.attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    rig.screen->onShow(rig.nav);
    rig.screen->onGesture(tapAt(rig.rowRect(3).center()), rig.nav);
    CHECK(rig.nav.pushed.size() == 1);
    CHECK(rig.nav.syncFake.opened.size() == 1);
}

TEST_CASE("open flow: items without a readable file say why") {
    Rig rig;
    ItemRow none = *rig.lib.rowFor("IT000005");
    rig.flow.open(none, rig.nav);
    REQUIRE(rig.nav.toasts.size() == 1);
    CHECK(rig.nav.toasts[0] == "No EPUB, PDF or saved web page for this item.");
    ItemRow linked = *rig.lib.rowFor("IT000002");
    rig.flow.open(linked, rig.nav);
    CHECK(rig.nav.toasts.back() == "This file is only on your computer.");
    CHECK(rig.nav.syncFake.opened.empty());
}

TEST_CASE("book screen: shows loading until highlights arrive") {
    Rig rig;
    ItemRow r = *rig.lib.rowFor("IT000003");
    BookScreen book(r);
    book.onShow(rig.nav);
    CHECK(book.highlights().size() == 3);  // already in the library
    ItemRow fresh = *rig.lib.rowFor("IT000018");
    BookScreen loading(fresh);
    loading.onShow(rig.nav);
    CHECK(loading.loading());
    SyncEvent e = rig.download(SyncEvent::Kind::AnnotationsReady, fresh);
    loading.onSyncEvent(e, rig.nav);
    CHECK_FALSE(loading.loading());
}

TEST_CASE("notes: newest first, grouped by item; tap opens the book") {
    Rig rig;
    NotesScreen notes;
    notes.onShow(rig.nav);
    auto lines = notes.page();
    REQUIRE(lines.size() >= 4);
    CHECK(lines[0].header);
    CHECK_FALSE(lines[1].header);
    CHECK(lines[0].itemKey == lines[1].note.itemKey);
    CHECK(lines[1].note.annotation.dateModified >= lines[2].note.annotation.dateModified);
    auto rects = notes.lineRects();
    notes.onGesture(tapAt(rects[1].center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<BookScreen*>(rig.nav.pushed[0].get()));
}

TEST_CASE("book, notes and empty notes match the reviewed goldens") {
    if (!fonts()) return;
    Rig rig;
    Canvas c(1236, 1547);
    BookScreen book(*rig.lib.rowFor("IT000003"));
    book.onShow(rig.nav);
    book.draw(c);
    test::checkGolden(c, "tests/golden/book.png");
    NotesScreen notes;
    notes.onShow(rig.nav);
    notes.draw(c);
    test::checkGolden(c, "tests/golden/notes.png");
    Db emptyDb = Db::open(":memory:");
    Library emptyLib(emptyDb);
    FakeNav emptyNav(emptyLib);
    NotesScreen none;
    none.onShow(emptyNav);
    none.draw(c);
    test::checkGolden(c, "tests/golden/notes-empty.png");
}

TEST_CASE("open flow: with a reader available, a downloaded book opens in it") {
    Rig rig;
    struct Marker : Screen {
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
    };
    std::vector<std::string> made;
    rig.flow.setReaderFactory([&](const ItemRow& r, Nav&) -> std::unique_ptr<Screen> {
        made.push_back(r.key);
        return std::make_unique<Marker>();
    });
    const ItemRow r = rig.row(3);
    rig.lib.setFileState(r.attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    rig.screen->onShow(rig.nav);
    rig.screen->onGesture(tapAt(rig.rowRect(3).center()), rig.nav);
    REQUIRE(made.size() == 1);
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<Marker*>(rig.nav.pushed[0].get()));
}

// ---- zk.library.open's outcome (OpenFlow's `done`) ----

#include "screens/file_choice.h"

TEST_CASE("open flow done: a local file reports \"\" once, as the reader shows") {
    Rig rig;
    rig.lib.setFileState(rig.row(0).attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    ItemRow r = *rig.lib.rowFor(rig.row(0).key, rig.row(0).attachmentKey);
    std::vector<std::string> told;
    REQUIRE(r.fileState == AttachmentRecord::State::Local);
    rig.flow.open(r, rig.nav, std::nullopt, [&](const std::string& e) { told.push_back(e); });
    CHECK(told == std::vector<std::string>{""});
    CHECK_FALSE(rig.nav.pushed.empty());
}

TEST_CASE("open flow done: a finished download reports \"\"; a failed one its message; leaving first, \"Cancelled\"") {
    Rig rig;
    const ItemRow r = rig.row(1);
    std::vector<std::string> told;
    auto done = [&](const std::string& e) { told.push_back(e); };
    rig.flow.open(r, rig.nav, std::nullopt, done);
    rig.flow.open(r, rig.nav, std::nullopt, done);  // joins the download
    CHECK(told.empty());
    SyncEvent bad = rig.download(SyncEvent::Kind::DownloadFinished, r);
    bad.error = UserError{};
    bad.error->message = "Couldn't download the book";
    rig.flow.onSyncEvent(bad, rig.nav);
    CHECK(told == std::vector<std::string>{"Couldn't download the book", "Couldn't download the book"});
    told.clear();
    rig.flow.open(r, rig.nav, std::nullopt, done);  // a retry hears its own outcome
    rig.nav.visible = nullptr;                      // the user left
    rig.flow.onSyncEvent(rig.download(SyncEvent::Kind::DownloadFinished, r), rig.nav);
    CHECK(told == std::vector<std::string>{"Cancelled"});
    told.clear();
    rig.nav.visible = rig.screen.get();
    rig.flow.open(r, rig.nav, std::nullopt, done);
    rig.flow.onSyncEvent(rig.download(SyncEvent::Kind::DownloadFinished, r), rig.nav);
    CHECK(told == std::vector<std::string>{""});
}

TEST_CASE("open flow done: a reset cancels the waiting open") {
    Rig rig;
    const ItemRow r = rig.row(1);
    std::vector<std::string> told;
    rig.flow.open(r, rig.nav, std::nullopt, [&](const std::string& e) { told.push_back(e); });
    rig.flow.reset();
    CHECK(told == std::vector<std::string>{"Cancelled"});
}

TEST_CASE("open flow done: no file says why; backing out of \"Open which file?\" is \"Cancelled\"") {
    if (!fonts()) return;
    Rig rig;
    std::vector<std::string> told;
    auto done = [&](const std::string& e) { told.push_back(e); };
    ItemRow none = rig.row(0);
    none.attachmentKey.clear();
    rig.flow.open(none, rig.nav, std::nullopt, done);
    REQUIRE(told.size() == 1);
    CHECK(told[0].find("No EPUB, PDF or saved web page") == 0);
    told.clear();
    // an item with two readable files asks which
    ItemRow two = rig.row(0);
    AttachmentRecord a;
    a.key = "SECONDPD";
    a.parentKey = two.key;
    a.contentType = "application/pdf";
    a.filename = "second.pdf";
    a.linkMode = "imported_file";
    rig.lib.upsertAttachment(a);
    two = *rig.lib.rowFor(two.key);
    REQUIRE(two.files > 1);
    rig.flow.open(two, rig.nav, std::nullopt, done);
    REQUIRE_FALSE(rig.nav.pushed.empty());
    auto* choice = dynamic_cast<FileChoiceScreen*>(rig.nav.pushed.back().get());
    REQUIRE(choice);
    rig.nav.visible = choice;
    choice->onGesture(tapAt({metrics().width / 2, 5}), rig.nav);  // above the sheet
    CHECK(told == std::vector<std::string>{"Cancelled"});
}

TEST_CASE("library rowFor: a file the item doesn't have leaves the row not exactFile (zk.library.open rejects it)") {
    Rig rig;
    const ItemRow r = rig.row(0);
    auto base = rig.lib.rowFor(r.key, "NOSUCHFILE");
    REQUIRE(base);
    CHECK_FALSE(base->exactFile);
    CHECK(rig.lib.rowFor(r.key, r.attachmentKey)->exactFile);
}

// ---- audit C1: which screen the download was asked from ----

TEST_CASE("open flow fix: a new screen at the old one's address doesn't open the book") {
    Rig rig;
    const ItemRow r = rig.row(2);
    auto open = [&rig](const ItemRow& row) { rig.flow.open(row, rig.nav); };
    // One slot for both screens, so the second has the first's address (as the allocator may give it).
    alignas(LibraryScreen) unsigned char slot[sizeof(LibraryScreen)];
    auto* first = new (slot) LibraryScreen(open, rig.flow.statusFn());
    rig.nav.visible = first;
    rig.flow.open(r, rig.nav);
    first->~LibraryScreen();
    auto* second = new (slot) LibraryScreen(open, rig.flow.statusFn());
    REQUIRE(static_cast<const void*>(second) == static_cast<const void*>(first));
    rig.nav.visible = second;
    rig.lib.setFileState(r.attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    rig.flow.onSyncEvent(rig.download(SyncEvent::Kind::DownloadFinished, r), rig.nav);
    CHECK(rig.nav.pushed.empty());
    second->~LibraryScreen();
    rig.nav.visible = rig.screen.get();
}

TEST_CASE("open flow fix: a file kept later after a failed tap doesn't open by itself") {
    Rig rig;
    const ItemRow r = rig.row(0);
    rig.screen->onGesture(tapAt(rig.rowRect(0).center()), rig.nav);
    SyncEvent failed = rig.download(SyncEvent::Kind::DownloadFinished, r);
    failed.error = UserError{ErrorKind::NoWifi, "No Wi-Fi.", ""};
    rig.event(failed);
    {  // A kept collection's download of it runs and fails too: the row ends on the error, not a percentage.
        SyncEvent p = rig.download(SyncEvent::Kind::DownloadProgress, r);
        p.done = 42;
        p.total = 100;
        rig.event(p);
        SyncEvent again = rig.download(SyncEvent::Kind::DownloadFinished, r);
        again.error = UserError{ErrorKind::NotOnServer, "Not on the server.", "HTTP 404"};
        rig.event(again);
        CHECK(rig.flow.statusFor(r.attachmentKey) == "Not on the server. (HTTP 404)");
        CHECK(rig.nav.pushed.empty());
    }
    // Later the same file comes down as a kept collection's (no tap): it isn't opened, and the
    // row no longer shows the old error.
    rig.lib.setFileState(r.attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    rig.event(rig.download(SyncEvent::Kind::DownloadFinished, r));
    CHECK(rig.nav.pushed.empty());
    CHECK(rig.flow.statusFor(r.attachmentKey).empty());
}
