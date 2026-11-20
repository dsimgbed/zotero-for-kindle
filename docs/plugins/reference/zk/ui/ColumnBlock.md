[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / ColumnBlock

# Interface: ColumnBlock

See [column](column.md).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="children"></a> `children` | [`Child`](Child.md)[] | - |
| <a id="fit"></a> `fit?` | `"trim"` \| `"page"` | Only on the root of a Home body (a screen, or a column inside, refuses it). `"page"` (default): what doesn't fit is paged. `"trim"`: the children in order while they fit; the first that doesn't is trimmed if it can be (a grid to the rows that fit, a column to its children that fit, recursively) and dropped otherwise; everything after it is dropped; never a pager. Needs app 0.12.0. |
| <a id="gap"></a> `gap?` | [`Gap`](Gap.md) | Default `"small"`. |
| <a id="kind"></a> `kind` | `"column"` | - |
| <a id="width"></a> `width?` | [`Width`](Width.md) | - |
