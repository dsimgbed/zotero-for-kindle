#include <doctest/doctest.h>

#include "app/accounts.h"
#include "app/device_settings.h"
#include "app/migrate_accounts.h"
#include "app/startup.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/config.h"
#include "test_paths.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>

using namespace zk;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kUser = 777;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A single-account install of the version before multiple accounts, in a temp dir: /var/local/zotero
// is <root>/var, /mnt/us/zotero is <root>/us.
struct OldInstall {
    fs::path root;
    Paths p;

    explicit OldInstall(const std::string& stem) : root(test::uniqueTempPath(stem)) {
        fs::remove_all(root);
        fs::create_directories(root);
        const fs::path us = root / "us";
        p = defaultPaths();
        p.configDir = (root / "var").string();
        p.droppedConfig = (us / "config.json").string();
        p.dbPath = (us / "zotero.db").string();
        p.filesDir = (us / "files").string();
        p.logDir = (us / "log").string();
        p.appDir = "";
        p.launcherPath = "";
        p.pluginsDir = (us / "plugins").string();
        p.cacheDir = (us / "cache").string();
        p.stateDir = (us / "state").string();
        p.accountsRoot = (us / "accounts").string();
        p.deviceSettingsFile = (us / "device.json").string();
    }
    ~OldInstall() { fs::remove_all(root); }
    AccountPaths account() const { return accountPaths(p, kUser); }

    // The library is left as a killed app leaves it: its last changes (an unsent annotation, a read
    // mark) only in zotero.db-wal, so a DB moved without its -wal loses them.
    void build(bool withConfig = true, bool withDbUser = true) {
        if (withConfig) {
            Config c;
            c.userId = kUser;
            c.apiKey = "secret-key";
            c.webdavUrl = "https://dav.example/";
            REQUIRE(saveConfig(p.configDir, c));
        }
        const std::string staging = (root / "staging.db").string();
        {
            Db db = Db::open(staging);
            Library lib(db);
            lib.setMetaValue("zotero.userId", withDbUser ? std::to_string(kUser) : "");
            lib.setMetaValue("zotero.userName", "pavel");
            lib.setMetaValue("settings", R"({"flashEvery":3,"homeScreen":"pin-board/home"})");
            lib.setMetaValue("annotationColour", "#ff0000");
            ItemRecord book;
            book.key = "BOOK0001";
            book.version = 1;
            book.itemType = "book";
            book.title = "A book";
            lib.upsertItem(book);
            for (const auto& [key, name, type] : {std::tuple{"EPUB0001", "book.epub", "application/epub+zip"},
                                                  std::tuple{"PDF00001", "paper.pdf", "application/pdf"}}) {
                AttachmentRecord a;
                a.key = key;
                a.parentKey = "BOOK0001";
                a.contentType = type;
                a.filename = name;
                a.md5 = "md5";
                a.linkMode = "imported_file";
                a.state = AttachmentRecord::State::Local;
                a.localPath = (fs::path(p.filesDir) / key / name).string();
                lib.upsertAttachment(a);
            }
        }
        {
            Db db = Db::open(staging);
            Library lib(db);
            AnnotationRecord a;
            a.parentKey = "EPUB0001";
            a.type = "highlight";
            a.text = "unsent words";
            a.color = "#ffd400";
            a.sortIndex = "00001|00000099";
            a.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/2!/4/2/1:0)"})j";
            lib.createAnnotation(a);
            lib.markRead("BOOK0001", true);
            fs::create_directories(fs::path(p.dbPath).parent_path());
            for (const char* s : {"", "-wal", "-shm"}) fs::copy_file(staging + s, p.dbPath + s);
        }
        for (const char* s : {"", "-wal", "-shm"}) fs::remove(staging + s);
        REQUIRE(fs::file_size(p.dbPath + "-wal") > 0);

