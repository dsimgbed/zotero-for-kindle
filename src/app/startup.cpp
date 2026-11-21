#include "app/startup.h"

#include "app/accounts.h"
#include "app/migrate_accounts.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <unistd.h>

namespace zk {

namespace fs = std::filesystem;

std::optional<int64_t> openAccountLayout(Paths& p, std::string* whyOut) {
    std::string why;
    if (migrateToAccounts(p, &why) == MigrateResult::Failed) {
        log(LogLevel::Error, "migration to the account layout failed (%s); resumed at the next start", why.c_str());
        if (whyOut) *whyOut = why;
        return std::nullopt;
    }
    const auto accounts = readAccounts(p.accountsFile());
    if (!accounts || accounts->active == 0) return 0;
    const AccountPaths a = accountPaths(p, accounts->active);
    p.dbPath = a.dbPath;
    p.filesDir = a.filesDir;
    p.cacheDir = a.root + "/cache";
    p.stateDir = a.stateDir;
    return accounts->active;
}

namespace {

void dropTaken(const std::string& path) {
    if (::unlink(path.c_str()) != 0)
        log(LogLevel::Warn, "config: taken in, but can't delete %s: %s", path.c_str(), std::strerror(errno));
}

// A config.json dropped over USB, taken in as Startup::Dropped says.
Startup::Dropped takeDropped(const Paths& p, Startup& s) {
    using Dropped = Startup::Dropped;
    std::error_code ec;
    if (p.droppedConfig.empty() || !fs::exists(p.droppedConfig, ec)) return Dropped::None;
    const auto c = readConfigFile(p.droppedConfig);
    if (!c) {
        log(LogLevel::Warn, "config: %s is missing userId or apiKey; left in place", p.droppedConfig.c_str());
        return Dropped::Invalid;
    }
    s.droppedUser = c->userId;
    const auto accounts = readAccounts(p.accountsFile());
    const bool listed = accounts && std::any_of(accounts->accounts.begin(), accounts->accounts.end(),
                                                [&](const AccountEntry& e) { return e.userId == c->userId; });
    const long long user = static_cast<long long>(c->userId);
    if (listed) {  // the same user: the account's key, under its own ID
        if (!saveCredentials(p, *c)) {
            log(LogLevel::Error, "config: the key of account %lld from %s can't be saved; left in place", user, p.droppedConfig.c_str());
            return Dropped::Invalid;
        }
        dropTaken(p.droppedConfig);
        log(LogLevel::Info, "config: %s is account %lld's new key", p.droppedConfig.c_str(), user);
        return Dropped::KeyUpdated;
    }
    if (!accounts || accounts->accounts.empty()) {  // a first account: Setup checks it first
        if (!importDroppedConfig(p.droppedConfig, p.configDir)) return Dropped::Invalid;
        s.config = c;
        s.imported = true;
        return Dropped::FirstAccount;
    }
    if (accounts->accounts.size() >= kMaxAccounts) {
        log(LogLevel::Warn, "config: %s is for account %lld, and this Kindle has %zu accounts already: refused, left in place",
            p.droppedConfig.c_str(), user, accounts->accounts.size());
        return Dropped::Refused;
    }
    if (!saveCredentials(p, *c) || !addAccount(p, c->userId, "")) {
        fs::remove(p.credentialsFile(c->userId), ec);  // nothing left behind
        log(LogLevel::Error, "config: account %lld from %s can't be added; left in place", user, p.droppedConfig.c_str());
        return Dropped::Refused;
    }
    dropTaken(p.droppedConfig);
    log(LogLevel::Info, "config: account %lld added from %s", user, p.droppedConfig.c_str());
    return Dropped::Added;
}

}  // namespace

Startup startupConfig(const Paths& p) {
    Startup s;
    s.dropped = takeDropped(p, s);
    if (s.dropped == Startup::Dropped::FirstAccount) return s;
    if (auto accounts = readAccounts(p.accountsFile()); accounts && accounts->active) {
        s.config = loadCredentials(p, accounts->active);
        // An account just added, and none to open (the active one has no key): the new one opens.
        if (!s.config && s.dropped == Startup::Dropped::Added) {
            accounts->active = s.droppedUser;
            if (writeAccounts(p.accountsFile(), *accounts)) s.config = loadCredentials(p, s.droppedUser);
        }
    }
    if (!s.config) s.config = loadConfig(p.configDir);
    return s;
}

}  // namespace zk
