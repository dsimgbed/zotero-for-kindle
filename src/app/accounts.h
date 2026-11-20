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

// Plain data; hashing and the backoff rules live elsewhere.
struct PinHash {
    std::string salt, hash;  // base64
    int iterations = 3;      // Argon2i passes
    int memoryKiB = 1;
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

// Drops entries lacking credentials or their data dir (the active id moves to the first left, or 0);
// orphan dirs and credential files are left and logged. Returns the log lines.
std::vector<std::string> repairAccounts(const Paths&, AccountsState&);

}  // namespace zk
