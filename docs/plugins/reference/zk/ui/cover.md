[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / cover

# Function: cover()

```ts
function cover(item, opts?): CoverBlock;
```

An item's cover as the app draws it, 2:3, as wide as `width` says (default `"fill"`).
Needs app 0.12.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `item` | [`ItemRef`](ItemRef.md) |
| `opts?` | \{ `width?`: [`Width`](Width.md); \} |
| `opts.width?` | [`Width`](Width.md) |

## Returns

[`CoverBlock`](CoverBlock.md)

## Example

```ts
zk.ui.cover(book.key, { width: "25%" });
```
