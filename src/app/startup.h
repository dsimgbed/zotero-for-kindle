#pragma once
// What the app starts with. First the account layout: a single-account install moves into it
// (migrate_accounts.h) before anything opens a database. Then the account: a config.json dropped
// over USB is taken in (below), and the active account's credentials are read. No config -> the
// Setup screen.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "app/paths.h"
#include "net/errors.h"
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
    //   KeyPending    an account on the Kindle's own user: nothing changes until Zotero says the key
    //                 is that user's (applyDroppedKey); the file waits in place
    //   Added         a new user, with fewer than three accounts listed: a new account, listed last,
    //                 named at its first sync; opened only if no account could be (else the open
    //                 one stays open)
    //   Refused       a new user with three accounts listed: nothing written, the file left (logged)
    //   FirstAccount  no account on the Kindle: Setup confirms it before anything is made (imported)
    //   Invalid       no userId or apiKey: left in place (logged)
    // The file is deleted once taken in (Added, FirstAccount; a pending key once applied). A user's
    // key is only ever written under that user's ID: never into another account. An account's key
    // is never replaced on the file's word alone (USB access would otherwise reset a PIN: the
    // forgotten-PIN proof is the stored key).
    enum class Dropped { None, Invalid, KeyPending, Added, Refused, FirstAccount };
    std::optional<Config> config;  // the active account's; FirstAccount: the dropped one
    bool imported = false;         // FirstAccount: Setup says the settings came from config.json
    Dropped dropped = Dropped::None;
    int64_t droppedUser = 0;  // the dropped file's user (KeyPending, Added, Refused, FirstAccount)
};

Startup startupConfig(const Paths&);

// A dropped config.json for an account on the Kindle, waiting for Zotero's word (KeyPending).
std::optional<Config> pendingDroppedKey(const Paths&);
// What Zotero said of the pending key `c` (checkKeyOwner, as Update key asks):
//   Applied   it is c.userId's: saved as that account's credentials, the file deleted
//   Offline   Zotero couldn't be asked (unreachable): nothing changes, asked again later
//   Refused   another user's, or a key Zotero doesn't take: nothing changes, the file left (logged)
//   None      no such file any more, or it changed meanwhile (asked again)
enum class DroppedKey { None, Applied, Offline, Refused };
DroppedKey settleDroppedKey(const Paths&, const Config& c, const UserError& owner);
// Both at once (`owner` asks Zotero; on the UI thread only in tests). `applied`: the key kept.
DroppedKey applyDroppedKey(const Paths&, const std::function<UserError(const Config&)>& owner, Config* applied = nullptr);

}  // namespace zk
