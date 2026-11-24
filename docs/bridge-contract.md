# Grimoire × Zotero Bridge Contract (`schema_version: 1`)

This document is the single frozen IPC and on-device bridge contract between `grimoire-scribe`, `zotero-for-kindle`, and the Kindle Trampoline Launcher (`bridge/GrimoireZotero.sh`).

## 1. Bridge Directory & File System Invariants

- **Default on-device path:** `/mnt/us/grimoire_zotero_bridge`
- **Override environment variable:** `GZ_BRIDGE_DIR` (if set and non-empty, both apps and the trampoline use this path instead of `/mnt/us/grimoire_zotero_bridge`).
- **Standalone / absent behavior:** If `GZ_BRIDGE_DIR` does not exist, both `grimoire-scribe` and `zotero-for-kindle` MUST operate 100% normally without errors, crashes, or UI regressions.
- **Directory layout:**
  ```text
  $GZ_BRIDGE_DIR/
    handoff.json              # Single-shot workspace switch payload (consumed & deleted on read)
    return_context.json       # Last known Grimoire essay/page to return to (non-sensitive)
    library_index.json        # Read-only Zotero catalog snapshot exported by zotero-for-kindle
    inbox/                    # Append-safe per-event citation files written by zotero-for-kindle
      <epoch_ms>-<nonce>.json
    bridge.log                # Trampoline & circuit-breaker diagnostic log (never contains secrets)
  ```
- **Atomic writes (mandatory for all JSON writers):**
  1. Ensure target directory exists and is NOT a symlink (`lstat` / `O_NOFOLLOW`).
  2. Write complete UTF-8 JSON to `<final_path>.tmp.<pid>.<nonce>` opened with `O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW` (mode `0644`).
  3. `fsync()` the temporary file descriptor and close it.
  4. Atomically `rename()` temporary path to `<final_path>`.
  5. Best-effort `fsync()` on the parent directory (tolerating `EINVAL`/`EBADF` on Kindle FAT/FUSE).
- **Safe reads (mandatory for all JSON readers):**
  1. Reject symlinks (`O_NOFOLLOW` / `symlink_metadata`).
  2. Enforce hard size caps BEFORE loading into memory:
     - `handoff.json`, `return_context.json`, and each `inbox/*.json`: **256 KiB** (`262144` bytes).
     - `library_index.json`: **10 MiB** (`10485760` bytes).
  3. Require valid UTF-8 and `"schema_version": 1`.
  4. Never pass any field parsed from JSON to `sh -c`, `system()`, `eval`, or unquoted shell expansion.

## 2. Strict Field Validation Rules

