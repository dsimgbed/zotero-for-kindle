#include <doctest/doctest.h>

#include "fake_http.h"
#include "fake_lipc.h"
#include "fake_zotero.h"
#include "fn_http.h"
#include "core/wake.h"
#include "platform/power.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_service.h"
#include "sync/webdav.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// A temp directory with a database file and a files folder; the server side is a FakeZotero for
// api.zotero.org and a FakeHttp for WebDAV.
struct Rig {
    fs::path dir;
    FakeZotero zotero;
    FakeHttp dav;
    FakeLipc lipc;
    KeepAwake keepAwake{lipc};
    std::mutex serverMutex;  // the worker thread uses the fakes; tests set them up before start
    std::function<HttpResponse(const HttpRequest&)> route;
    std::unique_ptr<SyncService> service;
    std::atomic<bool> wifi{true};
    std::atomic<uint64_t> freeBytes{uint64_t{1} << 40};
    std::atomic<int64_t> wallMs{1'762'000'000'000};
    int onlineGiveUpMs = 120'000;
    int openCapFloorMs = 600'000;

    Rig() {
        char tmpl[] = "/tmp/zk-svc-XXXXXX";
        dir = mkdtemp(tmpl);
        route = [this](const HttpRequest& r) {
            std::lock_guard<std::mutex> lock(serverMutex);
            if (r.url.rfind("https://api.zotero.org/", 0) == 0) return zotero.send(r);
            return dav.send(r);
        };
        zotero.put(FakeZotero::item("BOOK0001", "A book about owls"));
        zotero.put(FakeZotero::attachment("ABCD1234", "BOOK0001", "2eb101e67bfe381e248ca110b5aee500"));
        zotero.put(FakeZotero::annotation("ANNO0001", "ABCD1234", "owls are wise"));
    }
    ~Rig() {
        service.reset();
        fs::remove_all(dir);
    }
    std::string db() const { return (dir / "zotero.db").string(); }
    std::string files() const { return (dir / "files").string(); }

    void start(std::string webdav = "https://dav.example.org/zotero/") {
        SyncDeps deps;
        deps.makeHttp = [this] { return std::make_unique<FnHttp>([this](const HttpRequest& r) { return route(r); }); };
        deps.dbPath = db();
        deps.filesDir = files();
        deps.config.userId = 1;
        deps.config.apiKey = "k3y";
        deps.config.webdavUrl = std::move(webdav);
        deps.config.webdavUser = "u";
        deps.config.webdavPassword = "p";
        deps.keepAwake = &keepAwake;
        deps.wifiUp = [this] { return wifi.load(); };
        deps.freeBytes = [this] { return freeBytes.load(); };
        deps.wallMs = [this] { return wallMs.load(); };
        deps.onlinePollMs = 20;
        deps.onlineGiveUpMs = onlineGiveUpMs;
        deps.openCapFloorMs = openCapFloorMs;
        service = std::make_unique<SyncService>(std::move(deps));
        service->start();
    }

