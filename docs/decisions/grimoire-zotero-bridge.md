# Architecture Decision & Deviation Ledger: Grimoire × Zotero Bridge

**Workspace:** `/home/ubuntu/grimoire-zotero-workspace`  
**Contract:** [docs/bridge-contract.md](/home/ubuntu/grimoire-zotero-workspace/docs/bridge-contract.md) (`schema_version: 1`)  
**Target Platform:** Jailbroken Amazon Kindle Scribe (e.g., firmware `5.20.1`)

---

## 1. Architecture Summary

The **Grimoire × Zotero Bridge** connects two independent native Kindle Scribe applications—`grimoire-scribe` (Rust, `/dev/fb0` FBInk + raw `evdev` stylus/touch) and `zotero-for-kindle` (C++20 + QuickJS-ng, X11 `DISPLAY=:0` + Kindle wet-ink overlay)—into one dual-engine research-and-writing workspace without sharing a display server, process address space, or credentials.

### Core Mechanism
1. **Serialized Trampoline Launcher (`bridge/GrimoireZotero.sh`):**
   - POSIX `sh` / BusyBox `ash`-compatible supervisor with atomic `mkdir` single-instance locking and stale-PID recovery.
   - Orchestrates clean sequential handoffs via exit codes:
     - `42`: Grimoire saved its notebook, flushed/closed display & input descriptors, wrote `handoff.json` (`target: "zotero"`), and requests Zotero.
     - `43`: Zotero closed reader/UI/plugins cleanly (skipping `--reset-system-ink` in `Zotero.sh`), wrote `handoff.json` (`target: "grimoire"`), and requests Grimoire.
     - `75`: Zotero self-update restart (re-executes Zotero directly).
     - `0`: User exited cleanly; trampoline releases lock and returns to Kindle UI.
   - Enforces crash-streak and rapid-bounce circuit breakers, clearing `handoff.json`, restoring Kindle UI (`awesome`, `volumd`, `lipc` pillow), and removing `/tmp/zk-wet-ink-session` on trip.
2. **File-Based Atomic IPC (`$GZ_BRIDGE_DIR`, default `/mnt/us/grimoire_zotero_bridge`):**
   - `handoff.json`: Single-shot workspace switch consumed and unlinked on startup by the target app.
   - `return_context.json`: Persists the last Grimoire essay `(notebook_id, page_id, page_index)` so Zotero can always return to the exact page.
   - `inbox/<epoch_ms>-<nonce>.json`: Append-safe per-citation event files queued by Zotero's bundled `grimoire-bridge` QuickJS plugin and drained atomically by Grimoire.
   - `library_index.json`: Non-sensitive read-only catalog snapshot (`item_key`, `attachment_key`, `citation_key`, `title`, `authors`, `year`, `item_type`, `collections`, `tags`, `has_pdf`/`has_epub`/`has_snapshot`) exported by Zotero for offline search in Grimoire.
   - `bridge.log`: Fixed, bounded (`<= 64 KiB`), non-sensitive diagnostic lines.
3. **Notebook Sidecar & Knowledge Graph (`notebooks/<id>/zoterolinks.ini`):**
   - Grimoire stores citation anchors in an isolated sidecar file (preserving 100% backwards compatibility with `links.ini` and standalone Grimoire builds).
   - Reference nodes appear in Grimoire's Index/Graph (`mapview.rs`) using `z:<item_key>[:<attachment_key>[:<page>]]` navigation targets, allowing single-tap jumps directly to the cited source page in Zotero.
4. **In-Editor Zotero Sheet & Citation Badges (`zotero_sheet.rs`, `linkbar.rs`, `editor.rs`):**
   - Adds a compact `"Zotero"` pill to the Links bar (visible only when the bridge directory exists).
   - Opens an e-ink sheet with **Inbox** (pending citations from Zotero) and **Catalog** (searchable `library_index.json`), supporting both inserting a citation badge onto the current page and jumping directly to Zotero.

---

## 2. Design & Deviation Ledger

