#pragma once
// A per-account PIN (spec: multiple accounts, PIN binding): Argon2i hashing, the wrong-PIN backoff,
// and the forgotten-PIN reset key (the account's Zotero API key). Nothing here logs a PIN or a key.

#include "app/accounts.h"
#include "sync/config.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace zk {

// 4 to 6 ASCII digits.
bool validPinFormat(const std::string& pin);

// Argon2i, 16-byte random salt from /dev/urandom, parameters as in PinHash's defaults.
// nullopt when no random salt could be read.
std::optional<PinHash> hashPin(const std::string& pin);
// Constant-time compare of the hash; false for a malformed stored hash.
bool checkPin(const PinHash& stored, const std::string& pin);

// Milliseconds left to wait before another try (0: none), never more than 30 min (a clock moved back).
int64_t pinWaitMs(const PinBackoff&, int64_t nowMs);
// Misses 1-5 are free; the 6th waits 30 s, each further one doubles, capped at 30 min.
void recordMiss(PinBackoff&, int64_t nowMs);
void recordSuccess(PinBackoff&);

// The typed key (surrounding whitespace ignored) equals the account's API key; false when it has none.
bool keyMatches(const Config& stored, const std::string& typedKey);
// Reads (at most 4 KB of) the dropped reset-pin.txt, trims it and always deletes it, readable or not; nullopt when absent or empty.
std::optional<std::string> takeResetKey(const std::string& dropFile);

// The PINs of the accounts in accounts.json as the screens ask them: every check goes through the
// backoff, and a miss is written to accounts.json at once (a restart doesn't reset it). The wait is
// on the wall clock (it survives a restart) and, within a run, also on a monotonic one: moving the
// clock back shortens no wait begun in this run (moving it back across a restart is accepted as a
// limit of a lock on the screens).
class AccountPins {
public:
    AccountPins(std::string accountsFile, std::function<int64_t()> wallMs, std::function<int64_t()> monoMs);

    enum class Result {
        Ok,       // the PIN (or the key) is right: the misses are forgotten
        Wrong,    // counted as a miss
        Waiting,  // not tried: a wait is still running (waitMs)
        Failed,   // no such account, or accounts.json can't be read or written
    };
    std::optional<AccountEntry> entry(int64_t userId) const;
    int64_t waitMs(int64_t userId) const;
    Result tryPin(int64_t userId, const std::string& pin);
    // The forgotten-PIN proof: `key` is the account's API key (`stored`: its credentials). Right, it
    // also clears needsKey (and so does keyProved). tryPin on a needsKey account is Failed.
    Result tryKey(int64_t userId, const Config& stored, const std::string& key);
    // A miss of another kind.
    void miss(int64_t userId);
    // Zotero said a typed key is the account's (its new key): the owner is proved; misses forgotten.
    void keyProved(int64_t userId);
    // Hashes and keeps the PIN (Lock on sleep turned on when it had none); false if it can't.
    bool setPin(int64_t userId, const std::string& pin);
    bool removePin(int64_t userId);
    bool setLockOnSleep(int64_t userId, bool on);

private:
    bool update(int64_t userId, const std::function<void(AccountEntry&)>&);
    void missed(int64_t userId);
    std::string file_;
    std::function<int64_t()> wall_, mono_;
    std::map<int64_t, int64_t> monoUntil_;  // per account: the end of its wait on the monotonic clock
};

}  // namespace zk