    // Collects events until `until` returns true or the timeout passes.
    std::vector<SyncEvent> waitFor(std::function<bool(const SyncEvent&)> until, int timeoutMs = 5000) {
        std::vector<SyncEvent> out;
        const auto t0 = Clock::now();
        while (msSince(t0) < timeoutMs) {
            SyncEvent e;
            while (service->poll(e)) {
                out.push_back(e);
                if (until(e)) return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("timed out waiting for a sync event");
        return out;
    }
    static auto is(SyncEvent::Kind k) {
        return [k](const SyncEvent& e) { return e.kind == k; };
    }
    void serveFile() {
        std::lock_guard<std::mutex> lock(serverMutex);
        dav.expect("GET", "https://dav.example.org/zotero/ABCD1234.prop", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.prop")));
        dav.expect("GET", "https://dav.example.org/zotero/ABCD1234.zip", FakeHttp::ok(slurp("tests/data/webdav/ABCD1234.zip")));
    }
};

int count(const std::vector<SyncEvent>& v, SyncEvent::Kind k) {
    return static_cast<int>(std::count_if(v.begin(), v.end(), [k](const SyncEvent& e) { return e.kind == k; }));
}

}  // namespace

TEST_CASE("sync service: a sync runs on the worker and the UI's own connection sees the items") {
    Rig rig;
    rig.start();
    rig.service->syncNow();
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK(count(events, SyncEvent::Kind::SyncStarted) == 1);
    CHECK(count(events, SyncEvent::Kind::SyncProgress) >= 1);
    CHECK_FALSE(events.back().error);
    CHECK(events.back().report.itemsAdded == 1);
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.recentItems(10).size() == 1);
    CHECK(rig.lipc.setsCopy().back() == "com.lab126.powerd preventScreenSaver 0");  // keep-awake released
}

TEST_CASE("sync service: sync requests made while busy are coalesced") {
    Rig rig;
    std::atomic<int> listings{0};
    rig.route = [&listings, base = rig.route](const HttpRequest& r) {
        if (r.url.find("collections?since=") != std::string::npos) {
            ++listings;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return base(r);
    };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.service->syncNow();
    rig.service->syncNow();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(listings == 2);
}

TEST_CASE("sync service: opening a remote book downloads it, then its annotations") {
    Rig rig;
    rig.serveFile();
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.service->openBook("BOOK0001", "ABCD1234");
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::AnnotationsReady));
    CHECK(count(events, SyncEvent::Kind::DownloadProgress) >= 1);
    auto done = std::find_if(events.begin(), events.end(), Rig::is(SyncEvent::Kind::DownloadFinished));
    REQUIRE(done != events.end());
    CHECK_FALSE(done->error);
    CHECK(done->attachmentKey == "ABCD1234");
    CHECK(fs::exists(fs::path(rig.files()) / "ABCD1234" / "book.epub"));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ABCD1234")->state == AttachmentRecord::State::Local);
    CHECK(ui.annotationsFor("ABCD1234").size() == 1);
    CHECK(ui.isTracked("ABCD1234"));
}

TEST_CASE("sync service: a missing file is reported and leaves nothing behind") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.serverMutex);
        rig.dav.expect("GET", "https://dav.example.org/zotero/ABCD1234.prop", FakeHttp::status(404));
    }
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.service->openBook("BOOK0001", "ABCD1234");
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::DownloadFinished));
    REQUIRE(events.back().error);
    CHECK(events.back().error->kind == ErrorKind::NotOnServer);
    CHECK((!fs::exists(rig.files()) || fs::is_empty(rig.files())));
}

TEST_CASE("sync service: stop returns quickly while a request hangs, and releases keep-awake") {
    Rig rig;
    rig.route = [](const HttpRequest& r) {
        const auto t0 = Clock::now();
        while (msSince(t0) < 30000 && !(r.cancel && *r.cancel))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        HttpResponse resp;
        resp.curlCode = 42;
        resp.curlError = "cancelled";
        return resp;
    };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = Clock::now();
    rig.service->stop();
    CHECK(msSince(t0) < 2000);
    CHECK(rig.lipc.setsCopy().back() == "com.lab126.powerd preventScreenSaver 0");
}

TEST_CASE("sync service: stop interrupts a Retry-After wait") {
    Rig rig;
    rig.route = [](const HttpRequest&) { return FakeHttp::status(429, {{"retry-after", "60"}}); };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto t0 = Clock::now();
    rig.service->stop();
    CHECK(msSince(t0) < 2000);
}

TEST_CASE("sync service: a read tag Zotero refuses is told once, with the push; the sync goes on") {
    Rig rig;
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    {
        Db db = Db::open(rig.db());
        Library ui(db);
        ui.markRead("BOOK0001", true);
    }
    {
        std::lock_guard<std::mutex> lock(rig.serverMutex);
        rig.zotero.failObjects["BOOK0001"] = 400;
    }
    rig.service->syncNow();
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    int told = 0;
    for (const auto& e : events) told += static_cast<int>(e.report.notices.size());
    CHECK(told == 1);
    CHECK_FALSE(events.back().error);
}

