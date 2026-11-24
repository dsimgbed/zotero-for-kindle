// zk-sync: the storage and sync layer without a UI, for real runs on the Mac and the Kindle.
//   zk-sync [--account USERID] [--config DIR | --env FILE] [--db PATH] [--ca FILE] COMMAND
//   commands: pull | status | groups | search <words> | collections | annotations <attachmentKey>
//             | fetch <attachmentKey> <dir> | highlights-offline on|off
//             | push | annotate-test <attachmentKey> [page] | annotate-test-edit <key>
//             | position-test <attachmentKey>
//             | annotate-test-cleanup
//   `annotations` downloads that book's annotations first, as opening a book in the app will.
//   `annotate-test` highlights the first text of a page (3 by default) of a fetched file, as the
//   reader would, tagged zk-test with a "[zk-test]" comment; `annotate-test-cleanup` deletes
//   those (and only those); `annotate-test-edit` makes one of them blue with an "edited" comment.
//   They change the local library; `push` sends the changes.
// `pull` and `push` go through every library: the user's groups are listed first, then My Library
// and each enabled group are synced in turn; `groups` lists them.
// Every request goes through the WriteGuard, which allows annotation writes, the reading positions
// of the library's own files, the read tag of its items and pending plugin changes in their exact
// shape, and nothing else.
// The account: --account USERID, else the active one — its key (in DIR/accounts/) and its library
// (accounts/<userId>/zotero.db). Before the account layout: DIR/config.json and the old library.
// --env and --db override the key and the library. A config.json dropped over USB is the app's to
// take in, not this tool's.
// Credentials are never printed.

#include "docs/fonts.h"
#include "core/log.h"
#include "docs/engine_factory.h"
#include "net/http.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "app/accounts.h"
#include "app/zk_sync_json.h"
#include "sync/config.h"
#include "sync/libraries.h"
#include "sync/sync_engine.h"
#include "sync/webdav.h"
#include "sync/zotero_storage.h"
#include "zotero/api.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace zk;

namespace {

#ifdef ZK_KINDLE
std::string kindleHome() {
    const char* v = std::getenv("ZK_HOME");
    std::string s = (v && *v) ? std::string(v) : "/mnt/us/zotero";
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
const std::string kDefaultCaStr = kindleHome() + "/share/cacert.pem";
const std::string kCrengineDataStr = kindleHome() + "/share/crengine";
const char* kDefaultCa = kDefaultCaStr.c_str();
const char* kCrengineData = kCrengineDataStr.c_str();
#else
const char* kDefaultCa = "";
const char* kCrengineData = "third_party/crengine/cr3gui/data";
#endif

constexpr const char* kTestTag = "zk-test";
constexpr const char* kTestComment = "[zk-test] made by zk-sync annotate-test";

int usage() {
    std::fprintf(stderr,
                 "usage: zk-sync [--account USERID] [--config DIR | --env FILE] [--db PATH] [--files-dir DIR] [--ca FILE] COMMAND\n"
                 "  pull | status | groups | search <words> | collections | annotations <attachmentKey>\n"
                 "  | fetch <attachmentKey> <dir> | highlights-offline on|off\n"
                 "  | push | annotate-test <attachmentKey> [page] | annotate-test-edit <key>\n"
                 "  | annotate-test-cleanup\n"
                 "  | export-index <outPath>\n"
                 "  | pdf-info <attachmentKey> [--fetch]\n"
                 "  | ink-pull <attachmentKey> [--local-only]\n"
                 "  | ink-upsert <attachmentKey> <inputJsonPath>\n");
    return 2;
}

void printError(const UserError& e) {
    std::printf("error: %s\n  details: %s\n", e.message.c_str(), e.details.c_str());
}

std::string scrubSecrets(std::string s, const std::optional<Config>& cfg) {
    if (!cfg) return s;
    auto scrub = [&](const std::string& secret) {
        if (secret.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(secret, pos)) != std::string::npos) {
            s.replace(pos, secret.size(), "[REDACTED]");
            pos += 10;
        }
    };
    scrub(cfg->apiKey);
    scrub(cfg->webdavPassword);
    scrub(cfg->webdavUser);
    scrub(cfg->webdavUrl);
    return s;
}

void printJsonError(const std::string& msg, const std::optional<Config>& cfg) {
    const std::string clean = scrubSecrets(msg, cfg);
    std::fprintf(stderr, "error: %s\n", clean.c_str());
}

void writeJsonStdout(const nlohmann::json& j) {
    const std::string s = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

long peakRssKb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
}

std::string creatorLine(const ItemRecord& r) {
    std::string s;
    for (const auto& c : r.creators) {
        if (!s.empty()) {
            s += " et al.";
            break;
        }
        s = !c.last.empty() ? c.last : c.name;
    }
    return s;
}

void printItems(const std::vector<ItemRecord>& items) {
    for (const auto& r : items)
        std::printf("%s  %-14s %s — %s %s\n", r.key.c_str(), r.itemType.c_str(), r.title.empty() ? "(untitled)" : r.title.c_str(),
                    creatorLine(r).c_str(), r.date.c_str());
}

void printCollections(const std::vector<CollectionRecord>& all, const std::string& parent, int depth) {
    for (const auto& c : all)
        if (c.parentKey == parent) {
            std::printf("%*s%s  %s\n", depth * 2, "", c.key.c_str(), c.name.c_str());
            printCollections(all, c.key, depth + 1);
        }
}

}  // namespace

