# Grimoire Bridge

Send citations and quotes from **Zotero for Kindle** into **Grimoire**, and switch back to the essay you were writing.

The plugin adds a **Cite to Grimoire** action to the reader and to an item's details page, and a
**Grimoire Bridge** screen to the Plugins launcher.

## What it does

- **Cite a passage**: select text in a book or paper → the selection bar's **More…** → **Cite to
  Grimoire**. The citation (short form, e.g. `Kuhn 1962, p. 42`), the quoted passage (truncated to
  4000 characters) and the attachment are queued through the native `zk.bridge` into Grimoire's
  citation inbox, and the result pop-up offers **Open Grimoire**.
- **Cite an item**: from Library / Item Details → **More…** → **Cite to Grimoire**. No text
  selection needed; the item's short citation is queued.
- **Switch back**: **Switch to Grimoire** on the Grimoire Bridge screen (or **Open Grimoire** on a
  queued result) calls `zk.bridge.returnToGrimoire()` and the trampoline hands over to Grimoire,
  which opens the active essay page.

## Install (Kindle Scribe)

Copy the two files into the plugin folder on the device (connect the Kindle, or use
`/mnt/us/zotero/plugins/grimoire-bridge/`):

```sh
mkdir -p /mnt/us/zotero/plugins/grimoire-bridge
cp plugins/grimoire-bridge/plugin.json plugins/grimoire-bridge/main.js /mnt/us/zotero/plugins/grimoire-bridge/
chmod +x /mnt/us/zotero/plugins/grimoire-bridge/main.js
```

Only `plugin.json` and `main.js` belong in the folder; the app adds its own `plugin.log`,
`storage.db` and `data/` at run time.

Then turn the plugin on: **Settings → Plugins → Grimoire Bridge → Turn on**.

## Usage

1. Open a book or paper in Zotero for Kindle and read it.
2. Select a passage and choose **More…** → **Cite to Grimoire**, or open an item and choose
   **Cite to Grimoire** from its **Plugins** section.
3. When the citation is queued, tap **Open Grimoire** to hand over to the essay in progress, or use
   **Switch to Grimoire** on the Grimoire Bridge screen.

The Grimoire Bridge screen shows whether the bridge is available, the active Grimoire return
target (notebook and page, if set) and lets you refresh the library index or switch over.

## Permissions

- `library:read` — read the library items and attachments the citation is built from.
- `grimoire:bridge` — the native bridge to Grimoire (queue a citation, return to the essay).

No credentials are read or written by the plugin.

## Development

```sh
node --check plugins/grimoire-bridge/main.js
node plugins/grimoire-bridge/test_plugin.mjs
```
