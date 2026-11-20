#include "app/startup.h"

#include "app/accounts.h"
#include "app/migrate_accounts.h"
#include "core/log.h"

namespace zk {

std::optional<int64_t> openAccountLayout(Paths& p) {
    std::string why;
    if (migrateToAccounts(p, &why) == MigrateResult::Failed) {
        log(LogLevel::Error, "migration to the account layout failed (%s); resumed at the next start", why.c_str());
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

Startup startupConfig(const Paths& p) {
    Startup s;
    if (!p.droppedConfig.empty()) s.imported = importDroppedConfig(p.droppedConfig, p.configDir);
    if (!s.imported)
        if (const auto accounts = readAccounts(p.accountsFile()); accounts && accounts->active)
            s.config = loadCredentials(p, accounts->active);
    if (!s.config) s.config = loadConfig(p.configDir);
    return s;
}

}  // namespace zk
