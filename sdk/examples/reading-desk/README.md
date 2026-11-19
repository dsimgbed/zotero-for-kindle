# Reading Desk

A Home written by a plugin: the example for the app's `home: true` screens and the `items` block
(app 0.10.0). In Settings → Home, choose **Reading Desk — Reading Desk**.

## What it shows

- **BOOK · CONTINUE** — the book you opened last and haven't marked read (item type `book` or
  `bookSection`, or an EPUB file).
- **PAPER · CONTINUE** — the same for anything else.
- **PINNED** — items you pinned: long-press an item → **More…** → **Pin to Home / Unpin**. The
  action toggles and says which it did ("Pinned to Home" / "Unpinned"); an item already shown
  under Continue isn't repeated.
- **NEW IN INBOX (n)** — unread items of one collection, newest added first. Left out unless the
  `inbox` setting is set.

"Unread" is the read mark, never progress: a book at 100% stays under Continue until you mark
it read. Taps open the item in the reader; long-press shows the library's item menu. The app
draws the items and keeps their progress current; the plugin only picks the keys.

## Permissions

`library:read`, to find the items. Pins are kept in the plugin's own storage as
`{ key, library }[]`.

## Settings

`settings.json`, next to `plugin.json` (see `settings.example.json`):

```json
{ "inbox": "ABCD2345" }
```

- `inbox` — the key of the collection whose unread items are listed. Optional.

## Build

`node build.mjs`, or `sdk/check.sh` for all examples.
