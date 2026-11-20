[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / QuoteBlock

# Interface: QuoteBlock

See [quote](quote.md). Needs app 0.11.0.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="id"></a> `id?` | `string` | Makes it tappable: a tap sends a `tap` event with this id. |
| <a id="kind"></a> `kind` | `"quote"` | - |
| <a id="source"></a> `source?` | `string` | Where it is from, on one line under it (`"Pride and Prejudice · p. 1"`). |
| <a id="text"></a> `text` | `string` | The passage: at most 5 lines show. |