TEST_CASE("sync service: when Zotero asks for a long wait, no sync asks it again before then, across restarts") {
    Rig rig;
    std::atomic<bool> busy{true};
    std::atomic<int> asked{0};
    rig.route = [&, base = rig.route](const HttpRequest& r) {
        if (r.url.rfind("https://api.zotero.org/", 0) == 0) {
            ++asked;
            if (busy) return FakeHttp::status(429, {{"retry-after", "3600"}});
        }
        return base(r);
    };
    rig.start();
    rig.service->syncNow();
    auto first = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    REQUIRE(first.back().error);
    CHECK(first.back().error->kind == ErrorKind::RateLimited);
    const int before = asked;
    CHECK(before >= 1);
    busy = false;  // Zotero would answer now, but asked for an hour
    const std::string expected = rateLimitMessage(rig.wallMs + 3600 * 1000);
    // A tapped Sync: the message, without asking Zotero.
    const uint64_t id = rig.service->syncNowTracked();
    auto tapped = rig.waitFor([&](const SyncEvent& e) { return e.kind == SyncEvent::Kind::SyncFinished && e.request == id; });
    REQUIRE(tapped.back().error);
    CHECK(tapped.back().error->kind == ErrorKind::RateLimited);
    CHECK(tapped.back().error->message == expected);
    rig.service->syncWhenOnline();  // waking up: no request either
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.service.reset();  // a restart remembers it
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK(asked == before);
    rig.wallMs += int64_t{3601} * 1000;  // the hour is over: the next sync goes
    rig.service->syncNow();
    auto later = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK_FALSE(later.back().error);
    CHECK(asked > before);
}

TEST_CASE("sync service: a book that keeps dripping in past its time ends with an error; the next open works") {
    Rig rig;
    rig.openCapFloorMs = 150;  // the cap: max(this, size / 20 KB/s) = 200 ms for these 4 KB
    std::atomic<bool> drip{true};
    rig.route = [&, base = rig.route](const HttpRequest& r) {
        if (drip && r.url.size() > 4 && r.url.compare(r.url.size() - 4, 4, ".zip") == 0) {
            const auto t0 = Clock::now();
            for (int64_t done = 1; msSince(t0) < 5000; ++done) {  // a byte at a time, never stalling
                if (r.onProgress) r.onProgress(done, 4096);
                if (r.cancel && *r.cancel) {
                    HttpResponse aborted;
                    aborted.curlCode = 42;
                    aborted.curlError = "Callback aborted";
                    return aborted;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return FakeHttp::status(500);
        }
        return base(r);
    };
    rig.serveFile();
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    const auto t0 = Clock::now();
    rig.service->openBook("BOOK0001", "ABCD1234");
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::DownloadFinished));
    CHECK(msSince(t0) < 3000);
    REQUIRE(events.back().error);
    CHECK(events.back().error->message.find("too slowly") != std::string::npos);
    drip = false;  // a good line again: the same book opens
    rig.serveFile();
    rig.service->openBook("BOOK0001", "ABCD1234");
    auto again = rig.waitFor(Rig::is(SyncEvent::Kind::DownloadFinished));
    CHECK_FALSE(again.back().error);
}

