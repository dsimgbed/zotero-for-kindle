#pragma once
// Up to three Zotero accounts on one device, each with its own data (spec: multiple accounts, Layout).
// AccountPaths is the only place an account's paths are formed.

#include "app/paths.h"
#include "sync/config.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zk {

constexpr size_t kMaxAccounts = 3;

struct AccountPaths {
    std::string root;           // <accountsRoot>/<userId>
    std::string dbPath;         // root/zotero.db
    std::string filesDir;       // root/files
    std::string previewDir;     // root/cache/preview
    std::string creDir;         // root/cache/cre
    std::string fullTextDir;    // root/cache/fulltext
    std::string stateDir;       // root/state
    std::string pluginDataRoot; // root/plugins
    std::string pluginStorage(const std::string& pluginId) const { return pluginDataRoot + "/" + pluginId + "/storage.db"; }
    std::string pluginData(const std::string& pluginId) const { return pluginDataRoot + "/" + pluginId + "/data"; }
};

AccountPaths accountPaths(const Paths&, int64_t userId);

// Every account's plugin data root (AccountPaths::pluginDataRoot), for each account dir under
// accountsRoot (a userID-named folder, listed in accounts.json or not; dot-dirs aren't accounts):
// removing a plugin deletes its data in each.
std::vector<std::string> pluginDataRoots(const Paths&);

// Plain data; hashing and the backoff rules live elsewhere.
struct PinHash {
    std::string salt, hash;  // base64
    int iterations = 3;      // Argon2i passes
    int memoryKiB = 1024;  // 1 MiB
};
struct PinBackoff {
    int misses = 0;
    int64_t waitUntilMs = 0;
};

struct AccountEntry {
    int64_t userId = 0;
    std::string name;  // Zotero username
    int order = 0;
    std::optional<PinHash> pin;
    bool lockOnSleep = false;
    PinBackoff backoff;
    // What the account sheet shows of an account that isn't open, kept by its session after each
    // good sync and when it stops (so nothing of another account's library is opened to show it).
    int64_t itemCount = 0;
    int unsentCount = 0;
    int64_t syncedWallMs = 0;  // the last good sync (0: not known)
    // Listed again after accounts.json couldn't be read (rebuildAccounts): its PIN, if it had one,
    // isn't known, so it opens only once its owner shows its API key (as Forgot PIN? does).
    bool needsKey = false;
};

// Opening it asks something first: its PIN, or (needsKey) its API key.
inline bool asksBeforeOpening(const AccountEntry& e) { return e.pin.has_value() || e.needsKey; }

struct AccountsState {
    int version = 1;
    int64_t active = 0;
    std::vector<AccountEntry> accounts;
};

// nullopt: no file or unreadable (a torn temp file is never read; the target is).
std::optional<AccountsState> readAccounts(const std::string& file);
// temp + fsync + rename, mode 0600. Refuses more than kMaxAccounts.
bool writeAccounts(const std::string& file, const AccountsState&);

// Drops entries lacking their data dir (the active id moves to the first left, or 0); an entry
// without credentials stays (its library is there; the app asks for the key). Orphan dirs and
// credential files are left and logged (.leftovers and other dot-dirs aren't accounts). Returns the
// log lines.
std::vector<std::string> repairAccounts(const Paths&, AccountsState&);

// A new account on this Kindle (Add account, once its key was checked): its folder made and its
// entry listed last (`active` unchanged). False — nothing listed — when the user is already on
// the Kindle, there are kMaxAccounts, or accounts.json is there but can't be read (writing a new
// list would drop the others), or can't be written.
bool addAccount(const Paths&, int64_t userId, const std::string& name);
// The account taken off this Kindle (Remove from this Kindle), and nothing of any other: its folder
// accounts/<userId>/ is first renamed aside (accounts/.removing-<userId>, so the list never names a
// half-deleted library), then its entry and its credentials go, then the folder is deleted.
// `active` moves to the first account left (0: none). The account must not be open. A config.json
// of the layout before accounts naming that user goes too (forgetOldConfig).
bool removeAccount(const Paths&, int64_t userId);
// Deletes <configDir>/config.json when it names `userId` (a key of the layout before accounts, or
// one a first Setup wrote): an account's key is kept only in its credentials file.
void forgetOldConfig(const Paths&, int64_t userId);
// Startup: what removals cut short left (accounts/.removing-<userId>, unlisted by construction) is
// deleted, with the credentials and config.json of a user no longer listed. Returns the log lines.
std::vector<std::string> finishRemovals(const Paths&, const AccountsState&);
// accounts.json there but unreadable: it is kept aside (accounts.json.bad-<time>) and the list made
// again from the account folders (userID-named, at most kMaxAccounts, by user ID), each entry
// needsKey (its PIN isn't known), named by `nameOf` (its library's username; "" when unknown).
// Returns the list written (nullopt: nothing to do, or it can't be written).
std::optional<AccountsState> rebuildAccounts(const Paths&, const std::function<std::string(int64_t userId)>& nameOf);
// The account's summary (itemCount, unsentCount, and syncedWallMs when `syncedWallMs` > 0) written
// into its entry; `name` (its library's Zotero username) only where the entry has none (an account
// added from a dropped config.json, before its first sync). False when it isn't listed or the
// list can't be written.
bool keepAccountSummary(const std::string& accountsFile, int64_t userId, int64_t items, int unsent, int64_t syncedWallMs,
                        const std::string& name = {});

// The account a tool works on (zk-sync --account): `userId`, or 0 for the active one; its key and
// its library. Before the account layout (no accounts.json): config.json and the old library, for
// 0 only. No config: `error` says why.
struct AccountTarget {
    int64_t userId = 0;
    std::optional<Config> config;
    std::string dbPath;
    std::string error;
};
AccountTarget accountTarget(const Paths&, int64_t userId);

}  // namespace zk
