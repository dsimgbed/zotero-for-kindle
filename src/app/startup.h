#pragma once
// What the app starts with. First the account layout: a single-account install moves into it
// (migrate_accounts.h) before anything opens a database. Then the account: a config.json dropped
// over USB is imported (and removed), else the active account's credentials are read. No config
// -> the Setup screen.

#include <cstdint>
#include <optional>

#include "app/paths.h"
#include "sync/config.h"

namespace zk {

// Runs the migration, then points `paths`' dbPath, filesDir, cacheDir and stateDir at the active
// account's folder (until the account session owns them). Returns the active account's userId, 0
// when there is none (the old places stay: Setup). nullopt: the migration failed part-way — open
// no database; the next launch resumes it.
std::optional<int64_t> openAccountLayout(Paths&);

struct Startup {
    std::optional<Config> config;
    bool imported = false;  // came from a dropped config.json just now
};

Startup startupConfig(const Paths&);

}  // namespace zk