TEST_CASE("sync service: poll never waits for the worker") {
    Rig rig;
    rig.route = [](const HttpRequest& r) {
        const auto t0 = Clock::now();
        while (msSince(t0) < 3000 && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return FakeHttp::status(500);
    };
    rig.start();
    rig.service->syncNow();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(rig.service->busy());
    const auto t0 = Clock::now();
    SyncEvent e;
    for (int i = 0; i < 100; ++i) rig.service->poll(e);
    CHECK(msSince(t0) < 5);
}

TEST_CASE("sync service: removing downloads deletes the files and marks them remote") {
    Rig rig;
    rig.serveFile();
    rig.start();
    rig.service->syncNow();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    rig.service->openBook("BOOK0001", "ABCD1234");
    rig.waitFor(Rig::is(SyncEvent::Kind::AnnotationsReady));
    rig.service->removeDownloads();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK_FALSE(fs::exists(fs::path(rig.files()) / "ABCD1234"));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ABCD1234")->state == AttachmentRecord::State::Remote);
}

TEST_CASE("sync service: sync-when-online waits on the worker until Wi-Fi is back") {
    Rig rig;
    rig.wifi = false;
    rig.start();
    rig.service->syncWhenOnline();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SyncEvent e;
    CHECK_FALSE(rig.service->poll(e));  // nothing yet
    rig.wifi = true;
    auto events = rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    CHECK_FALSE(events.back().error);
}

TEST_CASE("sync service: opening a book goes ahead of a queued sync") {
    Rig rig;
    rig.serveFile();
    std::atomic<bool> release{false};
    rig.route = [&release, base = rig.route](const HttpRequest& r) {
        if (r.url.find("collections?since=") != std::string::npos)
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return base(r);
    };
    rig.start();
    rig.service->syncNow();  // runs (blocked)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.service->syncNow();  // queued
    rig.service->openBook("BOOK0001", "ABCD1234");  // must jump ahead of the queued sync
    release = true;
    std::vector<SyncEvent> events = rig.waitFor(Rig::is(SyncEvent::Kind::DownloadFinished), 8000);
    CHECK(count(events, SyncEvent::Kind::SyncFinished) == 1);  // only the running one finished first
}

TEST_CASE("sync service: waiting for Wi-Fi doesn't hold the Kindle awake; the sync once online does") {
    Rig rig;
    rig.wifi = false;
    rig.onlineGiveUpMs = 300;
    rig.start();
    rig.service->syncWhenOnline();  // woken with Wi-Fi off
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // the wait has run out
    for (const auto& s : rig.lipc.setsCopy()) CHECK(s.find("preventScreenSaver 1") == std::string::npos);
    rig.wifi = true;
    rig.service->syncWhenOnline();
    rig.waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    const auto sets = rig.lipc.setsCopy();
    REQUIRE(sets.size() >= 2);
    CHECK(sets.front() == "com.lab126.powerd preventScreenSaver 1");
    rig.service->stop();
    CHECK(rig.lipc.setsCopy().back() == "com.lab126.powerd preventScreenSaver 0");
}

namespace {
std::atomic<int> gSyncWakes{0};
void countSyncWake() { ++gSyncWakes; }
}  // namespace

TEST_CASE("sync service: every event wakes the UI loop, which doesn't poll for them") {
    Rig rig;
    uiWakeTaken();
    gSyncWakes = 0;
    setUiWaker(countSyncWake);
    rig.start();
    rig.service->syncNow();
    std::vector<SyncEvent> events;
    const auto t0 = Clock::now();
    while (msSince(t0) < 5000) {  // as the loop: look only after a wake
        if (!uiWakePending()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        uiWakeTaken();
        SyncEvent e;
        while (rig.service->poll(e)) events.push_back(e);
        if (!events.empty() && events.back().kind == SyncEvent::Kind::SyncFinished) break;
    }
    setUiWaker(nullptr);
    uiWakeTaken();
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().kind == SyncEvent::Kind::SyncFinished);
    CHECK(gSyncWakes >= 1);
}

// ---- kept collections (spec B) ---------------------------------------------------------------

namespace {

// A library with Shelf > Inner and books in each (and one outside), served from Zotero storage:
// /items/<key>/file redirects to files.example, whose body is "the book <key>".
struct KeptRig : Rig {
    std::mutex fileMutex;
    std::vector<std::string> fetched;  // attachment keys whose file was asked for, in order
    std::function<void(const HttpRequest&)> onFile;  // e.g. hold one back
    std::map<std::string, long> fileStatus;          // answer /file for this key with this status
    std::set<std::string> stalls;                    // these files start, then stall (curl 28)

    KeptRig() {
        zotero.putCollection("SHELF001", "Shelf");
        zotero.putCollection("INNER001", "Inner", "SHELF001");
        book("BOOKA001", "ATTA0001", "SHELF001");
        book("BOOKB001", "ATTB0001", "INNER001");
        book("BOOKC001", "ATTC0001", "");
        route = [this, base = route](const HttpRequest& r) {
            const std::string api = "https://api.zotero.org/users/1/items/";
            if (r.url.rfind(api, 0) == 0 && r.url.size() > api.size() + 8 && r.url.compare(r.url.size() - 5, 5, "/file") == 0) {
                const std::string key = r.url.substr(api.size(), 8);
                {
                    std::lock_guard<std::mutex> lock(fileMutex);
                    fetched.push_back(key);
                }
                if (onFile) onFile(r);
                {
                    std::lock_guard<std::mutex> lock(fileMutex);
                    if (auto it = fileStatus.find(key); it != fileStatus.end()) return FakeHttp::status(it->second);
                }
                if (r.cancel && *r.cancel) {
                    HttpResponse aborted;
                    aborted.curlCode = 42;
                    aborted.curlError = "cancelled";
                    return aborted;
                }
                return FakeHttp::status(302, {{"location", "https://files.example/" + key}});
            }
            if (r.url.rfind("https://files.example/", 0) == 0) {
                HttpResponse ok = FakeHttp::ok(content(r.url.substr(22)));
                bool stalled;
                {
                    std::lock_guard<std::mutex> lock(fileMutex);
                    stalled = stalls.count(r.url.substr(22)) > 0;
                }
                if (stalled) {
                    if (r.onProgress) r.onProgress(3, static_cast<int64_t>(ok.body.size()));
                    HttpResponse timedOut;
                    timedOut.curlCode = 28;
                    timedOut.curlError = "Operation too slow";
                    return timedOut;
                }
                if (r.onProgress) {  // like CurlHttp: the size is known early
                    r.onProgress(0, static_cast<int64_t>(ok.body.size()));
                    if (r.cancel && *r.cancel) {
                        HttpResponse aborted;
                        aborted.curlCode = 42;
                        aborted.curlError = "Callback aborted";
                        return aborted;
                    }
                }
                if (!r.outFile.empty()) {
                    std::ofstream(r.outFile, std::ios::binary) << ok.body;
                    ok.body.clear();
                }
                return ok;
            }
            return base(r);
        };
    }
    static std::string content(const std::string& key) { return "the book " + key; }
    static std::string md5(const std::string& key) {
        const std::string path = "/tmp/zk-kept-md5-" + key;
        std::ofstream(path, std::ios::binary) << content(key);
        const std::string h = md5OfFile(path);
        fs::remove(path);
        return h;
    }
    void book(const std::string& item, const std::string& att, const std::string& collection) {
        std::lock_guard<std::mutex> lock(serverMutex);
        FakeZotero::json it = FakeZotero::item(item, "Book " + item);
        if (!collection.empty()) it["collections"] = FakeZotero::json::array({collection});
        zotero.put(it);
        zotero.put(FakeZotero::attachment(att, item, md5(att)));
    }
    void startOnZotero() { start(""); }
    bool downloaded(const std::string& att) { return fs::exists(fs::path(files()) / att / "book.epub"); }
    void stall(const std::string& key, bool on = true) {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (on) stalls.insert(key);
        else stalls.erase(key);
    }
    void answerFile(const std::string& key, long status) {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (status) fileStatus[key] = status;
        else fileStatus.erase(key);
    }
    int fetches(const std::string& key) {
        std::lock_guard<std::mutex> lock(fileMutex);
        return static_cast<int>(std::count(fetched.begin(), fetched.end(), key));
    }
    std::vector<std::string> fetchedCopy() {
        std::lock_guard<std::mutex> lock(fileMutex);
        return fetched;
    }
    void keep(const std::string& collection, bool on = true) {
        Db db = Db::open(this->db());
        Library ui(db);
        ui.setCollectionKept(collection, on);
    }
    // Waits until `done` holds (polling the events away meanwhile).
    bool waitUntil(std::function<bool()> done, int timeoutMs = 5000) {
        const auto t0 = Clock::now();
        while (msSince(t0) < timeoutMs) {
            SyncEvent e;
            while (service->poll(e)) {
            }
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void syncAndWait() {
        service->syncNow();
        waitFor(Rig::is(SyncEvent::Kind::SyncFinished));
    }
};

}  // namespace

TEST_CASE("kept: turning keeping on downloads the missing files, subcollections included") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    CHECK(rig.fetchedCopy().empty());  // nothing kept yet: a sync downloads nothing
    rig.keep("SHELF001");
    rig.service->downloadKept();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    CHECK(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));  // done: the row says nothing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(rig.downloaded("ATTC0001"));  // not in the collection
    const auto fetched = rig.fetchedCopy();
    CHECK(std::set<std::string>(fetched.begin(), fetched.end()) == std::set<std::string>{"ATTA0001", "ATTB0001"});
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ATTA0001")->state == AttachmentRecord::State::Local);
    CHECK(ui.attachment("ATTB0001")->state == AttachmentRecord::State::Local);
}

TEST_CASE("kept: while catching up, each step says which of how many; events wake the UI") {
    KeptRig rig;
    std::atomic<bool> release{false};
    rig.onFile = [&](const HttpRequest& r) {
        while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.service->keptStatus("SHELF001").has_value(); }));
    const auto st = *rig.service->keptStatus("SHELF001");
    CHECK(st.current == 1);
    CHECK(st.total == 2);
    CHECK_FALSE(st.paused);
    release = true;
    auto events = rig.waitFor([&](const SyncEvent& e) {
        return e.kind == SyncEvent::Kind::KeptChanged && !rig.service->keptStatus("SHELF001");
    });
    CHECK(count(events, SyncEvent::Kind::DownloadFinished) >= 1);
}