        writeFile(fs::path(p.filesDir) / "EPUB0001" / "book.epub", "epub bytes");
        writeFile(fs::path(p.filesDir) / "PDF00001" / "paper.pdf", "pdf bytes");
        const fs::path preview = fs::path(p.cacheDir) / "preview";
        writeFile(preview / "P1.pdf", "preview bytes");
        writeFile(preview / "index.json",
                  nlohmann::json::array({{{"key", "P1"}, {"file", (preview / "P1.pdf").string()}, {"title", "Paper"},
                                          {"type", "pdf"}, {"bytes", 13}}})
                      .dump());
        writeFile(fs::path(p.cacheDir) / "cre" / "doc.cache", "cre bytes");
        writeFile(fs::path(p.stateDir) / "home-tree.json", R"({"tree":1})");
        const fs::path pin = fs::path(p.pluginsDir) / "pin-board";
        writeFile(pin / "plugin.json", R"({"id":"pin-board"})");
        writeFile(pin / "storage.db", "storage");
        writeFile(pin / "storage.db-wal", "storage wal");
        writeFile(pin / "data" / "pins.txt", "pins");
        writeFile(fs::path(p.pluginsDir) / "quiet" / "plugin.json", R"({"id":"quiet"})");  // no storage, no data
        writeFile(fs::path(p.pluginsDir) / ".cache" / "fulltext" / "EPUB0001-md5.txt", "full text");
        writeFile(p.droppedConfig, R"({"userId":999,"apiKey":"dropped"})");
    }

    // Every file under the root by its relative path (the root left out of its text), but
    // .leftovers (see leftovers()) and the account's database files: SQLite rewrites those whenever it opens them (the database is
    // checked by its rows instead).
    std::map<std::string, std::string> snapshot() const {
        std::map<std::string, std::string> out;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file()) continue;
            const std::string rel = fs::relative(e.path(), root).string();
            if (rel.rfind("us/accounts/777/zotero.db", 0) == 0 || rel.rfind("us/accounts/.leftovers/", 0) == 0) continue;
            std::string text = readFile(e.path());  // full paths (the preview index) without the temp dir
            for (size_t at; (at = text.find(root.string())) != std::string::npos;) text.replace(at, root.string().size(), "<root>");
            out[rel] = text;
        }
        return out;
    }

    // What a migration put aside in accounts/.leftovers/<time>/, by its path under the time.
    std::map<std::string, std::string> leftovers() const {
        std::map<std::string, std::string> out;
        const fs::path dir = fs::path(p.accountsRoot) / ".leftovers";
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            const fs::path rel = fs::relative(e.path(), dir);
            out[fs::relative(rel, *rel.begin()).string()] = readFile(e.path());
        }
        return out;
    }

    // Everything is where the account layout puts it, and nothing is left behind.
    void checkMigrated() const {
        const AccountPaths a = account();
        for (const std::string& gone : {p.dbPath, p.dbPath + "-wal", p.dbPath + "-shm", p.filesDir, p.cacheDir + "/preview",
                                        p.cacheDir + "/cre", p.stateDir + "/home-tree.json", p.configFile(),
                                        p.pluginsDir + "/pin-board/storage.db", p.pluginsDir + "/pin-board/storage.db-wal",
                                        p.pluginsDir + "/pin-board/data", p.pluginsDir + "/.cache/fulltext"})
            CHECK_MESSAGE(!fs::exists(gone), gone);
        CHECK(readFile(fs::path(a.filesDir) / "EPUB0001" / "book.epub") == "epub bytes");
        CHECK(readFile(fs::path(a.filesDir) / "PDF00001" / "paper.pdf") == "pdf bytes");
        CHECK(readFile(fs::path(a.previewDir) / "P1.pdf") == "preview bytes");
        const auto index = nlohmann::json::parse(readFile(fs::path(a.previewDir) / "index.json"));
        CHECK(index[0]["file"] == (fs::path(a.previewDir) / "P1.pdf").string());  // the preview cache still knows it
        CHECK(readFile(fs::path(a.creDir) / "doc.cache") == "cre bytes");
        CHECK(readFile(fs::path(a.stateDir) / "home-tree.json") == R"({"tree":1})");
        CHECK(readFile(a.pluginStorage("pin-board")) == "storage");
        CHECK(readFile(a.pluginStorage("pin-board") + "-wal") == "storage wal");
        CHECK(readFile(fs::path(a.pluginData("pin-board")) / "pins.txt") == "pins");
        CHECK(readFile(fs::path(a.fullTextDir) / "EPUB0001-md5.txt") == "full text");
        CHECK_FALSE(fs::exists(a.pluginDataRoot + "/quiet"));
        CHECK(fs::exists(p.pluginsDir + "/pin-board/plugin.json"));  // the plugin stays installed for the device
        CHECK(readFile(p.droppedConfig) == R"({"userId":999,"apiKey":"dropped"})");  // left for the import

        auto cred = loadCredentials(p, kUser);
        REQUIRE(cred);
        CHECK(cred->apiKey == "secret-key");
        CHECK(cred->webdavUrl == "https://dav.example/");
        struct stat st {};
        REQUIRE(::stat(p.credentialsFile(kUser).c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0600);
        auto accounts = readAccounts(p.accountsFile());
        REQUIRE(accounts);
        CHECK(accounts->active == kUser);
        REQUIRE(accounts->accounts.size() == 1);
        CHECK(accounts->accounts[0].userId == kUser);
        CHECK(accounts->accounts[0].name == "pavel");
        CHECK_FALSE(accounts->accounts[0].pin);
        CHECK_FALSE(fs::exists(p.accountsRoot + "/.migrating-777"));

        DeviceSettings device(p.deviceSettingsFile);
        CHECK(device.get("flashEvery") == "3");
        CHECK(device.get("annotationColour") == "#ff0000");

        Db db = Db::open(a.dbPath);
        Library lib(db);
        CHECK(lib.userId() == kUser);
        const auto unsent = lib.dirtyAnnotations();
        REQUIRE(unsent.size() == 1);
        CHECK(unsent[0].text == "unsent words");
        const auto marks = lib.readMarks();
        REQUIRE(marks.size() == 1);
        CHECK(marks[0].itemKey == "BOOK0001");
        CHECK(marks[0].read);
        CHECK(lib.attachment("EPUB0001")->localPath == (fs::path(a.filesDir) / "EPUB0001" / "book.epub").string());
        CHECK(lib.attachment("PDF00001")->localPath == (fs::path(a.filesDir) / "PDF00001" / "paper.pdf").string());
        const auto settings = nlohmann::json::parse(lib.metaValue("settings"));
        CHECK(settings["homeScreen"] == "pin-board/home");
        CHECK_FALSE(settings.contains("flashEvery"));
        CHECK(lib.metaValue("annotationColour").empty());
    }
};

