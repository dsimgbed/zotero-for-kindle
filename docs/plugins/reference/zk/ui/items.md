[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / items

# Function: items()

```ts
function items(items): ItemsBlock;
```

Library items drawn by the app: a `card` (title, authors, progress) or a `row` per item, or
a `shelf` of cover tiles, paged like the app's lists. The app resolves the keys itself, so the rows always show the
item's current title and reading progress, and repaints just this block when they change.
Items that are missing, in a switched-off group or unreadable are skipped silently; if all
are, `empty` shows (none: the block draws nothing).

**Taps and long-presses never reach the plugin**: a tap opens the item in the reader (the
screen then gets a `{ kind: "reader", closed: true }` event when it closes), a long-press
shows the library's own item menu. Use [zk.library.open](../library/open.md) to open one in answer to
your own button. Reading the items needs `library:read`. Needs app 0.10.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `items` | `Omit`\<[`ItemsBlock`](ItemsBlock.md), `"kind"`\> |

## Returns

[`ItemsBlock`](ItemsBlock.md)

## Examples

```ts
zk.ui.items({ id: "recent", style: "row", items: keys, empty: "Nothing opened yet" });
```

```ts
// an item from a group library, with its own label
zk.ui.items({ id: "pick", style: "card", items: [{ key: "ABCD2345", library: 12, label: "Up next" }] });
```

```ts
// the books being read, as covers (needs app 0.11.0)
zk.ui.items({ id: "shelf", style: "shelf", items: reading.map((i) => i.key), empty: "Nothing on the shelf" });
```