TEST_CASE("kept: new items after a sync download too") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    rig.book("BOOKD001", "ATTD0001", "INNER001");  // added on the computer
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTD0001"); }));
}

TEST_CASE("kept: turning it off stops future downloads and keeps the files") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    rig.keep("SHELF001", false);
    rig.book("BOOKD001", "ATTD0001", "SHELF001");
    rig.syncAndWait();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(rig.downloaded("ATTD0001"));
    CHECK(rig.downloaded("ATTA0001"));  // nothing deleted
    CHECK(rig.downloaded("ATTB0001"));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ATTA0001")->state == AttachmentRecord::State::Local);
}

TEST_CASE("kept: a group switched off mid-way stops its kept collection's downloads") {
    KeptRig rig;
    std::atomic<bool> release{false};
    rig.onFile = [&](const HttpRequest& r) {
        while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.service->keptStatus("SHELF001").has_value(); }));  // the first file under way
    {
        // Its collection is (now) a group's, and the user switches that group off.
        Db db = Db::open(rig.db());
        Library ui(db);
        Library::GroupMeta g;
        g.id = 31;
        g.version = 1;
        g.name = "Lab";
        g.libraryEditing = g.fileEditing = "members";
        ui.upsertGroup(g);
        ui.setGroupAccess(31, true, true);
        db.exec("UPDATE collections SET lib = 31 WHERE key = 'SHELF001'");
        ui.setLibraryEnabled(31, false);
    }
    release = true;
    REQUIRE(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(rig.fetchedCopy().size() == 1);  // the one under way, then no more
}