// The state an uninterrupted migration of the fixture ends in.
std::map<std::string, std::string> referenceState() {
    OldInstall ref("zk-migrate-ref");
    ref.build();
    std::string why;
    REQUIRE(migrateToAccounts(ref.p, &why) == MigrateResult::Migrated);
    return ref.snapshot();
}

}  // namespace

TEST_CASE("migrate accounts: a single-account install moves into accounts/<userId>, unsent changes intact") {
    OldInstall old("zk-migrate-full");
    old.build();
    std::string why;
    REQUIRE(migrateToAccounts(old.p, &why) == MigrateResult::Migrated);
    CHECK(why.empty());
    old.checkMigrated();
}

TEST_CASE("migrate accounts: interrupted after any step, the next launch ends in the same state") {
    const auto reference = referenceState();
    size_t count = 0;
    {
        OldInstall probe("zk-migrate-count");
        probe.build();
        count = migrationSteps(probe.p, kUser).size();
    }
    REQUIRE(count > 10);
    for (size_t k = 0; k <= count; ++k) {
        CAPTURE(k);
        OldInstall old("zk-migrate-k");
        old.build();
        auto steps = migrationSteps(old.p, kUser);
        REQUIRE(steps.size() == count);
        std::string why;
        for (size_t i = 0; i < k; ++i) REQUIRE_MESSAGE(steps[i].run(&why), steps[i].name, ": ", why);
        if (k > 0 && k < count) CHECK(fs::exists(old.p.accountsRoot + "/.migrating-777"));  // what the next launch finds
        const MigrateResult r = migrateToAccounts(old.p, &why);
        CHECK(r == (k == count ? MigrateResult::NotNeeded : MigrateResult::Migrated));
        old.checkMigrated();
        CHECK(old.snapshot() == reference);
        // And once more: nothing moves twice.
        CHECK(migrateToAccounts(old.p, &why) == MigrateResult::NotNeeded);
        CHECK(old.snapshot() == reference);
    }
}

