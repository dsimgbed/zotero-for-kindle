[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / item

# Function: item()

```ts
function item(item, child): ItemBlock;
```

Makes any block an item: a tap opens it as the app opens items (download, the choice of a
file, the reader), a long press shows its item sheet, and while it downloads the app draws
the status in a small bar over the bottom of the block. An item that can't be read: the
block is drawn but does nothing. Needs app 0.12.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `item` | [`ItemRef`](ItemRef.md) |
| `child` | [`Block`](Block.md) |

## Returns

[`ItemBlock`](ItemBlock.md)

## Example

```ts
zk.ui.item(book.key, zk.ui.row([zk.ui.cover(book.key, { width: "20%" }), zk.ui.text(book.title)]));
```