TEST_CASE("kept: stopping mid-way stops before the next file") {
    KeptRig rig;
    std::atomic<bool> release{false};
    rig.onFile = [&](const HttpRequest& r) {
        while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));
    rig.keep("SHELF001", false);
    release = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetchedCopy().size() == 1);  // the one under way finished; no other began
    CHECK_FALSE(rig.service->keptStatus("SHELF001"));
}

TEST_CASE("kept: without Wi-Fi nothing happens, and Wi-Fi is never asked to come up") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.wifi = false;
    rig.service->downloadKept();
    rig.syncAndWait();  // (the fake server answers even so)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetchedCopy().empty());
    CHECK_FALSE(rig.downloaded("ATTA0001"));
    CHECK_FALSE(rig.service->keptStatus("SHELF001"));
    for (const auto& s : rig.lipc.setsCopy()) CHECK(s.find("wireless") == std::string::npos);
    rig.wifi = true;  // back with the next sync
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
}

TEST_CASE("kept: under 300 MB free the downloads pause, and the row says so") {
    KeptRig rig;
    rig.freeBytes = 299ull << 20;
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.service->keptStatus("SHELF001").has_value(); }));
    CHECK(rig.service->keptStatus("SHELF001")->paused);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(rig.fetchedCopy().empty());
    rig.freeBytes = 2ull << 30;  // room again: the next sync carries on
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    CHECK(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));
}

TEST_CASE("kept: a book the user opens goes first") {
    KeptRig rig;
    std::atomic<bool> holdKept{true};
    rig.onFile = [&](const HttpRequest& r) {
        if (r.url.find("ATTC0001") != std::string::npos) return;  // the opened book: at once
        while (holdKept && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));  // a kept file is under way
    rig.service->openBook("BOOKC001", "ATTC0001");
    std::vector<SyncEvent> events = rig.waitFor([](const SyncEvent& e) {
        return e.kind == SyncEvent::Kind::DownloadFinished && e.attachmentKey == "ATTC0001";
    });
    CHECK_FALSE(events.back().error);
    CHECK(rig.downloaded("ATTC0001"));
    CHECK_FALSE(rig.downloaded("ATTA0001"));
    CHECK_FALSE(rig.downloaded("ATTB0001"));
    holdKept = false;  // then the kept ones carry on
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
}