TEST_CASE("migrate accounts: a folder cut short mid-move is finished entry by entry; what is in both places is put aside, never deleted") {
    const auto reference = referenceState();
    OldInstall old("zk-migrate-half");
    old.build();
    const AccountPaths a = old.account();
    auto steps = migrationSteps(old.p, kUser);
    REQUIRE_FALSE(steps.empty());
    std::string why;
    REQUIRE(steps[0].run(&why));  // the marker
    // A power cut while files/ and the full-text cache moved: some entries are there, some not.
    fs::create_directories(a.filesDir);
    fs::rename(fs::path(old.p.filesDir) / "EPUB0001", fs::path(a.filesDir) / "EPUB0001");
    // On FAT a rename cut short can leave one file under two names (both naming the same clusters):
    // deleting either would free the other's data.
    writeFile(fs::path(a.fullTextDir) / "EPUB0001-md5.txt", "full text");
    writeFile(fs::path(a.stateDir) / "home-tree.json", R"({"tree":1})");
    CHECK(migrateToAccounts(old.p, &why) == MigrateResult::Migrated);
    old.checkMigrated();
    CHECK(old.snapshot() == reference);
    const std::map<std::string, std::string> aside{{"cache/fulltext/EPUB0001-md5.txt", "full text"},
                                                   {"state/home-tree.json", R"({"tree":1})"}};
    CHECK(old.leftovers() == aside);
}

TEST_CASE("migrate accounts: a file different in both places keeps the target; the old one goes to .leftovers") {
    OldInstall old("zk-migrate-differs");
    old.build();
    const AccountPaths a = old.account();
    auto steps = migrationSteps(old.p, kUser);
    REQUIRE_FALSE(steps.empty());
    std::string why;
    REQUIRE(steps[0].run(&why));  // the marker
    writeFile(fs::path(a.stateDir) / "home-tree.json", R"({"tree":2})");
    CHECK(migrateToAccounts(old.p, &why) == MigrateResult::Migrated);
    CHECK(readFile(fs::path(a.stateDir) / "home-tree.json") == R"({"tree":2})");
    CHECK(old.leftovers() == std::map<std::string, std::string>{{"state/home-tree.json", R"({"tree":1})"}});
    CHECK_FALSE(fs::exists(old.p.stateDir + "/home-tree.json"));
}

TEST_CASE("migrate accounts: a folder that is the target under a second name is put aside whole, not emptied") {
    // Two names for one folder, as a FAT rename cut short leaves files/ (stood for by a link).
    OldInstall old("zk-migrate-samedir");
    old.build();
    const AccountPaths a = old.account();
    auto steps = migrationSteps(old.p, kUser);
    std::string why;
    REQUIRE(steps[0].run(&why));
    fs::create_directories(fs::path(a.filesDir).parent_path());
    fs::rename(old.p.filesDir, a.filesDir);
    fs::create_directory_symlink(a.filesDir, old.p.filesDir);
    CHECK(migrateToAccounts(old.p, &why) == MigrateResult::Migrated);
    CHECK(readFile(fs::path(a.filesDir) / "EPUB0001" / "book.epub") == "epub bytes");
    CHECK(readFile(fs::path(a.filesDir) / "PDF00001" / "paper.pdf") == "pdf bytes");
    CHECK_FALSE(fs::exists(fs::symlink_status(old.p.filesDir)));
    old.checkMigrated();
}

TEST_CASE("migrate accounts: the moves reach the disk before the credentials, accounts.json and the marker's removal") {
    OldInstall old("zk-migrate-flush");
    old.build();
    struct Seen {
        bool oldDb, credentials, accounts, marker;
    };
    std::vector<Seen> flushes;
    auto steps = migrationSteps(old.p, kUser, [&] {
        flushes.push_back({fs::exists(old.p.dbPath), fs::exists(old.p.credentialsFile(kUser)),
                           fs::exists(old.p.accountsFile()), fs::exists(old.p.accountsRoot + "/.migrating-777")});
    });
    std::string why;
    for (auto& step : steps) REQUIRE_MESSAGE(step.run(&why), step.name, ": ", why);
    REQUIRE(flushes.size() == 2);
    // First: every move done, nothing committed yet.
    CHECK_FALSE(flushes[0].oldDb);
    CHECK_FALSE(flushes[0].credentials);
    CHECK_FALSE(flushes[0].accounts);
    CHECK(flushes[0].marker);
    // Then: committed, the marker still there until both are on disk.
    CHECK(flushes[1].credentials);
    CHECK(flushes[1].accounts);
    CHECK(flushes[1].marker);
    CHECK_FALSE(fs::exists(old.p.accountsRoot + "/.migrating-777"));
    old.checkMigrated();
}

