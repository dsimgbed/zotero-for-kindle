#include "app/migrate_accounts.h"

#include "app/accounts.h"
#include "app/device_settings.h"
#include "app/settings_store.h"
#include "core/log.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/config.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace zk {

namespace fs = std::filesystem;

namespace {

const char* const kMarkerPrefix = ".migrating-";
const char* const kDbSidecars[] = {"", "-wal", "-shm", "-journal"};

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}

bool fail(std::string* why, const std::string& text) {
    if (why) *why = text;
    log(LogLevel::Error, "migration: %s", text.c_str());
    return false;
}

bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(fs::status(p, ec));
}

// Puts `src` at `aside` (made unique), in .leftovers: what a cut-short move left in both places.
// Renamed, never deleted: on FAT a rename cut short leaves two entries naming the same clusters,
// and deleting either frees the other's data.
bool putAside(const fs::path& src, const fs::path& dst, fs::path aside, std::string* why) {
    std::error_code ec;
    const fs::path base = aside;
    for (int n = 2; present(aside); ++n) {
        aside = base;
        aside += "-" + std::to_string(n);
    }
    fs::create_directories(aside.parent_path(), ec);
    log(LogLevel::Warn, "migration: %s is also at %s; the old one put aside as %s", src.c_str(), dst.c_str(), aside.c_str());
    fs::rename(src, aside, ec);
    return !ec || fail(why, "can't put " + src.string() + " aside as " + aside.string() + ": " + ec.message());
}

// Whether two folder names are one folder (a FAT rename cut short): a probe made in one shows in
// the other. Not knowing counts as the same (the folder is then put aside whole, nothing emptied).
bool sameFolder(const fs::path& a, const fs::path& b) {
    const fs::path probe = a / ".zk-migration-probe";
    std::error_code ec;
    {
        std::ofstream out(probe);
        if (!out) return true;
    }
    const bool same = present(b / ".zk-migration-probe");
    fs::remove(probe, ec);  // our own empty file
    return same;
}

// Moves `src` to `dst` so that running it again, after an interruption anywhere, ends the same:
// only the source: renamed; only the target: done; neither: nothing to do. Both: two folders are
// merged entry by entry (the emptied source folder goes); anything else in both places — and a
// folder that is the target under a second name — keeps the target and puts the source aside at
// `aside`. Nothing is ever deleted.
bool moveEntry(const fs::path& src, const fs::path& dst, const fs::path& aside, std::string* why) {
    std::error_code ec;
    if (!present(src)) return true;
    if (!present(dst)) {
        fs::create_directories(dst.parent_path(), ec);
        fs::rename(src, dst, ec);
        return !ec || fail(why, "can't move " + src.string() + " to " + dst.string() + ": " + ec.message());
    }
    if (!isDir(src) || !isDir(dst) || sameFolder(src, dst)) return putAside(src, dst, aside, why);
    std::vector<fs::path> children;
    for (const auto& e : fs::directory_iterator(src, ec)) children.push_back(e.path());
    if (ec) return fail(why, "can't list " + src.string() + ": " + ec.message());
    for (const auto& child : children)
        if (!moveEntry(child, dst / child.filename(), aside / child.filename(), why)) return false;
    fs::remove(src, ec);  // empty now, and not the target (sameFolder)
    return !ec || fail(why, "can't remove the emptied " + src.string() + ": " + ec.message());
}

// A meta value of an existing database, read without changing it (no create, no checkpoint).
std::string readMeta(const std::string& dbPath, const char* key) {
    if (!present(dbPath)) return {};
    std::string value;
    for (int flags : {SQLITE_OPEN_READONLY, SQLITE_OPEN_READWRITE}) {  // read-write: a WAL a reader can't open
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dbPath.c_str(), &db, flags, nullptr) == SQLITE_OK) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = ?", -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
                const int rc = sqlite3_step(st);
                if (rc == SQLITE_ROW) {
                    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                    value = text ? text : "";
                }
                sqlite3_finalize(st);
                sqlite3_close(db);
                if (rc == SQLITE_ROW || rc == SQLITE_DONE) return value;
                continue;
            }
        }
        sqlite3_close(db);
    }
    return value;
}

int64_t idFrom(const std::string& text) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
    try {
        return std::stoll(text);
    } catch (...) {
        return 0;
    }
}

// The userId of an interrupted migration (its marker), or 0.
int64_t markedUser(const Paths& p) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.accountsRoot, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(kMarkerPrefix, 0) == 0)
            if (int64_t id = idFrom(name.substr(std::strlen(kMarkerPrefix)))) return id;
    }
    return 0;
}

