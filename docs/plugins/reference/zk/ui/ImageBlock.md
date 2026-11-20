[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / ImageBlock

# Interface: ImageBlock

See [image](image.md). Needs app 0.12.0.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="id"></a> `id?` | `string` | Makes it tappable: a tap sends a `tap` event with this id. |
| <a id="kind"></a> `kind` | `"image"` | - |
| <a id="src"></a> `src` | `string` | A path in the plugin's package: not absolute, no `..`. |
| <a id="width"></a> `width?` | [`Width`](Width.md) | - |
