[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / grid

# Function: grid()

```ts
function grid(children, opts?): GridBlock;
```

Cells in rows. `columns`: a number from 1 to 12, or `"auto"` (the default): as many as fit
with each at least `minWidth` (5% to 100% of the grid, default `"25%"`). It pages by rows
like the app's lists, unless `rows` (1 to 20) is given: then only that many rows are drawn,
the rest are not, and it never pages. Needs app 0.12.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `children` | [`Child`](Child.md)[] |
| `opts?` | \{ `columns?`: `number` \| `"auto"`; `gap?`: [`Gap`](Gap.md); `minWidth?`: `` `${number}%` ``; `rows?`: `number`; `width?`: [`Width`](Width.md); \} |
| `opts.columns?` | `number` \| `"auto"` |
| `opts.gap?` | [`Gap`](Gap.md) |
| `opts.minWidth?` | `` `${number}%` `` |
| `opts.rows?` | `number` |
| `opts.width?` | [`Width`](Width.md) |

## Returns

[`GridBlock`](GridBlock.md)

## Example

```ts
zk.ui.grid(books.map((b) => zk.ui.item(b.key, zk.ui.cover(b.key))), { columns: 4, rows: 1 });
```
