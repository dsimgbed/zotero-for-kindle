[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / open

# Function: open()

```ts
function open(item, options?): Promise<void>;
```

Opens an item in the app's reader, as if the user had tapped it in the library: its
attachment (the one named by `options.attachment`, else the one the app would open) opens at
the reading place saved on this Kindle, downloading first if needed. Back returns to the
screen as it was, and the screen gets a `{ kind: "reader", closed: true }` event.

Only in answer to the user (a screen's `onEvent`, an action's `run`), like
[zk.reader.open](../reader/open.md); from a background task or a timer it throws, and nothing opens. The
promise resolves when the reader shows, and rejects with the reason when it can't: the item
is gone or unreadable (`"Item not found"`), the download failed, or the user cancelled it
(`"Cancelled"`). An unknown key never opens anything. Given as `{ key, library }`, the key is
looked up in that library; with `library` omitted (or a plain key) the app finds the item
by its key across the libraries it shows.

With `annotation` (an [Annotation](../Annotation.md)'s key, needs app 0.11.0) the reader opens the file
that annotation is in and goes to it, as a tap in the app's Notes does. An annotation that
is unknown, deleted, another item's, or not in the `attachment` given rejects with
`"Annotation not found"`; on an app before 0.11.0 the option is ignored and the item opens
where it was left.

**Permission**: `library:read`. Needs app 0.10.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `item` | \| `string` \| \{ `key`: `string`; `library?`: `number`; \} |
| `options?` | \{ `annotation?`: `string`; `attachment?`: `string`; \} |
| `options.annotation?` | `string` |
| `options.attachment?` | `string` |

## Returns

`Promise`\<`void`\>

## Examples

```ts
if (e.kind === "tap" && e.id === "continue") await zk.library.open(lastKey);
```

```ts
await zk.library.open({ key: item.key, library: item.libraryId }, { attachment: pdf.key });
```

```ts
// a highlight, tapped: the reader opens at it (app 0.11.0)
await zk.library.open(passage.itemKey, { annotation: passage.key });
```
