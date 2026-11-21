#include "app/startup.h"

#include "app/accounts.h"
#include "app/migrate_accounts.h"
#include "core/log.h"
#include "sync/libraries.h"

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
    if (listed) {  // the same user: its key once Zotero says it is that user's (applyDroppedKey)
        log(LogLevel::Info, "config: %s is for account %lld: kept until Zotero confirms the key", p.droppedConfig.c_str(), user);
        return Dropped::KeyPending;
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

bool sameConfig(const Config& a, const Config& b) {
    return a.userId == b.userId && a.apiKey == b.apiKey && a.webdavUrl == b.webdavUrl && a.webdavUser == b.webdavUser &&
           a.webdavPassword == b.webdavPassword;
}

}  // namespace

std::optional<Config> pendingDroppedKey(const Paths& p) {
    if (p.droppedConfig.empty()) return std::nullopt;
    auto c = readConfigFile(p.droppedConfig);
    if (!c) return std::nullopt;
    const auto accounts = readAccounts(p.accountsFile());
    const bool listed = accounts && std::any_of(accounts->accounts.begin(), accounts->accounts.end(),
                                                [&](const AccountEntry& e) { return e.userId == c->userId; });
    return listed ? c : std::nullopt;
}

DroppedKey settleDroppedKey(const Paths& p, const Config& c, const UserError& owner) {
    const auto now = pendingDroppedKey(p);
    if (!now || !sameConfig(*now, c)) return DroppedKey::None;  // gone or changed while Zotero was asked
    const long long user = static_cast<long long>(c.userId);
    if (owner.kind != ErrorKind::None) {
        if (unreachable(owner)) return DroppedKey::Offline;
        log(LogLevel::Warn, "config: the key in %s isn't account %lld's (%s): not used, left in place", p.droppedConfig.c_str(),
            user, owner.details.c_str());
        return DroppedKey::Refused;
    }
    if (!saveCredentials(p, c)) {
        log(LogLevel::Error, "config: account %lld's new key can't be saved; %s left in place", user, p.droppedConfig.c_str());
        return DroppedKey::Refused;
    }
    dropTaken(p.droppedConfig);
    log(LogLevel::Info, "config: account %lld's new key from %s, confirmed by Zotero, is kept", user, p.droppedConfig.c_str());
    return DroppedKey::Applied;
}

DroppedKey applyDroppedKey(const Paths& p, const std::function<UserError(const Config&)>& owner, Config* applied) {
    const auto c = pendingDroppedKey(p);
    if (!c) return DroppedKey::None;
    const DroppedKey r = settleDroppedKey(p, *c, owner(*c));
    if (r == DroppedKey::Applied && applied) *applied = *c;
    return r;
}

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
