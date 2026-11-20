[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / image

# Function: image()

```ts
function image(image): ImageBlock;
```

A PNG from the plugin's package, drawn in greys with its aspect kept. `src` is a path in
the package (never absolute, never with `..`). With an `id` it is tappable. Needs app
0.12.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `image` | \{ `id?`: `string`; `src`: `string`; `width?`: [`Width`](Width.md); \} |
| `image.id?` | `string` |
| `image.src` | `string` |
| `image.width?` | [`Width`](Width.md) |

## Returns

[`ImageBlock`](ImageBlock.md)

## Example

```ts
zk.ui.image({ src: "images/logo.png", width: "40%" });
```
