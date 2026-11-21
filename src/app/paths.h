#pragma once
// Where the app keeps its files (spec §7.7).

#include <cstdint>
#include <string>

namespace zk {

struct Paths {
    std::string configDir;      // credentials (Kindle: root-only partition)
    std::string droppedConfig;  // config.json dropped over USB, imported then deleted; "" = none
    std::string dbPath;         // library database
    std::string filesDir;       // downloaded attachments, <filesDir>/<attachmentKey>/<name>
    std::string logDir;
    std::string appDir;        // the app's folder (updates replace its bin/lib/share); "" = no updates
    std::string launcherPath;  // the library launcher, Zotero.sh
    std::string pluginsDir;    // <pluginsDir>/<id>/plugin.json; outside appDir so updates never touch it
    std::string cacheDir;      // what can be fetched again: <cacheDir>/preview (zk.reader.open's documents)
    std::string stateDir;      // what the app keeps between runs: <stateDir>/home-tree.json (Home's kept tree)
    // Multiple accounts: the per-account layout (see accounts.h for AccountPaths). dbPath, filesDir,
    // cacheDir and stateDir above are the old single-account locations, kept for the migration and --db.
    std::string accountsRoot;         // <accountsRoot>/<userId>/ holds one account's data
    std::string deviceSettingsFile;   // device-level settings (not an account's)
    std::string configFile() const { return configDir + "/config.json"; }
    std::string accountsFile() const { return configDir + "/accounts.json"; }
    std::string credentialsFile(int64_t userId) const { return configDir + "/accounts/" + std::to_string(userId) + ".json"; }
};

Paths defaultPaths();

}  // namespace zk