bool syncPath(const std::string& path, int flags) {
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

// The marker reaches the disk before anything moves: without it, a DB moved before a power cut
// (and no config.json to name the user) would leave the next launch looking at an empty install.
bool writeMarker(const Paths& p, int64_t userId, std::string* why) {
    const std::string marker = p.accountsRoot + "/" + kMarkerPrefix + std::to_string(userId);
    if (present(marker)) return true;
    std::error_code ec;
    fs::create_directories(p.accountsRoot, ec);
    const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail(why, "can't write " + marker + ": " + std::strerror(errno));
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || !syncPath(p.accountsRoot, O_RDONLY)) return fail(why, "can't sync " + marker + ": " + std::strerror(errno));
    return true;
}

// attachments.localPath names files under the old files/: point it at the account's.
bool repointLocalPaths(const std::string& dbPath, const std::string& oldFiles, const std::string& newFiles,
                       std::string* why) {
    if (!present(dbPath)) return true;
    try {
        Db db = Db::open(dbPath);
        Stmt table = db.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'attachments'");
        if (!table.step()) return true;
        const std::string from = oldFiles + "/", to = newFiles + "/";
        Stmt st = db.prepare("UPDATE attachments SET localPath = ? || substr(localPath, ?) WHERE substr(localPath, 1, ?) = ?");
        st.bind(1, to).bind(2, static_cast<int64_t>(from.size() + 1)).bind(3, static_cast<int64_t>(from.size())).bind(4, from);
        st.step();
        return true;
    } catch (const DbError& e) {
        return fail(why, std::string("can't update the files' paths in the library: ") + e.what());
    }
}

// The preview cache's index names its files by full path: point them at the account's folder.
bool repointPreviewIndex(const std::string& oldDir, const std::string& newDir, std::string* why) {
    const fs::path index = fs::path(newDir) / "index.json";
    std::ifstream in(index);
    if (!in) return true;
    auto j = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (!j.is_array()) return true;  // unreadable: the cache drops it as it would anyway
    bool changed = false;
    for (auto& e : j)
        if (e.is_object() && e.contains("file") && e["file"].is_string()) {
            const fs::path file = e["file"].get<std::string>();
            if (file.parent_path() == fs::path(oldDir)) {
                e["file"] = (fs::path(newDir) / file.filename()).string();
                changed = true;
            }
        }
    if (!changed) return true;
    const fs::path tmp = fs::path(newDir) / "index.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump();
        if (!out) return fail(why, "can't write " + tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, index, ec);
    return !ec || fail(why, "can't replace " + index.string() + ": " + ec.message());
}

bool moveDeviceKeysOf(const Paths& p, const std::string& dbPath, std::string* why) {
    if (!present(dbPath)) return true;
    try {
        Db db = Db::open(dbPath);
        Library lib(db);
        DeviceSettings device(p.deviceSettingsFile);
        moveDeviceKeys(lib, device);  // device.json not writable: they stay in the library (logged by it)
        return true;
    } catch (const std::exception& e) {
        return fail(why, std::string("can't read the library for its device settings: ") + e.what());
    }
}

// config.json -> accounts/<userId>.json (0600): written first, the old file removed after.
bool moveCredentials(const Paths& p, int64_t userId, std::string* why) {
    if (!present(p.configFile())) return true;
    auto c = loadConfig(p.configDir);
    if (!c || c->userId != userId) {
        log(LogLevel::Warn, "migration: config.json isn't this account's (or is incomplete); left in place");
        return true;
    }
    if (!saveCredentials(p, *c)) return fail(why, "can't save the account's credentials");
    std::error_code ec;
    fs::remove(p.configFile(), ec);
    return !ec || fail(why, "can't remove the old config.json: " + ec.message());
}

bool listAccount(const Paths& p, int64_t userId, const std::string& dbPath, std::string* why) {
    // A listed account has its folder (startup's repair drops one without): an install with
    // nothing to move (a first Setup's config.json only) gets it here.
    std::error_code ec;
    fs::create_directories(accountPaths(p, userId).root, ec);
    if (ec) return fail(why, "can't make " + accountPaths(p, userId).root + ": " + ec.message());
    AccountsState s = readAccounts(p.accountsFile()).value_or(AccountsState{});
    const bool listed = std::any_of(s.accounts.begin(), s.accounts.end(), [&](const AccountEntry& e) { return e.userId == userId; });
    if (listed && s.active == userId) return true;
    if (!listed) {
        AccountEntry e;
        e.userId = userId;
        e.name = readMeta(dbPath, "zotero.userName");
        e.order = static_cast<int>(s.accounts.size());
        s.accounts.push_back(std::move(e));
    }
    s.active = userId;
    return writeAccounts(p.accountsFile(), s) || fail(why, "can't write " + p.accountsFile());
}

}  // namespace

