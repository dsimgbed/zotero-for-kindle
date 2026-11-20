[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / row

# Function: row()

```ts
function row(children, opts?): RowBlock;
```

Places blocks side by side (e.g. a label and a button), up to 12. Wraps nothing: keep it
to a few short blocks. Each child takes what its `width` says (see [Width](Width.md)); the
rest of the width is shared equally by the children that `fill` (the default; buttons are
`"auto"`). `align` places the children vertically when they differ in height; with
`"stretch"` each is as tall as the row, and a [spacer](spacer.md) in a stretched column takes
the leftover height. `align` and the 12 blocks need app 0.12.0 (older apps: 6 blocks).

## Parameters

| Parameter | Type |
| ------ | ------ |
| `children` | [`Child`](Child.md)[] |
| `opts?` | \{ `align?`: [`RowAlign`](RowAlign.md); `gap?`: [`Gap`](Gap.md); `width?`: [`Width`](Width.md); \} |
| `opts.align?` | [`RowAlign`](RowAlign.md) |
| `opts.gap?` | [`Gap`](Gap.md) |
| `opts.width?` | [`Width`](Width.md) |

## Returns

[`RowBlock`](RowBlock.md)

## Example

```ts
zk.ui.row([zk.ui.text("math.PR · 12 left", { style: "label" }), zk.ui.button({ id: "feeds", title: "Feeds" })]);
```
