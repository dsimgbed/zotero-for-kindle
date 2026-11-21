# Installing Zotero for Kindle by hand

You need a jailbroken Kindle that runs scriptlets (see the [README](README.md#requirements)) and a USB cable.
This takes about five minutes.

## 1. Download

Download **[zotero-for-kindle.zip](https://github.com/ievlevpn/zotero-for-kindle/releases/latest/download/zotero-for-kindle.zip)**
and unzip it. You get two folders, `zotero` and `documents`.

## 2. Copy them onto the Kindle

Connect the Kindle by USB.

- **It shows up as a drive** (Windows, Linux, older Kindles on a Mac): open it.
- **It doesn't show up on a Mac** (newer Kindles, such as the Paperwhite 5 and the Scribe, use MTP): install
  [OpenMTP](https://openmtp.ganeshrvel.com) and open the Kindle there. Quit Android File Transfer and Image Capture
  first if they're running.

Copy both folders into the top level of the Kindle, next to its existing `documents` folder.
When asked, **merge** them: don't replace the Kindle's `documents` folder, only add `Zotero.sh` to it.

You end up with:

    Kindle/
      zotero/            the app
      documents/
        Zotero.sh        its launcher (other books stay as they were)

Eject the Kindle and unplug it.

## 3. Start it and sign in

Open the Kindle library and tap **Zotero**. It has a Zotero cover. The first screen asks for:

- **Zotero user ID and API key.** Create a key at [zotero.org/settings/keys](https://www.zotero.org/settings/keys)
  with *Allow library access* and *Allow write access*. Write access lets highlights made on the Kindle go back to
  Zotero. Your user ID is shown at the top of that page ("Your user ID for use in API calls is …").
- **WebDAV URL, user and password**, only if Zotero desktop syncs your files with WebDAV (*Settings → Sync → File
  Syncing*), copied from there. With Zotero storage, the default, leave these empty.
  The address can be entered either with or without the trailing `zotero/`.

Tap **Connect**. The first sync of a big library takes a few minutes.

### Or put the account in a file

To avoid typing on the Kindle, save this as `zotero/config.json` on the Kindle before step 3:

Leave out the three `webdav` lines if you use Zotero storage.

```json
{
  "userId": 1234567,
  "apiKey": "your API key",
  "webdavUrl": "https://dav.example.com/zotero/",
  "webdavUser": "your WebDAV user",
  "webdavPassword": "your WebDAV password"
}
```

The app reads it at the next start and stores it in a protected place inside the Kindle's system storage. It then
deletes the file from the USB drive.

The same file works later too:

- **For an account already on the Kindle** (the same `userId`), it replaces that account's key and WebDAV details
  once Zotero confirms, with Wi-Fi on, that the key is that user's. Until then nothing changes and the file stays;
  a key of another user is not used.
- **For another Zotero user**, it adds an account (up to three; see [Accounts](README.md#accounts)). The account
  you have open stays open; the new one is in *Settings → Accounts* and on Home's account switch.
- **With three accounts already**, the file is left on the USB drive and nothing changes; the app says so. Remove
  an account in *Settings → Accounts* first.

### Forgot an account's PIN

Put a text file named `reset-pin.txt` into the `zotero` folder over USB, containing that account's Zotero API key,
then tap *Forgot PIN?* → *Check reset-pin.txt* on the PIN pad. See [Accounts](README.md#accounts).

## Updating

From 0.2.0 on, open *Settings → Check for updates* on the Kindle. The app shows what's new, downloads the update,
checks its signature and restarts into it. If the new version doesn't start, it goes back to the one you had.
The new version counts as started after a minute on screen (or when you quit it); if the Kindle is switched off or
restarted before that, it goes back too, and you can install the update again.

Before installing, the app backs up the account list and every account's library (not the downloaded books); if
the Kindle is short of space, only the open account's. A rollback puts them back.

From 0.1.0, or to update by hand: copy the new `zotero` and `documents` folders the same way, merging and replacing
files. Your library, downloads and accounts stay.

From 0.13.0 on, each account's data is in `zotero/accounts/<userID>/` (the app moves an older single library there
at its first start). Going back by hand to a version before 0.13.0 after that isn't supported: it would find no
library and ask to sign in again. (An update that doesn't start is still rolled back safely.)

## Uninstalling

Delete the `zotero` folder and `documents/Zotero.sh` (and `documents/Zotero-no-plugins.sh`, if an older version left it).

## If something goes wrong

- **No Zotero item in the library:** the jailbreak doesn't run scriptlets. Install the Universal Hotfix and restart the Kindle.
- **The app stops responding:** it closes itself within 15 seconds and returns to the Kindle home. Then start it
  again.
- **Anything else:** the log is in `zotero/log/zotero.log` on the USB drive. It never contains passwords or keys.
  Attach it to a [GitHub issue](https://github.com/ievlevpn/zotero-for-kindle/issues).
