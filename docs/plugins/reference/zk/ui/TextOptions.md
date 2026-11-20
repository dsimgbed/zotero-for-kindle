[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / TextOptions

# Interface: TextOptions

Options of [text](text.md). Needs app 0.12.0 except `style` and `lines`.

## Extended by

- [`TextBlock`](TextBlock.md)

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="align"></a> `align?` | `"center"` \| `"start"` \| `"end"` | Where its lines sit in its width. Default `"start"`. |
| <a id="caps"></a> `caps?` | `boolean` | Uppercase and spaced out: a section label. |
| <a id="font"></a> `font?` | `"serif"` \| `"sans"` | - |
| <a id="id"></a> `id?` | `string` | Makes it tappable (a link): a tap sends a `tap` event with this id. |
| <a id="italic"></a> `italic?` | `boolean` | - |
| <a id="lines"></a> `lines?` | `number` | At most this many lines, ending with "…". |
| <a id="size"></a> `size?` | `"small"` \| `"xsmall"` \| `"large"` \| `"title"` \| `"body"` | - |
| <a id="style"></a> `style?` | [`TextStyle`](TextStyle.md) | A preset of font, size, weight and tone; the options below override it. Default `"body"`. |
| <a id="tone"></a> `tone?` | `"ink"` \| `"secondary"` \| `"inverse"` | `"inverse"` is white. Unset inside an `"ink"` box: white; elsewhere the style's. |
| <a id="weight"></a> `weight?` | `"bold"` \| `"medium"` \| `"regular"` | - |
| <a id="width"></a> `width?` | [`Width`](Width.md) | - |