| Field | Rule |
|---|---|
| `schema_version` | Integer `1` (required on every bridge JSON document). |
| `item_key`, `attachment_key` | Non-empty (when present) ASCII `1..=32` chars, strictly `[A-Za-z0-9_-]`. Reject `/`, `\`, `..`, NUL, spaces. |
| `notebook_id` | ASCII `1..=63` chars, strictly `[A-Za-z0-9_-]` (matches Grimoire `STORE_ID_LEN = 64`). |
| `page_id` | Unsigned 32-bit integer (`0` = unspecified; `>= 1` = Grimoire stable page ID). |
| `page_index` | Non-negative 32-bit integer (`0`-based fallback index in notebook). |
| `page` (Zotero) | Non-negative 32-bit integer (`0` = unspecified; `1..=1000000` = 1-based page number). |
| `locator` | Optional UTF-8 string, `<= 512` bytes, no NUL or ASCII control chars `< 0x20`. |
| `nonce` / `id` | ASCII `1..=64` chars, strictly `[A-Za-z0-9_-]`. |
| `short_cite`, `citation_key`, `authors`, `year`, `item_type` | UTF-8 strings, no NUL; control chars stripped/collapsed; bounded (`<= 256` bytes each). |
| `title` | UTF-8 string, `<= 512` bytes, no NUL. |
| `quote`, `note` | Optional UTF-8 strings, `<= 4096` bytes each, no NUL. |

**Zero-Credential Invariant:** `handoff.json`, `return_context.json`, `library_index.json`, `inbox/*.json`, and `bridge.log` MUST NEVER contain Zotero API keys, WebDAV URLs/usernames/passwords, auth tokens, account PINs, or `config.json`/`accounts.json` contents.

## 3. Versioned JSON Schemas (`schema_version: 1`)

### 3.1 `handoff.json`
Single-shot workspace switch request. Written atomically right before the active app begins clean shutdown and exits with its handoff exit code. Consumed and immediately unlinked by the target app at startup.

```json
{
  "schema_version": 1,
  "nonce": "1732350000123-a1b2c3d4",
  "created_at": 1732350000,
  "source": "grimoire",
  "target": "zotero",
  "action": "open_item",
  "zotero": {
    "item_key": "ABCD1234",
    "attachment_key": "EFGH5678",
    "page": 42,
    "locator": ""
  },
  "grimoire": {
    "notebook_id": "20261123-101500-ab12",
    "page_id": 7,
    "page_index": 3
  },
  "return_to": {
    "target": "grimoire",
    "notebook_id": "20261123-101500-ab12",
    "page_id": 7,
    "page_index": 3
  }
}
```

- `target`: `"zotero"` or `"grimoire"`.
- `action`: `"open_item"` (for `target: "zotero"`) or `"return_essay"` (for `target: "grimoire"`).
- When `target == "zotero"`: `zotero.item_key` is required; `attachment_key`, `page`, and `locator` are optional. `return_to` (if present and valid) is persisted by Zotero to `return_context.json`.
- When `target == "grimoire"`: `grimoire.notebook_id` may be empty (opens Grimoire's normal start/last notebook) or a valid `notebook_id` with `page_id` (preferred) and `page_index` (fallback).
- **Staleness / corruption cleanup:** If `handoff.json` fails validation, exceeds 256 KiB, or is malformed, the consumer (or trampoline) deletes it and logs a short non-sensitive message.

### 3.2 `return_context.json`
Persisted by `zotero-for-kindle` whenever it consumes a Grimoire→Zotero handoff containing a valid `return_to` block (and optionally updated by Grimoire on startup/notebook open if desired). Allows "Return to Grimoire" from any Zotero screen to jump straight back to the active Grimoire essay page.

```json
{
  "schema_version": 1,
  "updated_at": 1732350005,
  "notebook_id": "20261123-101500-ab12",
  "page_id": 7,
  "page_index": 3
}
```

### 3.3 `library_index.json`
Compact read-only catalog snapshot exported atomically by `zotero-for-kindle` after sync (`SyncFinished`), on startup (when bridge dir exists and index is absent), and on demand via `zk.bridge.refreshIndex()`.

```json
{
  "schema_version": 1,
  "exported_at": 1732350000,
  "item_count": 1,
  "items": [
    {
      "item_key": "ABCD1234",
      "attachment_key": "EFGH5678",
      "citation_key": "Kuhn 1962",
      "title": "The Structure of Scientific Revolutions",
      "authors": "Kuhn",
      "year": "1962",
      "item_type": "book",
      "collections": ["Philosophy of Science"],
      "tags": ["epistemology"],
      "has_pdf": true,
      "has_epub": false,
      "has_snapshot": false
    }
  ]
}
```

### 3.4 Append-Safe Citation Inbox (`inbox/<epoch_ms>-<nonce>.json`)
Per-event JSON files eliminate read-modify-write races when queuing multiple citations from Zotero while Grimoire is closed. Filenames MUST match `^[0-9]+-[A-Za-z0-9_-]+\.json$` (no subdirectories, no symlinks). Grimoire scans `inbox/*.json` in lexicographical order, validates each file, imports/buffers valid events, and unlinks each consumed file (and deletes malformed/oversized files so they never poison the queue).

```json
{
  "schema_version": 1,
  "id": "1732350012345-f0e1d2",
  "created_at": 1732350012,
  "item_key": "ABCD1234",
  "attachment_key": "EFGH5678",
  "short_cite": "Kuhn 1962, p. 42",
  "title": "The Structure of Scientific Revolutions",
  "authors": "Kuhn",
  "year": "1962",
  "page": 42,
  "locator": "",
  "quote": "Normal science, the activity in which most scientists inevitably spend almost all their time...",
  "note": ""
}
```

*(Optional compatibility: if a legacy single-file `citation_inbox.json` with `schema_version: 1` and `"citations": [...]` exists, Grimoire may also drain and unlink it.)*

## 4. Process Exit Codes & Trampoline State Machine

| Exit Code | Producer | Meaning |
|---|---|---|
| `0` | Either app | User cleanly exited workspace → Trampoline terminates and restores Kindle UI. |
| `42` | `grimoire-scribe` | Clean handoff to `zotero-for-kindle` (`handoff.json` written; notebook saved; `/dev/fb0` & evdev closed). |
| `43` | `zotero-for-kindle` | Clean handoff to `grimoire-scribe` (`handoff.json` written; Scribe ink flushed; X11/evdev/wet-ink closed). |
| `75` | `zotero-for-kindle` | Upstream Zotero self-update restart → Trampoline relaunches Zotero cleanly. |
| Any other | Either app | Unexpected exit / crash → Crash-loop circuit breaker logic applies. |

### 4.1 Kindle Display Stack Coordination (Critical Hardware Invariant)
- `grimoire-scribe` renders directly to `/dev/fb0` (FBInk) + raw `/dev/input/*` and pauses Kindle UI (`awesome`, `pillow`, `volumd`) inside `kindle/app/grimoire.sh`, restoring them on exit.
- `zotero-for-kindle` is an X11 fullscreen app (`DISPLAY=:0`) requiring `awesome`/X11 to be running; it manages Kindle Scribe wet ink (`/tmp/zk-wet-ink-session`) and flushes ink during `app.closeAll()`.
- **Mandatory serialization rule:** Neither app may ever spawn the other as a child process. App A MUST finish all saves/flushes, close all display/input descriptors, and exit completely BEFORE the Trampoline launches App B.
- **Trampoline (`bridge/GrimoireZotero.sh`) responsibilities:**
  1. Single-instance lock (atomic symlink/mkdir lock in `/tmp/grimoire_zotero_trampoline.lock` with stale-PID cleanup).
  2. Start with Grimoire (or Zotero if `--start zotero` or a valid fresh `handoff.json` targets Zotero).
  3. On exit `42` from Grimoire → launch Zotero (`--handoff`).
  4. On exit `43` from Zotero → launch Grimoire.
  5. On exit `75` from Zotero → relaunch Zotero (bounded retry).
  6. **Crash-loop circuit breaker:**
     - Measure wall duration of each app run (`date +%s`).
     - If an app exits non-zero (other than `42`/`43`/`75`) in `< 3` seconds twice in a row, OR if handoff bounces occur rapidly (`< 3` seconds) more than 4 times in a row:
       - Remove `$GZ_BRIDGE_DIR/handoff.json`.
       - Log the fault to `$GZ_BRIDGE_DIR/bridge.log`.
       - Ensure Kindle UI / `awesome` / `pillow` / wet-ink are restored (when running on Kindle).
       - Exit cleanly with non-zero diagnostic code (never looping infinitely).

## 5. Grimoire Sidecar Persistence (`zoterolinks.ini`)

Stored per-notebook at `notebooks/<id>/zoterolinks.ini` (matching Grimoire's backwards-compatible `weblinks.ini` / `doclinks.ini` sidecar architecture so older builds open notebooks untouched):

```ini
# Grimoire: Zotero reference links (docs/bridge-contract.md)
zref.<page_id>=<seed>,<x>,<y>,<w>,<h>,<item_key>,<attachment_key>,<page>,<locator_pct>,<short_cite_pct>[,<quote_pct>]
```

- Unknown lines are carried verbatim (capped at 16 KiB) and unreadable files set `writable = false` (never overwritten).
- `locator_pct`, `short_cite_pct`, and the optional 11th `quote_pct` use uppercase RFC 3986 `%XX` encoding for control bytes, `%`, `,`, `\r`, `\n`, spaces (safe single-line INI format supporting full UTF-8).
- Citing a Zotero item automatically surfaces a **Zotero Reference Node** in Grimoire's Index/Graph view linked to every citing essay page, and tapping either the page badge or the Graph Reference Node triggers the clean `42` handoff to Zotero.
