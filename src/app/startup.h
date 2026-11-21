#pragma once
// What the app starts with. First the account layout: a single-account install moves into it
// (migrate_accounts.h) before anything opens a database. Then the account: a config.json dropped
// over USB is taken in (below), and the active account's credentials are read. No config -> the
// Setup screen.

#include <cstdint>
#include <optional>
#include <string>

#include "app/paths.h"
#include "sync/config.h"

namespace zk {

// Runs the migration, then points `paths`' dbPath, filesDir, cacheDir and stateDir at the active
// account's folder (everything takes the account's paths from its AccountSession; the updater
// backs up every account's library itself). Returns the active account's userId, 0 when there is none (the
// old places stay: Setup). nullopt (and *why): the migration failed part-way — open no database;
// the next launch resumes it.
std::optional<int64_t> openAccountLayout(Paths&, std::string* why = nullptr);

struct Startup {
    // What became of a config.json dropped over USB (/mnt/us/zotero/config.json):
    //   KeyUpdated    an account on the Kindle's own user: that account's key (its file, nothing else)
    //   Added         a new user, with fewer than three accounts listed: a new account, listed last,
    //                 named at its first sync; opened only if no account could be (else the open
    //                 one stays open)
    //   Refused       a new user with three accounts listed: nothing written, the file left (logged)
    //   FirstAccount  no account on the Kindle: Setup confirms it before anything is made (imported)
    //   Invalid       no userId or apiKey: left in place (logged)
    // The file is deleted once taken in (KeyUpdated, Added, FirstAccount). A user's key is only
    // ever written under that user's ID: never into another account.
    enum class Dropped { None, Invalid, KeyUpdated, Added, Refused, FirstAccount };
    std::optional<Config> config;  // the active account's; FirstAccount: the dropped one
    bool imported = false;         // FirstAccount: Setup says the settings came from config.json
    Dropped dropped = Dropped::None;
    int64_t droppedUser = 0;  // the dropped file's user (KeyUpdated, Added, Refused, FirstAccount)
};

Startup startupConfig(const Paths&);

}  // namespace zk
