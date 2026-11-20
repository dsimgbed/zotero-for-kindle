[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / progress

# Function: progress()

```ts
function progress(value, opts?): ProgressBlock;
```

The app's thin progress bar, `value` from 0 to 1. Needs app 0.12.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `value` | `number` |
| `opts?` | \{ `width?`: [`Width`](Width.md); \} |
| `opts.width?` | [`Width`](Width.md) |

## Returns

[`ProgressBlock`](ProgressBlock.md)

## Example

```ts
zk.ui.progress(0.4);
```
