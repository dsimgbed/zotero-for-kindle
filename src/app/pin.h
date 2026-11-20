#pragma once
// A per-account PIN (spec: multiple accounts, PIN binding): Argon2i hashing, the wrong-PIN backoff,
// and the forgotten-PIN reset key (the account's Zotero API key). Nothing here logs a PIN or a key.

#include "app/accounts.h"
#include "sync/config.h"

#include <cstdint>
#include <optional>
#include <string>

namespace zk {

// 4 to 6 ASCII digits.
bool validPinFormat(const std::string& pin);

// Argon2i, 16-byte random salt from /dev/urandom, parameters as in PinHash's defaults.
PinHash hashPin(const std::string& pin);
// Constant-time compare of the hash; false for a malformed stored hash.
bool checkPin(const PinHash& stored, const std::string& pin);

// Milliseconds left to wait before another try (0: none).
int64_t pinWaitMs(const PinBackoff&, int64_t nowMs);
// Misses 1-5 are free; the 6th waits 30 s, each further one doubles, capped at 30 min.
void recordMiss(PinBackoff&, int64_t nowMs);
void recordSuccess(PinBackoff&);

// The typed key (surrounding whitespace ignored) equals the account's API key; false when it has none.
bool keyMatches(const Config& stored, const std::string& typedKey);
// Reads the dropped reset-pin.txt, trims it and always deletes it; nullopt when absent or empty.
std::optional<std::string> takeResetKey(const std::string& dropFile);

}  // namespace zk