| Starting Hypothesis | Final Implementation | Why We Changed It | Trade-offs / Follow-ups |
|---|---|---|---|
| **Single shared launcher stops Kindle UI (`awesome`/`pillow`) once across both apps.** | `bridge/GrimoireZotero.sh` delegates display lifecycle to each app's own launcher: `grimoire.sh` pauses/restores `awesome`/`volumd`/`pillow` around FBInk, while `Zotero.sh` runs under X11 (`DISPLAY=:0`) and skips `--reset-system-ink` on exit `43`. | Codebase audit revealed a fundamental display-stack asymmetry: `grimoire-scribe` renders directly to `/dev/fb0` and must pause `awesome`, whereas `zotero-for-kindle` is an X11 application that requires `awesome`/X11 alive to render and receive input. Holding `awesome` paused across Zotero would hang Zotero immediately. | Adds ~1 s configurable settle delay (`GZ_SWITCH_SETTLE_SEC=1` on Kindle) between app exits; avoids framebuffer/X11 contention completely and preserves 100% standalone app behavior. |
| **Single mutable `citation_inbox.json` array appended by Zotero and truncated by Grimoire.** | Append-safe per-event files `inbox/<epoch_ms>-<nonce>.json` written atomically (`O_EXCL\|O_NOFOLLOW` temp + `fsync` + `rename`) and unlinked individually after consumption. | A shared mutable array risks partial-write corruption or lost citations across crashes/restarts on FAT/FUSE `/mnt/us`. Per-event files make queuing idempotent, lock-free, and naturally ordered lexicographically. | Slight inode overhead (bounded by small pending queue); `grimoire_bridge` retains `LEGACY_INBOX_FILE` constant only for safety/path awareness without mutating shared arrays. |
| **Store Zotero links directly inside Grimoire's existing `links.ini`.** | Isolated per-notebook sidecar `notebooks/<id>/zoterolinks.ini` with uppercase `%XX` escaping for single-line fields and strict bounds (`ZOTEROLINKS_MAX=4000`, `1 MiB` cap). | Modifying `links.ini` syntax risked breaking older/standalone Grimoire versions or corrupting existing notebook links. A dedicated sidecar keeps standalone Grimoire completely untouched and lets `mapview.rs` refresh Zotero anchors cleanly via generation counters. | Sidecar must be carried alongside notebook folders if users manually copy individual files instead of whole notebook directories. |
| **Add `serde`/`serde_json` to `grimoire-scribe` for bridge JSON parsing.** | Implemented zero-dependency bounded JSON serialization and parsing on top of Grimoire's existing `json_min.rs` tokenizer (with dynamic token vector sizing up to 6M tokens and a dedicated `ju32_opt` unsigned 32-bit parser). | `grimoire-scribe` enforces a strict zero-new-crates supply-chain policy (`Cargo.toml`, `Cargo.lock`, `deny.toml` untouched). Reusing `json_min.rs` avoided adding dependencies while keeping binary size and attack surface minimal. | Custom serializer/parser requires explicit cross-language wire-format tests; Wave 3 adversarial review caught signed `i32` clamping (`jint`) and off-by-one `clean_text` truncation, both now fixed with regression tests. |
| **JS plugin and C++ QuickJS binding built independently from the contract doc alone would interoperate cleanly.** | Aligned `bindings_bridge.cpp` and `plugins/grimoire-bridge/main.js` and added an end-to-end QuickJS-ng test in `tests/test_grimoire_bridge.cpp` that executes the actual bundled `main.js`. | Post-Wave-1 integration audit caught 5 JS↔C++ runtime mismatches (`available()` method vs property, `{ok, id}` return shape from `sendCitation`, snake_case `{notebook_id, page_id, page_index}` from `getReturnTarget`, `{item_count}` + `library:read` gate in `refreshIndex`, and `year` stringification + avoiding disk I/O inside `render()`). | Bundled QuickJS integration test guarantees future plugin/binding drift breaks CI immediately. |
| **Always emit `return_to` in `writeReturnToGrimoireHandoff` and fail Rust `handoff_validate` whenever `return_to` has an empty `notebook_id`.** | C++ omits `return_to` when no prior return context exists and rejects empty `writeReturnContext`; Rust ignores an empty `return_to` in `handoff_parse` and removes the faulty `grimoire.notebook_id` coupling from `handoff_validate`. | Wave 3 independent adversarial review caught a CRITICAL interop defect: launching Zotero directly (no prior Grimoire session) and tapping "Return to Grimoire" wrote `"return_to":{"notebook_id":""}`, which Rust rejected and deleted. | Both direct Zotero→Grimoire launches and round-trip Grimoire→Zotero→Grimoire returns now validate identically on both engines. |
| **Flush editor strokes on shutdown only when `a.scr == ScreenId::Editor`.** | Updated `app_shutdown` (`app.rs`) to call `editor_save_now(a)` whenever `a.ed.is_some()` (internally guarded by `s.open`). | Wave 3 adversarial review noted that a handoff exit `42` triggered from outside the Editor screen (e.g., Links/Graph overlay or alternate screen while a notebook remains open) could skip flushing pending strokes. | Zero-cost safety net since `save_page_for` is a no-op when `!s.open || !s.page_dirty`. |
| **Desktop Meson test build requires SDL2 for `zotero-for-kindle` target.** | Made desktop `sdl2` dependency optional (`required: false`) in `zotero-for-kindle/meson.build` so headless Linux hosts without `libsdl2-dev` can build `libzk-bridge.a` and run `zk-tests` without modifying Kindle cross-build targets. | Build environment lacks `libsdl2-dev` and desktop engine `.so`s (`libzk-epub.so`, `libzk-pdf.so`). Gating only the desktop SDL2 simulator executable unblocked full native C++20 compilation and doctest + QuickJS-ng execution. | Desktop visual simulator binary is skipped only when `sdl2` is absent; Kindle ARM/hf target and full `zk-tests` binary build normally. |
| **Place Grimoire citation badges near top-left page chrome.** | Positioned citation badges clear of the left tool panel and top-right clock/status corner in `editor.rs`. | Track E simulator tests revealed tap hit-testing overlaps with the left editor panel and status chrome, blocking badge tap jumps to Zotero. | Keeps badges reliably tappable with finger or stylus across orientations; future polish can add custom drag-to-reposition for badges. |