std::vector<MigrationStep> migrationSteps(const Paths& p, int64_t userId, std::function<void()> flush) {
    const AccountPaths a = accountPaths(p, userId);
    if (!flush) flush = [] { ::sync(); };
    // What a cut-short move left in both places: <accountsRoot>/.leftovers/<time>/<path in the account>.
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&now, &tm);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    const fs::path leftovers = fs::path(p.accountsRoot) / ".leftovers" / stamp;
    std::vector<MigrationStep> steps;
    auto move = [&](std::string name, fs::path src, fs::path dst) {
        const fs::path aside = leftovers / fs::path(dst).lexically_relative(a.root);
        steps.push_back({"move " + name, [src, dst, aside](std::string* why) { return moveEntry(src, dst, aside, why); }});
    };
    auto flushStep = [&](std::string name) {
        steps.push_back({std::move(name), [flush](std::string*) {
                             flush();
                             return true;
                         }});
    };
    steps.push_back({"write the marker", [p, userId](std::string* why) { return writeMarker(p, userId, why); }});
    for (const char* s : kDbSidecars) move(std::string("zotero.db") + s, p.dbPath + s, a.dbPath + s);
    move("files", p.filesDir, a.filesDir);
    steps.push_back({"repoint the files' paths", [p, a](std::string* why) {
                         return repointLocalPaths(a.dbPath, p.filesDir, a.filesDir, why);
                     }});
    const std::string oldPreview = p.cacheDir + "/preview";
    move("cache/preview", oldPreview, a.previewDir);
    steps.push_back({"repoint the preview index", [oldPreview, a](std::string* why) {
                         return repointPreviewIndex(oldPreview, a.previewDir, why);
                     }});
    move("cache/cre", p.cacheDir + "/cre", a.creDir);
    move("the full-text cache", fs::path(p.pluginsDir) / ".cache" / "fulltext", a.fullTextDir);
    move("state/home-tree.json", fs::path(p.stateDir) / "home-tree.json", fs::path(a.stateDir) / "home-tree.json");
    // Each plugin's storage (and its SQLite sidecars), data/ and any full-text cache of its own.
    // The plugin itself (plugin.json, main.js, settings.json) stays: plugins belong to the device.
    std::vector<std::string> plugins;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.pluginsDir, ec)) {
        const std::string id = e.path().filename().string();
        if (!id.empty() && id[0] != '.' && e.is_directory(ec)) plugins.push_back(id);
    }
    std::sort(plugins.begin(), plugins.end());
    for (const std::string& id : plugins) {
        const fs::path dir = fs::path(p.pluginsDir) / id;
        for (const char* s : kDbSidecars) move(id + "/storage.db" + s, dir / ("storage.db" + std::string(s)), a.pluginStorage(id) + s);
        move(id + "/data", dir / "data", a.pluginData(id));
        move(id + "/.cache/fulltext", dir / ".cache" / "fulltext", a.fullTextDir);
    }
    steps.push_back({"move the device settings", [p, a](std::string* why) { return moveDeviceKeysOf(p, a.dbPath, why); }});
    // /mnt/us is FAT: its renames reach the disk in no set order. Nothing may say "done" (config.json
    // gone, accounts.json written, the marker removed) before they are there.
    flushStep("flush the moves");
    steps.push_back({"move the credentials", [p, userId](std::string* why) { return moveCredentials(p, userId, why); }});
    steps.push_back({"write accounts.json", [p, userId, a](std::string* why) { return listAccount(p, userId, a.dbPath, why); }});
    flushStep("flush before the marker goes");
    steps.push_back({"remove the marker", [p, userId](std::string* why) {
                         std::error_code ec;
                         fs::remove(p.accountsRoot + "/" + kMarkerPrefix + std::to_string(userId), ec);
                         return !ec || fail(why, "can't remove the marker: " + ec.message());
                     }});
    return steps;
}

std::string readLibraryMeta(const std::string& dbPath, const char* key) { return readMeta(dbPath, key); }

MigrateResult migrateToAccounts(const Paths& p, std::string* why) {
    int64_t userId = markedUser(p);
    if (userId) {
        log(LogLevel::Info, "migration: resuming for account %lld", static_cast<long long>(userId));
    } else {
        if (present(p.accountsFile())) return MigrateResult::NotNeeded;
        if (!present(p.configFile()) && !present(p.dbPath)) return MigrateResult::NotNeeded;  // a new install
        if (auto c = loadConfig(p.configDir)) userId = c->userId;
        if (!userId) userId = idFrom(readMeta(p.dbPath, "zotero.userId"));
        if (!userId) {
            log(LogLevel::Warn, "migration: no userID in config.json or the library; the old files stay (Setup)");
            return MigrateResult::NoAccount;
        }
        log(LogLevel::Info, "migration: moving the install into the layout of account %lld", static_cast<long long>(userId));
    }
    for (const MigrationStep& step : migrationSteps(p, userId)) {
        std::string reason;
        if (!step.run(&reason)) {
            if (why) *why = step.name + ": " + reason;
            return MigrateResult::Failed;
        }
    }
    log(LogLevel::Info, "migration: done");
    return MigrateResult::Migrated;
}

}  // namespace zk
