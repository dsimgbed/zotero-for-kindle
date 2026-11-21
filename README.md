<h1 align="center">Zotero for Kindle</h1>

<p align="center">
  Your Zotero library on a jailbroken Kindle.<br>
  Read your PDFs and EPUBs, highlight and annotate, and everything syncs back to Zotero.
</p>

<p align="center">
  <a href="https://github.com/ievlevpn/zotero-for-kindle/releases/latest">Download</a> ·
  <a href="INSTALL.md">Install</a> ·
  <a href="docs/plugins/using.md">Plugins</a>
</p>

<table align="center">
  <tr>
    <td align="center"><img src="docs/screenshots/home.png" width="190" alt="Home"><br><sub>Home</sub></td>
    <td align="center"><img src="docs/screenshots/reader.png" width="190" alt="Reading, with a highlight"><br><sub>Reading</sub></td>
    <td align="center"><img src="docs/screenshots/reader-menus.png" width="190" alt="The reading menu"><br><sub>Reading menu</sub></td>
    <td align="center"><img src="docs/screenshots/notes.png" width="190" alt="Notes"><br><sub>Highlights and notes</sub></td>
  </tr>
  <tr>
    <td align="center" colspan="4"><img src="docs/screenshots/scribe-landscape.png" width="800" alt="Kindle Scribe, landscape, two columns"><br><sub>Kindle Scribe, landscape, two columns</sub></td>
  </tr>
</table>

> **Unofficial.** A personal project, built for my own use; not made or supported by Zotero or Amazon. It works on my
> Kindles — it may not work on yours.

## What it does

**Your library, on the Kindle.** Items, collections and tags sync from zotero.org. Browse, search the whole library
or one collection, pin collections, mark books read (a Zotero tag of your choice). Books download on demand from Zotero
storage or WebDAV — or keep a whole collection downloaded, and new papers in it arrive by themselves.

**A reader made for e-ink.** EPUB, PDF and the web pages Zotero saved (snapshots), with fonts, sizes and margins,
PDF margin trimming, landscape with two columns on big screens, footnotes in a pop-up, bookmarks, *Go to page*, and
Fast reading fonts for EPUBs.

**Annotations that go both ways.** Highlights, underlines and comments; on PDFs also **areas** (a box around a figure,
a formula or a table) and **sticky notes** in the margin. They appear in Zotero desktop, and Zotero's appear here.

**Write with the Scribe's pen.** On a Kindle Scribe the pen writes straight onto PDF pages; strokes become Zotero ink
annotations. The eraser end takes them away, and a palm resting on the screen is ignored. Contributed by Anna
Smirnova — details in [docs/scribe-pen.md](docs/scribe-pen.md).

