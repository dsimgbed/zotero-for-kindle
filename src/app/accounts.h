#pragma once
// Up to three Zotero accounts on one device, each with its own data (spec: multiple accounts, Layout).
// AccountPaths is the only place an account's paths are formed.

#include "app/paths.h"
#include "sync/config.h"

#include <cstdint>
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
};

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
// the Kindle, there are kMaxAccounts, or accounts.json can't be written.
bool addAccount(const Paths&, int64_t userId, const std::string& name);
// The account taken off this Kindle (Remove from this Kindle), and nothing of any other: its folder
// accounts/<userId>/ is first renamed aside (accounts/.removing-<userId>, so the list never names a
// half-deleted library), then its entry and its credentials go, then the folder is deleted.
// `active` moves to the first account left (0: none). The account must not be open.
bool removeAccount(const Paths&, int64_t userId);

}  // namespace zk
