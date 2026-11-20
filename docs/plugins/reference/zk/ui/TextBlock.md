[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / TextBlock

# Interface: TextBlock

See [text](text.md).

## Extends

- [`TextOptions`](TextOptions.md)

## Properties

| Property | Type | Description | Inherited from |
| ------ | ------ | ------ | ------ |
| <a id="align"></a> `align?` | `"center"` \| `"start"` \| `"end"` | Where its lines sit in its width. Default `"start"`. | [`TextOptions`](TextOptions.md).[`align`](TextOptions.md#align) |
| <a id="caps"></a> `caps?` | `boolean` | Uppercase and spaced out: a section label. | [`TextOptions`](TextOptions.md).[`caps`](TextOptions.md#caps) |
| <a id="font"></a> `font?` | `"serif"` \| `"sans"` | - | [`TextOptions`](TextOptions.md).[`font`](TextOptions.md#font) |
| <a id="id"></a> `id?` | `string` | Makes it tappable (a link): a tap sends a `tap` event with this id. | [`TextOptions`](TextOptions.md).[`id`](TextOptions.md#id) |
| <a id="italic"></a> `italic?` | `boolean` | - | [`TextOptions`](TextOptions.md).[`italic`](TextOptions.md#italic) |
| <a id="kind"></a> `kind` | `"text"` | - | - |
| <a id="lines"></a> `lines?` | `number` | At most this many lines, ending with "…". | [`TextOptions`](TextOptions.md).[`lines`](TextOptions.md#lines) |
| <a id="size"></a> `size?` | `"small"` \| `"xsmall"` \| `"large"` \| `"title"` \| `"body"` | - | [`TextOptions`](TextOptions.md).[`size`](TextOptions.md#size) |
| <a id="style"></a> `style?` | [`TextStyle`](TextStyle.md) | A preset of font, size, weight and tone; the options below override it. Default `"body"`. | [`TextOptions`](TextOptions.md).[`style`](TextOptions.md#style) |
| <a id="text"></a> `text` | `string` \| [`Span`](Span.md)[] | - | - |
| <a id="tone"></a> `tone?` | `"ink"` \| `"secondary"` \| `"inverse"` | `"inverse"` is white. Unset inside an `"ink"` box: white; elsewhere the style's. | [`TextOptions`](TextOptions.md).[`tone`](TextOptions.md#tone) |
| <a id="weight"></a> `weight?` | `"bold"` \| `"medium"` \| `"regular"` | - | [`TextOptions`](TextOptions.md).[`weight`](TextOptions.md#weight) |
| <a id="width"></a> `width?` | [`Width`](Width.md) | - | [`TextOptions`](TextOptions.md).[`width`](TextOptions.md#width) |