TEST_CASE("kept: catching up doesn't hold the Kindle awake beyond the sync") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    const size_t sets = rig.lipc.setsCopy().size();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    CHECK(rig.lipc.setsCopy().size() == sets);  // no keep-awake taken for it
}

TEST_CASE("kept: a server or network failure ends the round; the next sync tries again") {
    KeptRig rig;
    rig.answerFile("ATTA0001", 503);
    rig.answerFile("ATTB0001", 503);
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetchedCopy().size() == 1);  // not file after file
    CHECK_FALSE(rig.service->keptStatus("SHELF001"));
    rig.answerFile("ATTA0001", 0);
    rig.answerFile("ATTB0001", 0);
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
}

TEST_CASE("kept: a file missing on the server isn't asked for after every sync; after a week, or a change, it is") {
    KeptRig rig;
    rig.answerFile("ATTA0001", 404);
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTB0001"); }));  // the others carry on
    REQUIRE(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));
    CHECK(rig.fetches("ATTA0001") == 1);
    rig.syncAndWait();
    rig.syncAndWait();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(rig.fetches("ATTA0001") == 1);
    {  // remembered in the library, across restarts
        rig.service.reset();
        rig.startOnZotero();
        rig.syncAndWait();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(rig.fetches("ATTA0001") == 1);
    }
    rig.wallMs += int64_t{8} * 24 * 3600 * 1000;  // a week on
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.fetches("ATTA0001") == 2; }));
    rig.book("BOOKA001", "ATTA0001", "SHELF001");  // changed in Zotero (a new version), and there now
    rig.answerFile("ATTA0001", 0);
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001"); }));
}

TEST_CASE("kept: a file whose size would take the Kindle under 300 MB is cancelled, and the row pauses") {
    KeptRig rig;
    rig.freeBytes = kKeptMinFree + 5;  // above the floor, but not by a whole book
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.service->keptStatus("SHELF001") && rig.service->keptStatus("SHELF001")->paused; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(rig.fetchedCopy().size() == 1);
    CHECK_FALSE(rig.downloaded("ATTA0001"));
    CHECK_FALSE(rig.downloaded("ATTB0001"));
    // The cancel was for that file only: opening a book still works.
    rig.freeBytes = uint64_t{1} << 40;
    rig.service->openBook("BOOKC001", "ATTC0001");
    auto events = rig.waitFor([](const SyncEvent& e) { return e.kind == SyncEvent::Kind::DownloadFinished && e.attachmentKey == "ATTC0001"; });
    CHECK_FALSE(events.back().error);
}

TEST_CASE("kept: an opened book isn't told to wait while only kept files download") {
    KeptRig rig;
    std::atomic<bool> release{false};
    rig.onFile = [&](const HttpRequest& r) {
        if (r.url.find("ATTC0001") != std::string::npos) return;
        while (!release && !(r.cancel && *r.cancel)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.fetchedCopy().size() == 1; }));
    CHECK_FALSE(rig.service->busy());
    release = true;
    rig.service.reset();  // before `release` goes
}

TEST_CASE("kept: every readable file of an item downloads, not only the one Open opens; with its title") {
    KeptRig rig;
    {
        std::lock_guard<std::mutex> lock(rig.serverMutex);
        auto pdf = FakeZotero::attachment("ATTA0002", "BOOKA001", KeptRig::md5("ATTA0002"));
        pdf["contentType"] = "application/pdf";
        pdf["filename"] = "paper.pdf";
        pdf["title"] = "Full Text PDF";
        rig.zotero.put(pdf);
    }
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    rig.service->downloadKept();
    CHECK(rig.waitUntil([&] { return rig.fetches("ATTA0001") && rig.fetches("ATTA0002") && rig.fetches("ATTB0001"); }));
    CHECK(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.attachment("ATTA0001")->state == AttachmentRecord::State::Local);
    CHECK(ui.attachment("ATTA0002")->state == AttachmentRecord::State::Local);
    CHECK(ui.attachment("ATTA0002")->title == "Full Text PDF");
    CHECK(ui.rowFor("BOOKA001")->files == 2);
}

