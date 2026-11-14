[Plugin API v0](../../README.md) / [zk](../../zk.md) / [reader](../reader.md) / open

# Function: open()

```ts
function open(doc): Promise<void>;
```

Opens a document in the app's reader as a **preview**: nothing is added to the library.

- `url`: a PDF or EPUB to download (https only, and the plugin needs `net` permission for
  its host, as for [zk.fetch](../fetch.md)). The type comes from the response's Content-Type, then
  from the file's first bytes; a page that turns out to be HTML opens as `html`. A download
  screen shows the progress with **Cancel**. At most 50 MB.
- `html`: a page the plugin already has (an article it extracted), opened like a saved web
  page. No scripts run; images aren't fetched (`data:` URLs show). `baseUrl` resolves its
  relative links.

In a preview there is no highlighting, notes or bookmarks: a selection offers Search and
plugin actions (their [TextSelection](../TextSelection.md) has `item: null` and `preview`). The top bar says
"Preview". Nothing syncs. The app keeps the last 10 previewed documents (at most 100 MB) with
their reading places, so opening one again is instant.

Only in answer to the user (a screen's `onEvent`, an action's `run`), like [zk.ui.open](../ui/open.md);
elsewhere, or with a URL that isn't https or whose host isn't declared, it throws. The
promise resolves when the reader shows, and rejects if the download fails or the user
cancels it; waiting doesn't count against the call's time. Back returns to the screen as it
was, and the screen gets a `{ kind: "reader", closed: true }` event.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `doc` | \| \{ `title?`: `string`; `url`: `string`; \} \| \{ `baseUrl?`: `string`; `html`: `string`; `title`: `string`; \} |

## Returns

`Promise`\<`void`\>

## Examples

```ts
if (e.kind === "tap" && e.id === "card") await zk.reader.open({ url: `https://arxiv.org/pdf/${card.arxiv}`, title: card.title });
```

```ts
await zk.reader.open({ html: article, title: card.title, baseUrl: card.link });
```
