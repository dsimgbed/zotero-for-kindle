#pragma once
// The frozen Grimoire × Zotero bridge (docs/bridge-contract.md, schema_version 1).
//
// Nothing here ever reads an app's config, credentials or accounts: the JSON written by this
// module carries only bibliographic metadata, citations and the notebook page to return to.
// Every reader rejects symlinks, enforces a size cap before loading, requires valid UTF-8 and
// schema_version 1; every writer goes through the atomic temp-file + fsync + rename sequence.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace zk {

class Library;

namespace bridge {

inline constexpr const char* kDefaultDir = "/mnt/us/grimoire_zotero_bridge";
inline constexpr const char* kEnvVar = "GZ_BRIDGE_DIR";
inline constexpr size_t kMaxJsonBytes = 262'144;   // handoff, return context, inbox events
inline constexpr size_t kMaxIndexBytes = 10'485'760;  // library_index.json
inline constexpr int kSchemaVersion = 1;

struct ReturnContext {
    std::string notebookId;  // "" opens Grimoire's normal start/last notebook
    uint32_t pageId = 0;
    uint32_t pageIndex = 0;
};

struct HandoffRequest {
    std::string nonce, source, target, action;
    int64_t createdAt = 0;
    // target == "zotero" (action "open_item")
    std::string itemKey, attachmentKey;
    uint32_t page = 0;
    std::string locator;
    // target == "grimoire" (action "return_essay")
    std::string notebookId;
    uint32_t pageId = 0, pageIndex = 0;
    // Persisted by Zotero when a Grimoire handoff carries one.
    ReturnContext returnTo;
    bool hasReturnTo = false;
};

struct CitationPayload {
    std::string id;
    int64_t createdAt = 0;
    std::string itemKey, attachmentKey;
    std::string shortCite, title, authors, year;
    uint32_t page = 0;
    std::string locator, quote, note;
};

// Where the bridge lives: GZ_BRIDGE_DIR when non-empty, else the on-device default. The supplied
// directory (when non-empty) always wins; no function here creates a directory on its own except
// the atomic writer that is explicitly asked to write into it.
std::string bridgeDir();
// True when a bridge is configured (env var set) or the default directory exists. Never creates it.
bool bridgeActive();

// ---- Strict field validation (the contract's table) ----------------------------------------
bool validKey(const std::string& s, size_t max = 32);  // item/attachment keys: 1..max [A-Za-z0-9_-]
bool validNotebookId(const std::string& s);             // 1..63, same alphabet
bool validNonceOrId(const std::string& s);             // 1..64, same alphabet
bool validPage(uint32_t page);                         // 0 or 1..1000000
bool validBoundedText(const std::string& s, size_t maxBytes);  // UTF-8, no NUL, <= maxBytes
bool validLocator(const std::string& s);               // <= 512 bytes, no NUL/control < 0x20
bool validUtf8(const std::string& s);
// One line of text for a JSON field: controls collapsed, UTF-8 faults replaced, bounded in bytes.
std::string sanitiseText(const std::string& s, size_t maxBytes);

// ---- Safe file I/O --------------------------------------------------------------------------
// Writes `<path>.tmp.<pid>.<nonce>` with O_EXCL|O_NOFOLLOW (0644), fsyncs it, then renames it
// over `path`. Refuses symlinks in the target and in any directory it creates. `why`, if given,
// receives a short non-sensitive reason on failure.
bool writeFileAtomic(const std::string& path, const std::string& content, std::string* why = nullptr);
// lstat + O_NOFOLLOW, a size cap checked before reading, then strict UTF-8. Missing files return
// false with a "missing" reason (nothing to warn about at a first start).
bool readFileSafe(const std::string& path, size_t maxBytes, std::string* out, std::string* why = nullptr);
bool pathIsRegularFile(const std::string& path);

// ---- Schema v1 documents --------------------------------------------------------------------
std::optional<HandoffRequest> parseHandoff(const std::string& text, std::string* why = nullptr);
std::optional<ReturnContext> parseReturnContext(const std::string& text, std::string* why = nullptr);
std::optional<CitationPayload> parseCitation(const std::string& text, std::string* why = nullptr);

// An empty `dir` means bridgeDir(). `return_context.json` round-trips through write/read.
bool writeReturnContext(const ReturnContext& ctx, const std::string& dir = {}, std::string* why = nullptr);
std::optional<ReturnContext> readReturnContext(const std::string& dir = {}, std::string* why = nullptr);

// Writes a target="grimoire" handoff (action "return_essay") for a clean exit 43.
bool writeReturnToGrimoireHandoff(const ReturnContext& ctx, const std::string& dir = {}, std::string* why = nullptr);
bool writeHandoff(const HandoffRequest& h, const std::string& dir = {}, std::string* why = nullptr);

// Reads handoff.json. A malformed, oversized or invalid file is unlinked (with a short,
// non-sensitive reason in `why`); a valid zotero handoff is consumed and its return_to persisted;
// a handoff aimed elsewhere is left untouched.
std::optional<HandoffRequest> consumeZoteroHandoff(const std::string& dir = {}, std::string* why = nullptr);

// Writes inbox/<epoch_ms>-<nonce>.json. `idOut`, if given, receives the file's id.
bool queueCitation(const CitationPayload& c, const std::string& dir = {}, std::string* idOut = nullptr,
                   std::string* why = nullptr);

// Exports only non-sensitive bibliographic fields from `lib`. Never touches config/accounts.
bool exportLibraryIndex(Library& lib, const std::string& dir = {}, std::string* why = nullptr,
                        size_t* countOut = nullptr);
// Startup export: only when no valid library_index.json is there yet.
bool exportLibraryIndexIfAbsent(Library& lib, const std::string& dir = {}, std::string* why = nullptr);

}  // namespace bridge
}  // namespace zk
