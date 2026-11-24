#pragma once
// Headless JSON subcommands for zk-sync (docs/unified-ink-sync-contract.md, schema_version 1).
// All functions operate directly on local SQLite (and optionally a caller-provided PDF fetcher)
// and never read or emit credentials.

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "docs/document.h"
#include "store/library.h"

namespace zk {

constexpr int kZkSyncJsonSchemaVersion = 1;
constexpr size_t kZkSyncMaxPayloadBytes = 10u * 1024 * 1024;  // 10 MiB

using PdfFetcher = std::function<bool(const AttachmentRecord&, std::string& localPathOut, std::string* err)>;

// Atomically exports the Zotero catalog (superset of library_index.json) to `outPath`
// and populates `summaryOut` with {schema_version, out_path, item_count, exported_at}.
bool exportIndexJson(Library& lib, const std::string& outPath, nlohmann::json& summaryOut, std::string* err);

// Returns cache status, local PDF path, and per-page PDF geometry in points.
// If `!cached && fetch`, invokes `fetcher`, verifies the downloaded file on disk, and updates
// SQLite attachment state to Local before inspecting pages.
bool pdfInfoJson(Library& lib, const std::string& attachmentKey, const EngineEnv& env, bool fetch,
                 const PdfFetcher& fetcher, nlohmann::json& out, std::string* err);

// Emits all non-deleted ink annotations plus read-only highlight/underline summaries and
// other_counts for `attachmentKey`.
bool inkPullJson(Library& lib, const std::string& attachmentKey, bool synced, nlohmann::json& out, std::string* err);

// Reads `inputJsonPath` safely (non-symlink, <= 10 MiB, valid UTF-8), validates the entire
// payload, and applies all ink annotation mutations atomically inside a SQLite transaction.
bool inkUpsertJson(Library& lib, const std::string& attachmentKey, const std::string& inputJsonPath,
                   const EngineEnv& env, nlohmann::json& out, std::string* err);

// Validates and applies an already-parsed JSON payload atomically into SQLite.
bool inkUpsertJsonPayload(Library& lib, const std::string& attachmentKey, const nlohmann::json& payload,
                          const EngineEnv& env, nlohmann::json& out, std::string* err);

}  // namespace zk