**Plugins.** Small JavaScript plugins add what doesn't belong in the app itself — an action on selected text, a
screen of their own, work after a sync. *Settings → Plugins → Get plugins* lists the catalog; the first plugin is
[Feed Riffle](https://github.com/ievlevpn/zk-feed-riffle), which deals the new papers from your Zotero feeds as cards
to keep or discard. [More below](#plugins).

**Up to three Zotero accounts**, each with its own library, downloads and plugin data, and an optional PIN.
[More below](#accounts).

**Easy on the battery.** The app rests while you read, never turns Wi-Fi on by itself, and updates itself from
*Settings → Check for updates* (signed releases; a version that doesn't start is rolled back).

## Requirements

- **A jailbroken Kindle** that can run scriptlets (`.sh` files in `documents/` show up in the library); most
  jailbreaks provide this through the Universal Hotfix — see [kindlemodding.org](https://kindlemodding.org).
  Tested on a Paperwhite 5 (firmware 5.18.4) and a Kindle Scribe (5.16.8).
- **A Zotero account and an API key** from [zotero.org/settings/keys](https://www.zotero.org/settings/keys).
- **Your files in Zotero storage or on WebDAV** — where Zotero desktop syncs them (*Settings → Sync → File Syncing*).

## Install

The easiest way is with an AI agent: connect the Kindle by USB, then give Claude Code, Codex or a similar agent this
prompt:

> Install Zotero for Kindle on my Kindle by following
> https://github.com/ievlevpn/zotero-for-kindle/blob/master/INSTALL-WITH-AI.md

To do it yourself, see [INSTALL.md](INSTALL.md) — it amounts to copying two folders over USB. After that, updates come
from *Settings → Check for updates* on the Kindle.

## Accounts

One Kindle can hold up to three Zotero accounts, for a family or a shared device. With one account nothing looks
different: *Settings → Accounts* shows it, with *Add account*.

- **Add account** opens the same screen as the first start: a user ID and API key (and WebDAV, if that account
  uses it). Each Zotero user can be on the Kindle once. You can also add one over USB with a `config.json`
  ([INSTALL.md](INSTALL.md#or-put-the-account-in-a-file)).
- **Switching.** With two or more accounts, Home's header shows the open one; tap it to switch. Each account keeps
  its own library, downloads, reading positions and plugin data; only the open account syncs. Changes not yet sent
  go first (up to 15 seconds, or *Skip*: they wait in that account for its next sync).
- **PIN** (optional): in *Settings → Accounts*, the account's *Set PIN…*, 4 to 6 digits. It is asked when you switch to the account,
  when the app starts with it, and when the Kindle wakes up (*Lock on sleep*). After five wrong tries the PIN pad
  waits 30 seconds, then twice as long after each further miss (up to 30 minutes). A PIN locks the app's screens;
  the account's books and library sit on the Kindle's USB storage and can be read from a computer. Its API key
  can't: it is kept in the Kindle's protected system storage.
- **Forgot the PIN?** Tap *Forgot PIN?* on the PIN pad. Connect the Kindle by USB and put a text file named
  `reset-pin.txt` into the `zotero` folder, containing that account's Zotero API key (from
  [zotero.org/settings/keys](https://www.zotero.org/settings/keys)); eject, then tap *Check reset-pin.txt*. Or type
  the key on the Kindle. If the key matches, you can set a new PIN or remove it. The file is deleted once read,
  whether it matched or not. If you made a new key on zotero.org since, that one works too (with Wi-Fi).
- **Update key** takes a new API key or WebDAV details for the same Zotero user. A key of another user is refused:
  that's *Add account*.
- **Remove from this Kindle** deletes the account's library, downloads and plugin data from the Kindle — nothing in
  Zotero itself. It shows what it deletes and how many changes haven't been sent to Zotero. For the open account
  (with Wi-Fi) it offers *Send now* first; for another account, switch to it first to send them, or removing loses
  them. A locked account can be removed without its PIN, after a warning.
- If the account list on the Kindle (`accounts.json`) ever can't be read, the app keeps it aside
  (`accounts.json.bad-<time>`) and lists the accounts again from their folders. A PIN they had can't be known then,
  so each opens once with its API key, as with *Forgot PIN?*; set a PIN again afterwards.

## Plugins

A plugin can add an action to the reader's selection bar or to an item's page, open screens of its own (drawn by the
app, in its own style), and run in the background after a sync. Get one from *Settings → Plugins → Get plugins*, or
copy its folder into `zotero/plugins/` over USB. A plugin can also fill the body of Home (*Settings → Home*), drawing
your items with their reading progress; the app keeps the frame and falls back to its own body if the plugin fails.

<p align="center"><img src="docs/screenshots/reading-desk.png" width="300" alt="Reading Desk, a Home written by a plugin"><br>
<sub><a href="https://github.com/ievlevpn/zk-reading-desk">Reading Desk</a> 0.3: the book and the paper you're in, a row
of pins, what's new in an inbox (or a passage from your highlights), and Your desk one tap away — built only from the
app's primitives (<a href="docs/plugins/composing.md">Composing screens</a>)</sub></p>

They're kept on a short leash:

- **Off until you say so** — a plugin stays off until you've read what it may do and turned it on.
- **Changes go through the app** — it checks every change, and nothing is removed without asking you each time.
- **Signed** — catalog plugins by their authors, the catalog itself by the app.
- **Easy to turn off** — one that keeps failing turns itself off; a file named `OFF` in the plugins folder turns them
  all off.

For people writing plugins: [Using plugins](docs/plugins/using.md) ·
[Getting started](docs/plugins/getting-started.md) · [API reference](docs/plugins/reference/README.md) ·
[Publishing](docs/plugins/publishing.md) · examples in [sdk/examples](sdk/examples).

## Building from source

C++20 with cairo/pango, crengine (EPUB), MuPDF (PDF) and FBInk, built with Meson.

    meson setup build-desktop && meson compile -C build-desktop   # desktop build (macOS)
    ./build-desktop/tests/zk-tests                                # the tests
    ./build-desktop/zotero --device pw5                           # try it in a window (or --device scribe)
    tools/build-kindle.sh                                         # cross-build in the zk-kindlehf container
    tools/package.sh                                              # dist/zotero-for-kindle.zip
    tools/deploy.sh                                               # copy to a Kindle over USBNetwork (SSH)
    tools/release.sh 0.7.0 notes.md                               # a signed release (needs the signing key)

`tools/screenshots.sh` regenerates the screenshots above from a demo library of public-domain books.

## Licence

AGPL-3.0-or-later. An unofficial project for personal use, with no affiliation to Zotero or Amazon. The Fast reading
fonts are [Fast Font](https://github.com/chelaxian/Fast-Font) by Born2Root (MIT, see `assets/fonts-fast/`). The plugin
runtime embeds [QuickJS-ng](https://github.com/quickjs-ng/quickjs) (MIT, see `third_party/quickjs-ng/LICENSE`).
Zotero is a trademark of the Corporation for Digital Scholarship. Kindle is a trademark of Amazon.