int main(int argc, char** argv) {
    Paths paths = defaultPaths();
    std::string envFile, dbFlagPath, filesDirFlag, ca = kDefaultCa;
    int64_t account = 0;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string(argv[++i]);
        };
        if (a == "--config") {
            auto v = next();
            if (!v) return usage();
            paths.configDir = *v;
        } else if (a == "--env") {
            auto v = next();
            if (!v) return usage();
            envFile = *v;
        } else if (a == "--db") {
            auto v = next();
            if (!v) return usage();
            dbFlagPath = *v;
        } else if (a == "--files-dir") {
            auto v = next();
            if (!v) return usage();
            filesDirFlag = *v;
        } else if (a == "--ca") {
            auto v = next();
            if (!v) return usage();
            ca = *v;
        } else if (a == "--account") {
            auto v = next();
            if (!v) return usage();
            account = std::atoll(v->c_str());
            if (account <= 0) return usage();
        } else {
            args.push_back(a);
        }
    }
    if (args.empty()) return usage();
    const std::string cmd = args[0];

    const bool isJsonCmd = (cmd == "export-index" || cmd == "pdf-info" || cmd == "ink-pull" || cmd == "ink-upsert");
    bool pdfInfoFetch = false;
    bool inkPullLocalOnly = false;
    if (cmd == "export-index") {
        if (args.size() != 2 || args[1].empty()) return usage();
    } else if (cmd == "pdf-info") {
        if (args.size() == 2 && !args[1].empty()) {
            pdfInfoFetch = false;
        } else if (args.size() == 3 && !args[1].empty() && args[2] == "--fetch") {
            pdfInfoFetch = true;
        } else {
            return usage();
        }
    } else if (cmd == "ink-pull") {
        if (args.size() == 2 && !args[1].empty()) {
            inkPullLocalOnly = false;
        } else if (args.size() == 3 && !args[1].empty() && args[2] == "--local-only") {
            inkPullLocalOnly = true;
        } else {
            return usage();
        }
    } else if (cmd == "ink-upsert") {
        if (args.size() != 3 || args[1].empty() || args[2].empty()) return usage();
    }

    std::string dbPath = dbFlagPath;
    // Without --env or --db, the account's own key and library; nothing of another's.
    AccountTarget target;
    if (envFile.empty() || dbPath.empty()) target = accountTarget(paths, account);
    std::optional<Config> cfg = envFile.empty() ? target.config : configFromEnvFile(envFile);
    if (dbPath.empty()) dbPath = target.dbPath;
    const bool hasCreds = cfg && cfg->userId > 0 && !cfg->apiKey.empty();

    const bool allowNoCreds = (cmd == "export-index") ||
                              (cmd == "pdf-info") ||  // --fetch only requires creds if PDF is not already cached
                              (cmd == "ink-upsert") ||
                              (cmd == "ink-pull" && (inkPullLocalOnly || !dbFlagPath.empty()));

    if (dbPath.empty()) {
        if (isJsonCmd) {
            printJsonError(!target.error.empty() ? target.error : "no library database configured", cfg);
        } else {
            std::printf("error: No Zotero account configured.\n  details: %s\n",
                        !target.error.empty() ? target.error.c_str() : "no library");
        }
        return 1;
    }
    if (!allowNoCreds && !hasCreds) {
        const std::string detail = !envFile.empty() && !cfg ? ("no key in " + envFile)
                                   : !target.error.empty()  ? (target.error + (envFile.empty() ? "; or pass --env FILE" : ""))
                                                            : "no key or library";
        if (isJsonCmd) {
            printJsonError("No Zotero account configured: " + detail, cfg);
        } else {
            std::printf("error: No Zotero account configured.\n  details: %s\n", detail.c_str());
        }
        return 1;
    }
    if (!envFile.empty() && target.userId > 0 && cfg && cfg->userId != target.userId) {  // never one account's key on another's library
        if (isJsonCmd) {
            printJsonError("credentials user does not match selected account library", cfg);
        } else {
            std::printf("error: %s is user %lld's key, not account %lld's.\n  details: pass --account %lld, or --db\n",
                        envFile.c_str(), static_cast<long long>(cfg->userId), static_cast<long long>(target.userId),
                        static_cast<long long>(cfg->userId));
        }
        return 1;
    }
    // crengine's cache and filesDir: the account's own (the app's place for it), else the legacy paths.
    const bool inAccount = target.userId > 0 && dbPath == accountPaths(paths, target.userId).dbPath;
    const std::string filesDir = !filesDirFlag.empty() ? filesDirFlag
                                 : inAccount           ? accountPaths(paths, target.userId).filesDir
                                                       : paths.filesDir;
    const EngineEnv kEngineEnv{kCrengineData, readingFontDirs(),
                               inAccount ? accountPaths(paths, target.userId).creDir : paths.cacheDir + "/cre"};

    std::optional<Db> db;
    try {
        db.emplace(Db::open(dbPath));
    } catch (const DbError& e) {
        if (isJsonCmd) printJsonError(std::string("Can't open the library database: ") + e.what(), cfg);
        else printError({ErrorKind::Other, "Can't open the library database.", e.what()});
        return 1;
    }
    Library lib(*db);

    if (isJsonCmd) {
        try {
            nlohmann::json out;
            std::string err;
            if (cmd == "export-index") {
                if (!exportIndexJson(lib, args[1], out, &err)) {
                    printJsonError(err, cfg);
                    return 1;
                }
                writeJsonStdout(out);
                return 0;
            }
            if (cmd == "pdf-info") {
                PdfFetcher fetcher;
                if (pdfInfoFetch) {
                    fetcher = [&](const AttachmentRecord& att, std::string& localPathOut, std::string* ferr) -> bool {
                        if (!hasCreds) {
                            if (ferr) *ferr = "No Zotero account configured for --fetch";
                            return false;
                        }
                        CurlHttp curl(ca);
                        WriteGuard http(
                            curl, [&](const std::string& key) { return lib.annotation(key).has_value(); },
                            [&](const std::string& key) { return lib.attachment(key).has_value(); },
                            [&](const std::string& key) -> std::optional<std::vector<std::string>> {
                                if (auto item = lib.zoteroItem(key)) return item->tags;
                                return std::nullopt;
                            },
                            pluginChangeShapes(lib));
                        http.setLibraries([&](const std::string& key) { return lib.libraryOf(key); },
                                          [&](int64_t group) { return lib.libraryEditable(group); },
                                          [&](const std::string& key) { return lib.annotationEditable(key); });
                        auto files = makeFileStore(http, *cfg);
                        UserError uerr;
                        if (!files->download(att, filesDir, &uerr)) {
                            if (ferr) *ferr = uerr.message + (uerr.details.empty() ? "" : ": " + uerr.details);
                            return false;
                        }
                        localPathOut = filesDir + "/" + att.key + "/" + localFileName(att);
                        return true;
                    };
                }
                if (!pdfInfoJson(lib, args[1], kEngineEnv, pdfInfoFetch, fetcher, out, &err)) {
                    printJsonError(err, cfg);
                    return 1;
                }
                writeJsonStdout(out);
                return 0;
            }
            if (cmd == "ink-pull") {
                bool synced = false;
                if (!inkPullLocalOnly && hasCreds) {
                    auto att = lib.attachment(args[1]);
                    if (!att) {
                        printJsonError("attachment not found: " + args[1], cfg);
                        return 1;
                    }
                    CurlHttp curl(ca);
                    WriteGuard http(
                        curl, [&](const std::string& key) { return lib.annotation(key).has_value(); },
                        [&](const std::string& key) { return lib.attachment(key).has_value(); },
                        [&](const std::string& key) -> std::optional<std::vector<std::string>> {
                            if (auto item = lib.zoteroItem(key)) return item->tags;
                            return std::nullopt;
                        },
                        pluginChangeShapes(lib));
                    http.setLibraries([&](const std::string& key) { return lib.libraryOf(key); },
                                      [&](int64_t group) { return lib.libraryEditable(group); },
                                      [&](const std::string& key) { return lib.annotationEditable(key); });
                    if (lib.userId() == 0) lib.setUser(cfg->userId, "");
                    ZoteroApi api(http, cfg->userId, cfg->apiKey);
                    api.setOnWait([](const UserError& e) { std::fprintf(stderr, "%s\n", e.message.c_str()); });
                    SyncReport rep = SyncEngine(api, lib, att->lib).pullAnnotationsFor(args[1], nullptr);
                    if (rep.error) {
                        printJsonError(rep.error->message + (rep.error->details.empty() ? "" : ": " + rep.error->details), cfg);
                        return 1;
                    }
                    synced = true;
                }
                if (!inkPullJson(lib, args[1], synced, out, &err)) {
                    printJsonError(err, cfg);
                    return 1;
                }
                writeJsonStdout(out);
                return 0;
            }
            if (cmd == "ink-upsert") {
                if (!inkUpsertJson(lib, args[1], args[2], kEngineEnv, out, &err)) {
                    printJsonError(err, cfg);
                    return 1;
                }
                writeJsonStdout(out);
                return 0;
            }
        } catch (const DbError& e) {
            const UserError le = lib.lastError();
            printJsonError(le.kind == ErrorKind::None ? (std::string("Database error: ") + e.what())
                                                      : (le.message + (le.details.empty() ? "" : ": " + le.details)),
                           cfg);
            return 1;
        }
        return 1;
    }

    CurlHttp curl(ca);
    WriteGuard http(
        curl, [&](const std::string& key) { return lib.annotation(key).has_value(); },
        [&](const std::string& key) { return lib.attachment(key).has_value(); },  // reading positions
        [&](const std::string& key) -> std::optional<std::vector<std::string>> {  // the read tag
            if (auto item = lib.zoteroItem(key)) return item->tags;  // as Zotero has them
            return std::nullopt;
        },
        pluginChangeShapes(lib));  // plugin changes, each in its pending shape
    http.setLibraries([&](const std::string& key) { return lib.libraryOf(key); },
                      [&](int64_t group) { return lib.libraryEditable(group); },
                      [&](const std::string& key) { return lib.annotationEditable(key); });
    if (lib.userId() == 0) lib.setUser(cfg->userId, "");
    ZoteroApi api(http, cfg->userId, cfg->apiKey);
    auto nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    };
    api.setOnWait([](const UserError& e) { std::printf("%s\n", e.message.c_str()); });

    try {
        if (cmd == "pull") {
            const auto t0 = std::chrono::steady_clock::now();
            UserError discovery;
            if (!discoverGroups(api, lib, nowMs(), &discovery)) {
                if (unreachable(discovery) || foreignKey(discovery)) {  // nothing else would get through, or may
                    printError(discovery);
                    return 1;
                }
                std::printf("groups: not listed (%s); My Library only\n", discovery.message.c_str());
            }
            bool failed = false;
            std::vector<int64_t> groups;
            SyncReport all;
            for (int64_t l : librariesToSync(lib)) {
                SyncEngine engine(api, lib, l);
                std::string lastPhase;
                std::printf("%s\n", l == kMyLibrary ? "My Library" : ("group " + std::to_string(l) + ": " + lib.libraryName(l)).c_str());
                SyncReport rep = engine.pull([&](const SyncProgress& p) {
                    if (p.phase != lastPhase || p.done == p.total || p.done % 1000 < 50) {
                        std::printf("  %-12s %6d / %d\n", p.phase.c_str(), p.done, p.total);
                        std::fflush(stdout);
                    }
                    lastPhase = p.phase;
                });
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::printf("pull: version %lld -> %lld; items +%d ~%d -%d; annotations %d; conflicts %d; %.1f s; peak RSS %ld KB\n",
                            static_cast<long long>(rep.fromVersion), static_cast<long long>(rep.toVersion), rep.itemsAdded,
                            rep.itemsUpdated, rep.itemsRemoved, rep.annotations, rep.conflicts, secs, peakRssKb());
                if (l != kMyLibrary) {
                    groups.push_back(l);
                    scopeGroupError(rep, l, lib);
                    for (const auto& g : rep.groupErrors) std::printf("%s: can't sync: %s\n", g.name.c_str(), g.message.c_str());
                    all.groupErrors.insert(all.groupErrors.end(), rep.groupErrors.begin(), rep.groupErrors.end());
                }
                if (rep.error) {
                    printError(*rep.error);
                    failed = true;
                    break;  // My Library's: the groups wait for the next pull
                }
            }
            recordGroupErrors(lib, groups, all);
            if (failed) return 1;
        } else if (cmd == "groups") {
            const GroupAccess access = lib.keyGroupAccess();
            std::printf("key's group access: %s\n", access == GroupAccess::All    ? "all groups"
                                                     : access == GroupAccess::Some ? "some groups"
                                                     : access == GroupAccess::None ? "none"
                                                                                   : "unknown (pull first)");
            for (const auto& l : lib.libraries(true))
                std::printf("%10lld  %-30s %5lld items  %s%s%s\n", static_cast<long long>(l.id), l.name.c_str(),
                            static_cast<long long>(l.items), l.canEdit ? "editable" : "read-only",
                            l.enabled ? "" : ", switched off", l.gone ? ", no longer available" : l.unreadable ? ", not readable with this key" : "");
        } else if (cmd == "status") {
            LibraryStats s = lib.stats();
            std::printf("highlights: %s (%zu books opened)\n",
                        lib.allAnnotationsOffline() ? "all offline" : "on demand", lib.trackedAttachments().size());
            std::printf("library version %lld\nitems %lld (incl. child items)\nattachments %lld\nannotations %lld\n"
                        "collections %lld\nconflicts %lld\nunsent %lld\n",
                        static_cast<long long>(lib.libraryVersion()), static_cast<long long>(s.items),
                        static_cast<long long>(s.attachments), static_cast<long long>(s.annotations),
                        static_cast<long long>(s.collections), static_cast<long long>(s.conflicts),
                        static_cast<long long>(s.unsent));
        } else if (cmd == "search" && args.size() >= 2) {
            std::string q;
            for (size_t i = 1; i < args.size(); ++i) q += (i > 1 ? " " : "") + args[i];
            printItems(lib.search(q, 25));
        } else if (cmd == "collections") {
            printCollections(lib.collections(), "", 0);
        } else if (cmd == "highlights-offline" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
            SyncEngine(api, lib).setAllHighlightsOffline(args[1] == "on");
            std::printf("highlights: %s%s\n", args[1] == "on" ? "all offline" : "on demand",
                        args[1] == "on" ? " (downloaded at the next pull)" : "");
        } else if (cmd == "annotations" && args.size() == 2) {
            const auto t0 = std::chrono::steady_clock::now();
            SyncReport rep = SyncEngine(api, lib).pullAnnotationsFor(args[1], nullptr);
            std::printf("synced %d annotation(s) in %.1f s\n", rep.annotations,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            if (rep.error) printError(*rep.error);
            for (const auto& a : lib.annotationsFor(args[1]))
                std::printf("%s  %-9s %s  %s%s%s\n", a.key.c_str(), a.type.c_str(), a.sortIndex.c_str(),
                            a.text.substr(0, 80).c_str(), a.comment.empty() ? "" : "  // ", a.comment.substr(0, 60).c_str());
        } else if (cmd == "fetch" && args.size() == 3) {
            auto a = lib.attachment(args[1]);
            if (!a) {
                std::printf("error: No attachment %s in the local library (run pull first).\n", args[1].c_str());
                return 1;
            }
            auto files = makeFileStore(http, *cfg);  // WebDAV, or Zotero storage without one
            const auto t0 = std::chrono::steady_clock::now();
            UserError err;
            if (!files->download(*a, args[2], &err)) {
                printError(err);
                return 1;
            }
            a->localPath = args[2] + "/" + a->key + "/" + localFileName(*a);
            a->state = AttachmentRecord::State::Local;
            lib.upsertAttachment(*a);
            std::printf("fetched %s (md5 ok) in %.1f s\n", a->localPath.c_str(),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        } else if (cmd == "push") {
            bool failed = false;
            // Plugin changes no group can take (read-only, switched off, gone): refused, as the app does.
            if (const int refused = lib.refuseUnsendablePluginChanges(); refused > 0)
                std::printf("refused %d plugin change(s) to group libraries the Kindle can't edit\n", refused);
            std::vector<int64_t> groups;
            SyncReport all;
            for (int64_t l : librariesToSync(lib)) {
                SyncEngine engine(api, lib, l);
                const int pending = engine.pendingChanges();
                if (pending == 0 && l != kMyLibrary) continue;
                SyncReport rep = engine.push();
                if (l != kMyLibrary) {
                    groups.push_back(l);
                    scopeGroupError(rep, l, lib);
                    for (const auto& g : rep.groupErrors) std::printf("%s: can't sync: %s\n", g.name.c_str(), g.message.c_str());
                    all.groupErrors.insert(all.groupErrors.end(), rep.groupErrors.begin(), rep.groupErrors.end());
                }
                std::printf("push%s: %d change(s) pending, %d sent; conflicts %d; library version %lld\n",
                            l == kMyLibrary ? "" : (" (" + lib.libraryName(l) + ")").c_str(), pending, rep.annotations,
                            rep.conflicts, static_cast<long long>(rep.toVersion));
                if (rep.error) {
                    printError(*rep.error);
                    failed = true;
                }
            }
            recordGroupErrors(lib, groups, all);
            if (failed) return 1;
        } else if (cmd == "position-test" && args.size() == 2) {
            // Zotero's reading position for a fetched file -> its page here -> one page on -> sent back.
            auto a = lib.attachment(args[1]);
            if (!a || a->state != AttachmentRecord::State::Local || a->localPath.empty()) {
                std::printf("error: %s is not downloaded (run fetch first).\n", args[1].c_str());
                return 1;
            }
            UserError err;
            auto engine = loadEngine(a->contentType == "application/pdf" ? DocType::Pdf : DocType::Epub, kEngineEnv, &err);
            if (!engine || !engine->open(a->localPath, &err)) {
                printError(err);
                return 1;
            }
            engine->setLayout(Layout{});
            auto rp = lib.readingPosition(a->key);
            const std::string remote = rp ? rp->remote : "";
            const int page = remote.empty() ? 0 : engine->pageOfZoteroPosition(remote);
            std::printf("Zotero's position: %s -> page %d of %d\n", remote.empty() ? "(none)" : remote.c_str(), page + 1,
                        engine->pageCount());
            if (page < 0) return 1;
            const std::string next = engine->zoteroPosition(page + 1);
            std::printf("one page on: %s (back to page %d)\n", next.c_str(), engine->pageOfZoteroPosition(next) + 1);
            lib.setLocalPosition(a->key, next);
            SyncEngine sync(api, lib);
            SyncReport rep = sync.push();
            if (rep.error) {
                printError(*rep.error);
                return 1;
            }
            std::printf("sent; Zotero's setting is now version %lld\n",
                        static_cast<long long>(lib.readingPosition(a->key)->remoteVersion));
        } else if (cmd == "annotate-test" && (args.size() == 2 || args.size() == 3)) {
            auto a = lib.attachment(args[1]);
            if (!a || a->state != AttachmentRecord::State::Local || a->localPath.empty()) {
                std::printf("error: %s is not downloaded (run fetch first).\n", args[1].c_str());
                return 1;
            }
            const bool pdf = a->contentType == "application/pdf";
            UserError err;
            auto engine = loadEngine(pdf ? DocType::Pdf : DocType::Epub, kEngineEnv, &err);
            if (!engine || !engine->open(a->localPath, &err)) {
                printError(err);
                return 1;
            }
            const int page = (args.size() == 3 ? std::atoi(args[2].c_str()) : 3) - 1;
            std::optional<Selection> sel;
            // The first line of text from the top, wherever the page's margins are.
            auto good = [&] { return sel && !sel->text.empty() && sel->roundTrips; };
            for (int y = 120; y < 1400 && !good(); y += 20)
                for (int x : {200, 350, 500}) {
                    sel = engine->select(page, {x, y}, {x + 500, y});
                    if (good()) break;
                }
            if (!sel || sel->text.empty() || !sel->roundTrips) {
                std::printf("error: No selectable text on page %d.\n", page + 1);
                return 1;
            }
            AnnotationRecord n;
            n.parentKey = a->key;
            n.type = "highlight";
            n.text = sel->text;
            n.comment = kTestComment;
            n.color = "#ffd400";
            n.tags = {kTestTag};
            n.sortIndex = sel->sortIndex;
            n.position = sel->position;
            n.pageLabel = sel->pageLabel;
            const std::string key = lib.createAnnotation(n);
            std::printf("created %s on page %d: \"%s\"\n  position %s\n  sortIndex %s (run push to send it)\n",
                        key.c_str(), page + 1, sel->text.c_str(), sel->position.c_str(), sel->sortIndex.c_str());
        } else if (cmd == "annotate-test-edit" && args.size() == 2) {
            auto a = lib.annotation(args[1]);
            const bool ours = a && std::find(a->tags.begin(), a->tags.end(), kTestTag) != a->tags.end() &&
                              a->comment.rfind("[zk-test]", 0) == 0;
            if (!ours) {
                std::printf("error: %s is not one of this tool's zk-test annotations.\n", args[1].c_str());
                return 1;
            }
            a->color = "#2ea8e5";
            a->comment = "[zk-test] edited on the Kindle side";
            lib.editAnnotation(*a);
            std::printf("edited %s: blue, comment \"%s\" (run push to send it)\n", a->key.c_str(), a->comment.c_str());
        } else if (cmd == "select-bench" && (args.size() == 2 || args.size() == 3)) {
            // How long a selection takes on this device, as a reader drag asks for it.
            auto a = lib.attachment(args[1]);
            if (!a || a->state != AttachmentRecord::State::Local) {
                std::printf("error: %s is not downloaded.\n", args[1].c_str());
                return 1;
            }
            UserError err;
            auto engine = loadEngine(a->contentType == "application/pdf" ? DocType::Pdf : DocType::Epub, kEngineEnv, &err);
            if (!engine || !engine->open(a->localPath, &err)) {
                printError(err);
                return 1;
            }
            const int page = args.size() == 3 ? std::atoi(args[2].c_str()) : 20;
            auto ms = [](auto t0) {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            };
            Point from{0, 0};
            const auto first = std::chrono::steady_clock::now();
            for (int y = 150; y < 1300 && !from.x; y += 20)
                for (int x : {200, 350, 500})
                    if (auto s = engine->select(page, {x, y}, {x, y}); s && !s->text.empty()) {
                        from = {x, y};
                        break;
                    }
            if (!from.x) {
                std::printf("error: no text on page %d\n", page + 1);
                return 1;
            }
            std::printf("first selection on the page (chapter not yet parsed): %.1f ms\n", ms(first));
            double total = 0, worst = 0;
            const int steps = 20;
            for (int i = 1; i <= steps; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                auto s = engine->select(page, from, {from.x + 30 * i, from.y + 25 * i});
                const double t = ms(t0);
                total += t;
                worst = std::max(worst, t);
                if (i == steps && s) std::printf("last selection: %zu chars, %zu rects\n", s->text.size(), s->rects.size());
            }
            std::printf("select on page %d: %d drag steps, mean %.1f ms, worst %.1f ms\n", page + 1, steps, total / steps,
                        worst);
        } else if (cmd == "annotate-test-cleanup") {
            int n = 0;
            for (const auto& row : lib.recentAnnotations(0, 1'000'000)) {
                const AnnotationRecord& a = row.annotation;
                const bool tagged = std::find(a.tags.begin(), a.tags.end(), kTestTag) != a.tags.end();
                if (!tagged || a.comment.rfind("[zk-test]", 0) != 0) continue;  // only this tool's own
                lib.deleteAnnotation(a.key);
                std::printf("deleting %s \"%s\"\n", a.key.c_str(), a.text.substr(0, 60).c_str());
                ++n;
            }
            std::printf("%d test annotation(s) marked for deletion (run push to send)\n", n);
        } else {
            return usage();
        }
    } catch (const DbError& e) {
        printError(lib.lastError().kind == ErrorKind::None ? UserError{ErrorKind::Other, "Database error.", e.what()}
                                                           : lib.lastError());
        return 1;
    }
    return 0;
}