---

## 3. Hard Safety & Security Invariants Audit

1. **Zero Display / Input Contention:**
   - Grimoire exits `42` only after `app_shutdown` → `display_wait` → `input_close` → `display_close` (`grimoire-scribe/rust/src/mainloop.rs`).
   - Zotero exits `43` only after `app.closeAll()`, `switcher.close()`, `plugins->stop()` (`zotero-for-kindle/src/app/main.cpp`), and `assets/launcher/Zotero.sh` skips `--reset-system-ink` on `43`.
2. **Never Strand Kindle UI:**
   - `bridge/GrimoireZotero.sh` circuit breaker handles crash loops (`GZ_MAX_CRASHES=2`) and bounce storms (`GZ_MAX_RAPID_BOUNCES=4` within `GZ_RAPID_THRESHOLD_SEC=3`), unlinks `handoff.json`, removes `/tmp/zk-wet-ink-session`, and resumes `awesome`, `volumd`, and `lipc` pillow.
3. **Zero Credential Leakage:**
   - Verified across `grimoire_bridge.cpp`, `bindings_bridge.cpp`, `main.js`, `zotero_bridge.rs`, and `GrimoireZotero.sh`: no Zotero API keys, WebDAV passwords, `config.json`, `accounts.json`, or PINs are read or exported into `library_index.json`, `handoff.json`, `inbox/*.json`, or `bridge.log`.
4. **Strict Validation & Path Safety:**
   - Every bridge read/write uses `O_NOFOLLOW`, atomic `O_EXCL` temp + `fsync` + `rename`, full path-component symlink rejection (`checkComponents` in C++ and `ensure_dir` in Rust), UTF-8 / NUL / control-byte filtering, strict regex-equivalent key/ID validation, and bounded file sizes (`256 KiB` IPC JSON, `10 MiB` catalog, `64 KiB` log).
   - `bridge/GrimoireZotero.sh` never evaluates untrusted JSON (`eval`/`source`/`sh -c` absent) and deletes symlink or oversized `handoff.json` files before launching apps.
