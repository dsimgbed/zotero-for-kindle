[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / GridBlock

# Interface: GridBlock

See [grid](grid.md). Needs app 0.12.0.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="children"></a> `children` | [`Child`](Child.md)[] | - |
| <a id="columns"></a> `columns?` | `number` \| `"auto"` | 1 to 12, or `"auto"` (default). |
| <a id="gap"></a> `gap?` | [`Gap`](Gap.md) | Default `"small"`. |
| <a id="kind"></a> `kind` | `"grid"` | - |
| <a id="minwidth"></a> `minWidth?` | `` `${number}%` `` | With `"auto"` columns, the narrowest a cell gets: `"5%"` to `"100%"`, default `"25%"`. |
| <a id="rows"></a> `rows?` | `number` | 1 to 20: draw only this many rows, never page. |
| <a id="width"></a> `width?` | [`Width`](Width.md) | - |
