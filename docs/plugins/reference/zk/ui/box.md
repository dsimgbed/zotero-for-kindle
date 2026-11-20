[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / box

# Function: box()

```ts
function box(child, opts?): BoxBlock;
```

A frame around one block: padding, a border, a fill, rounded corners. Text inside an `"ink"`
box is drawn white unless its own `tone` says otherwise. With an `id`, tapping it sends a
`tap` event with that id: the way to make a custom button, a link, a segmented switch.
Needs app 0.12.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `child` | [`Block`](Block.md) |
| `opts?` | \{ `border?`: [`Border`](Border.md); `fill?`: [`Fill`](Fill.md); `id?`: `string`; `pad?`: [`Gap`](Gap.md); `radius?`: [`Radius`](Radius.md); `width?`: [`Width`](Width.md); \} |
| `opts.border?` | [`Border`](Border.md) |
| `opts.fill?` | [`Fill`](Fill.md) |
| `opts.id?` | `string` |
| `opts.pad?` | [`Gap`](Gap.md) |
| `opts.radius?` | [`Radius`](Radius.md) |
| `opts.width?` | [`Width`](Width.md) |

## Returns

[`BoxBlock`](BoxBlock.md)

## Example

```ts
zk.ui.box(zk.ui.text("Week", { tone: "inverse" }), { id: "week", pad: "small", fill: "ink", radius: "round" });
```