TEST_CASE("migrate accounts: the userID comes from the database when there is no config") {
    OldInstall old("zk-migrate-dbuser");
    old.build(/*withConfig=*/false);
    std::string why;
    REQUIRE(migrateToAccounts(old.p, &why) == MigrateResult::Migrated);
    const AccountPaths a = old.account();
    CHECK(fs::exists(a.dbPath));
    CHECK_FALSE(fs::exists(old.p.dbPath));
    CHECK_FALSE(loadCredentials(old.p, kUser));  // nothing to move: Setup asks for the key
    auto accounts = readAccounts(old.p.accountsFile());
    REQUIRE(accounts);
    CHECK(accounts->active == kUser);
    // Its key is asked for later (Update key); the start's repair keeps the library listed meanwhile.
    repairAccounts(old.p, *accounts);
    REQUIRE(accounts->accounts.size() == 1);
    CHECK(accounts->accounts[0].userId == kUser);
    CHECK(accounts->active == kUser);
    Db db = Db::open(a.dbPath);
    Library lib(db);
    CHECK(lib.dirtyAnnotations().size() == 1);
}

TEST_CASE("migrate accounts: no userID anywhere is no account, and nothing moves") {
    OldInstall old("zk-migrate-nouser");
    old.build(/*withConfig=*/false, /*withDbUser=*/false);
    const auto before = old.snapshot();
    std::string why;
    CHECK(migrateToAccounts(old.p, &why) == MigrateResult::NoAccount);
    CHECK_FALSE(fs::exists(old.p.accountsRoot));
    CHECK_FALSE(fs::exists(old.p.accountsFile()));
    auto after = old.snapshot();
    // SQLite's shared-memory index may be rewritten by a reader; every other byte is as it was.
    after.erase("us/zotero.db-shm");
    auto expected = before;
    expected.erase("us/zotero.db-shm");
    CHECK(after == expected);
}

TEST_CASE("migrate accounts: already migrated, or a fresh install, is not needed") {
    {
        OldInstall fresh("zk-migrate-fresh");
        std::string why;
        CHECK(migrateToAccounts(fresh.p, &why) == MigrateResult::NotNeeded);
        CHECK_FALSE(fs::exists(fresh.p.accountsRoot));
    }
    OldInstall old("zk-migrate-again");
    old.build();
    std::string why;
    REQUIRE(migrateToAccounts(old.p, &why) == MigrateResult::Migrated);
    const auto after = old.snapshot();
    CHECK(migrateToAccounts(old.p, &why) == MigrateResult::NotNeeded);
    CHECK(old.snapshot() == after);
}

TEST_CASE("startup: the account layout is prepared first; the app then opens the active account's files") {
    OldInstall old("zk-migrate-startup");
    old.build();
    Paths p = old.p;
    REQUIRE(openAccountLayout(p) == kUser);
    const AccountPaths a = old.account();
    CHECK(p.dbPath == a.dbPath);
    CHECK(p.filesDir == a.filesDir);
    CHECK(p.cacheDir + "/preview" == a.previewDir);
    CHECK(p.stateDir == a.stateDir);
    old.checkMigrated();
    const std::string dropped = readFile(p.droppedConfig);
    fs::remove(p.droppedConfig);
    const Startup s = startupConfig(p);  // the credentials moved with the account
    REQUIRE(s.config);
    CHECK(s.config->userId == kUser);
    CHECK(s.config->apiKey == "secret-key");
    CHECK_FALSE(s.imported);
    // A config.json dropped over USB is imported after the migration (Setup confirms it), never
    // taken for the migrated account's.
    writeFile(p.droppedConfig, dropped);
    const Startup d = startupConfig(p);
    REQUIRE(d.config);
    CHECK(d.imported);
    CHECK(d.config->userId == 999);
    CHECK(loadCredentials(p, kUser)->apiKey == "secret-key");
}

TEST_CASE("startup: no account keeps the old places; a migration that failed opens nothing") {
    {
        OldInstall old("zk-migrate-startup-none");
        old.build(/*withConfig=*/false, /*withDbUser=*/false);
        Paths p = old.p;
        CHECK(openAccountLayout(p) == 0);
        CHECK(p.dbPath == old.p.dbPath);
        CHECK(p.filesDir == old.p.filesDir);
    }
    OldInstall old("zk-migrate-startup-fail");
    old.build();
    // The account's folder can't be made: a file is in its way.
    writeFile(old.p.accountsRoot, "in the way");
    Paths p = old.p;
    CHECK_FALSE(openAccountLayout(p));
    CHECK(fs::exists(old.p.dbPath));  // nothing moved
}
