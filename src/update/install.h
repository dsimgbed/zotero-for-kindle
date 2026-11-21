#pragma once
// Putting a verified release in place, so that a failed update can always be undone.
//
// installRelease copies the current bin/lib/share, the launcher and the libraries into
// <appDir>/update/old/ — every account's database as update/old/accounts/<userId>/zotero.db (not
// its files), and a library of the layout before accounts as update/old/zotero.db — and the
// account list and an old config.json beside themselves (<file>.old, mode 0600, off the USB
// drive). Short of space for every library, only the open account's. It writes update/state = "installing <from> <to>", swaps
// the new folders in (current ones to update/trash/), then writes "installed <from> <to>". The
// app then exits with 75 and the launcher (assets/launcher/Zotero.sh, protocol format 1) starts
// the new version, marking it "tried"; if it never confirms, the launcher restores update/old/.
// confirmStartup is that confirmation.

#include "net/errors.h"
#include "update/version.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zk {

struct AccountLibrary {
    int64_t userId = 0;
    std::string dbPath;  // /mnt/us/zotero/accounts/<userId>/zotero.db
};

struct InstallPaths {
    std::string appDir;        // /mnt/us/zotero; empty = can't install (the desktop)
    std::string launcherPath;  // /mnt/us/documents/Zotero.sh
    std::string configFile;    // /var/local/zotero/config.json: the account before accounts (if there)
    std::string dbPath;        // /mnt/us/zotero/zotero.db: the library before accounts (if there)
    std::string accountsFile;  // /var/local/zotero/accounts.json
    // Every account's library, the open one first (asked at install time: accounts come and go).
    std::function<std::vector<AccountLibrary>()> libraries;
    std::string updateDir() const { return appDir + "/update"; }
};

// The zip's zotero/* into newDir/*, documents/Zotero.sh as newDir/Zotero.sh; programs made
// executable; refused (and newDir removed) if an entry escapes or a needed file is missing.
bool unpackRelease(const std::string& zipPath, const std::string& newDir, UserError*);

// `beat` is called between steps (the stall watchdog). Fails without changing the install if
// anything goes wrong before the swap; a failure during the swap leaves state "installing", which
// the launcher undoes at the next start.
struct InstallHooks {
    std::function<void()> flush;  // to disk before each state change; default sync()
    std::function<int64_t(const std::string& dir)> freeSpace;  // bytes; default statvfs
};
bool installRelease(const InstallPaths&, const std::string& newDir, Version from, Version to,
                    std::function<void(const char* step)> beat, UserError*, InstallHooks hooks = {});

enum class StartupNote { None, Updated, RolledBack };
struct Confirmed {
    StartupNote note = StartupNote::None;
    Version from, to;
};

// The start is good: the backups are deleted. Updated: this is the new version's first good start.
// RolledBack: the launcher went back after a failed update.
Confirmed confirmStartup(const InstallPaths&, Version running);
Confirmed confirmStartup(const InstallPaths&);  // running = appVersion()

// The confirmation of an update, deferred: a new version counts as good only after it has been
// in front kConfirmMs in all (like plugins' safe mode, kSafeModeConfirmMs), not at its first frame:
// what comes after it (plugins, a sync, a book) could still crash it, and the launcher can roll
// back only while the backup is there. Once due, the deletion waits for the next background (the
// screensaver, a system dialog), never mid-reading. A clean end confirms too (a reader who quits at
// once is not a crash, and would otherwise find the old version started again); so does installing
// the next update. Anything else that ends the app first rolls the update back at the next start,
// a Kindle shutdown or restart (SIGTERM) within that first minute in front included.
// With no update of the running version to confirm, the first screen tidies update/ at once.
class UpdateConfirm {
public:
    static constexpr int64_t kConfirmMs = 60'000;
    UpdateConfirm(InstallPaths, Version running);
    Confirmed atFirstScreen();  // what to tell the reader now; an update's backups stay
    void tick(int64_t nowMs, bool foreground);  // every loop step: confirms once it is time
    std::optional<int64_t> wakeAt(int64_t nowMs) const;  // when tick must run next (none: asleep, or done)
    void cleanEnd();
    bool pending() const { return pending_; }

private:
    InstallPaths paths_;
    Version running_;
    bool pending_ = true;
    bool due_ = false;         // in front long enough: confirmed at the next background
    int64_t frontMs_ = 0;      // in front, in all, until frontSince_
    int64_t frontSince_ = -1;  // in front since (-1: in the background)
    void confirm();
};

}  // namespace zk