TEST_CASE("kept: a file whose download stalls goes behind the others; it doesn't hold them up, now or after") {
    KeptRig rig;
    rig.book("BOOKD001", "ATTD0001", "SHELF001");
    rig.startOnZotero();
    rig.syncAndWait();
    rig.keep("SHELF001");
    // The file the round asks for first stalls.
    std::string first;
    rig.onFile = [&](const HttpRequest& r) {
        std::lock_guard<std::mutex> lock(rig.fileMutex);
        if (first.empty()) {
            first = r.url.substr(std::string("https://api.zotero.org/users/1/items/").size(), 8);
            rig.stalls.insert(first);
        }
    };
    rig.service->downloadKept();
    std::vector<std::string> others;
    // The round ends with only that one left; the row says so (not nothing, as if all were in).
    REQUIRE(rig.waitUntil([&] {
        auto st = rig.service->keptStatus("SHELF001");
        return st && st->stalled == 1;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (const char* k : {"ATTA0001", "ATTB0001", "ATTD0001"})
        if (k != first) others.push_back(k);
    REQUIRE(others.size() == 2);
    CHECK(rig.downloaded(others[0]));  // the others came in the same round
    CHECK(rig.downloaded(others[1]));
    CHECK_FALSE(rig.downloaded(first));
    CHECK(rig.fetches(first) == 1);  // tried once this round, not over and over
    // The next round (rebuilt from the library, as after a restart) asks for a new file first.
    rig.book("BOOKE001", "ATTE0001", "SHELF001");
    rig.syncAndWait();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTE0001") && rig.fetches(first) == 2; }));
    const auto fetched = rig.fetchedCopy();
    const auto e = std::find(fetched.begin(), fetched.end(), "ATTE0001");
    const auto again = std::find(std::next(std::find(fetched.begin(), fetched.end(), first)), fetched.end(), first);
    CHECK(e < again);
    rig.stall(first, false);  // the line is good again: it comes in
    rig.syncAndWait();
    CHECK(rig.waitUntil([&] { return rig.downloaded(first); }));
}

TEST_CASE("sync service: a book's download says why it waits when Zotero asks it to") {
    KeptRig rig;
    std::atomic<bool> once{true};
    rig.route = [&, base = rig.route](const HttpRequest& r) {
        if (r.url.find("/items/ATTC0001/file") != std::string::npos && once.exchange(false))
            return FakeHttp::status(429, {{"retry-after", "1"}});
        return base(r);
    };
    rig.startOnZotero();
    rig.syncAndWait();
    rig.service->openBook("BOOKC001", "ATTC0001");
    auto events = rig.waitFor([](const SyncEvent& e) { return e.kind == SyncEvent::Kind::DownloadFinished; });
    CHECK_FALSE(events.back().error);
    bool told = false;
    for (const auto& e : events)
        told = told || (e.kind == SyncEvent::Kind::DownloadProgress && e.error &&
                        e.error->message.rfind("Zotero asks to wait until ", 0) == 0);
    CHECK(told);
}

TEST_CASE("kept: files from Zotero storage aren't asked for while Zotero asked for quiet") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    {
        Db db = Db::open(rig.db());
        Library ui(db);
        ui.setMetaValue("zoteroWaitUntil", std::to_string(rig.wallMs + 3600 * 1000));
    }
    rig.keep("SHELF001");
    rig.service->downloadKept();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetchedCopy().empty());
    rig.wallMs += int64_t{3601} * 1000;
    rig.service->downloadKept();
    CHECK(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
}

TEST_CASE("kept: a stalled file no longer kept, or gone, is off the stalled list") {
    KeptRig rig;
    rig.startOnZotero();
    rig.syncAndWait();
    {
        Db db = Db::open(rig.db());
        Library ui(db);
        ui.setMetaValue("keptStalled", R"({"GONE0001": 1, "ATTC0001": 1})");  // gone; not in a kept collection
    }
    rig.keep("SHELF001");
    rig.service->downloadKept();
    REQUIRE(rig.waitUntil([&] { return rig.downloaded("ATTA0001") && rig.downloaded("ATTB0001"); }));
    REQUIRE(rig.waitUntil([&] { return !rig.service->keptStatus("SHELF001"); }));
    Db db = Db::open(rig.db());
    Library ui(db);
    CHECK(ui.metaValue("keptStalled") == "{}");
}