5. **100% Standalone Backwards Compatibility:**
   - When `/mnt/us/grimoire_zotero_bridge` is absent (and `GZ_BRIDGE_DIR` is unset), Grimoire hides the `"Zotero"` Links-bar pill and treats all bridge operations as no-ops; Zotero disables bridge plugin actions cleanly (`bridge.available() === false`).

---

## 4. Kindle Scribe Firmware `5.20.1` On-Device Verification Checklist

### Deployment Layout on `/mnt/us`
- Grimoire Scribe installed at `/mnt/us/grimoire/` (with `/mnt/us/grimoire/app/grimoire.sh` and updated Rust binary).
- Zotero for Kindle installed at `/mnt/us/zotero/` (via `zotero-for-kindle/tools/package.sh`, which now bundles `plugins/grimoire-bridge/{plugin.json,main.js}`) and launcher at `/mnt/us/documents/Zotero.sh`.
- Trampoline launcher copied to `/mnt/us/documents/GrimoireZotero.sh` (or invoked via KUAL menu entry) with `chmod +x`.
- Bridge state directory created at `/mnt/us/grimoire_zotero_bridge` (auto-created on first `GrimoireZotero.sh` run).

### Step-by-Step Verification Protocol
1. **Standalone Regression (No Bridge Dir):**
   - Temporarily rename `/mnt/us/grimoire_zotero_bridge` if present.
   - Launch Grimoire standalone (`/mnt/us/grimoire/app/grimoire.sh`): confirm no `"Zotero"` button in Links bar, normal pen writing/erasing/sleep, and clean exit back to Kindle Home.
   - Launch Zotero standalone (`/mnt/us/documents/Zotero.sh`): confirm `grimoire-bridge` plugin shows unavailable / no-op without errors, wet ink works on PDFs, and normal exit restores Kindle UI.
2. **Trampoline Launch & Library Index Export:**
   - Launch `GrimoireZotero.sh --start zotero`.
   - Open a Zotero PDF item; verify `/mnt/us/grimoire_zotero_bridge/library_index.json` is written atomically and contains **zero** `api_key`, `password`, or `webdav` strings (`grep -iE 'api_key|password|token|webdav' /mnt/us/grimoire_zotero_bridge/library_index.json` returns empty).
3. **Reader → Notebook Citation Capture & Return (`exit 43`):**
   - In Zotero, select a passage (or open a page menu) and tap **"Cite / Send to Grimoire"**. Confirm a new `/mnt/us/grimoire_zotero_bridge/inbox/<epoch>-<nonce>.json` file appears.
   - Tap **"Return to Grimoire"** (note: even without a prior Grimoire session, confirm Grimoire launches cleanly with no framebuffer artifacts and stylus responds immediately).
   - Open an essay notebook, draw a stroke, open the Links bar → tap **"Zotero"** → **Inbox** tab → tap the queued citation to insert a citation badge onto the page. Verify `notebooks/<id>/zoterolinks.ini` is created and the inbox file is consumed/removed.
4. **Notebook → Reader Jump & Exact-Page Round-Trip (`exit 42` → `exit 43`):**
   - Draw one new stroke on page 2 of the essay (do not manually save), then tap the inserted citation badge (or tap a Zotero Reference Node in the Index/Graph view).
   - Verify Grimoire flushes the stroke, closes FBInk cleanly (`exit 42`), and Zotero opens directly to the exact cited PDF attachment and page with wet ink responsive.
   - Tap **"Return to Grimoire"** in Zotero (`exit 43`). Verify Grimoire reopens directly to the exact essay and page 2, with the pre-jump stroke intact and zero ghosting or pen lockup.
5. **Failure & Circuit-Breaker Recovery:**
   - Test a corrupted/oversized `handoff.json` (or symlink) in `/mnt/us/grimoire_zotero_bridge/handoff.json` before launching `GrimoireZotero.sh`: confirm the trampoline removes it, logs to `bridge.log`, and opens Grimoire cleanly.
   - Verify exiting Grimoire normally (`exit 0`) terminates `GrimoireZotero.sh`, releases `/tmp/grimoire_zotero_trampoline.lock`, and leaves Kindle Home / `awesome` / `pillow` fully responsive.
