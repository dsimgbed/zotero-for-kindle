#pragma once
// First launch of the multiple-accounts version: a single-account install moves into the account
// layout (spec: multiple accounts, Migration). /mnt/us is FAT, where a rename is not guaranteed
// atomic on power loss, so the move is an ordered list of steps, each safe to run again: a step is
// done when its target exists and its source doesn't; both existing, it moves what is left (a
// folder: entry by entry); neither, it is skipped. A marker accounts/.migrating-<userId>, written
// before anything moves and removed last, makes the next launch resume after an interruption.

#include "app/paths.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zk {

enum class MigrateResult {
    NotNeeded,  // accounts.json is there (no marker), or there is no old install at all
    Migrated,   // moved now, or an interrupted move finished
    NoAccount,  // an old install without a userID anywhere: left untouched (Setup)
    Failed,     // a step failed (*why); the next launch resumes. Open no database meanwhile.
};

// A meta value of an existing library (e.g. "zotero.userName"), read without changing it; "" when
// there is none or it can't be read.
std::string readLibraryMeta(const std::string& dbPath, const char* key);

// Runs before anything opens a database. Leaves a config.json dropped over USB alone.
MigrateResult migrateToAccounts(const Paths&, std::string* why);

// The steps migrateToAccounts runs for `userId`, in order (the first writes the marker, the last
// removes it). Tests run a prefix to stand for an interruption. `flush` puts what was written on
// the disk (default ::sync()): after the moves, before anything says they are done, and again
// before the marker goes.
struct MigrationStep {
    std::string name;
    std::function<bool(std::string* why)> run;
};
std::vector<MigrationStep> migrationSteps(const Paths&, int64_t userId, std::function<void()> flush = {});

}  // namespace zk
