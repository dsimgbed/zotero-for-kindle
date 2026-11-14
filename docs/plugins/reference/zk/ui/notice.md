[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / notice

# Function: notice()

```ts
function notice(text, opts?): NoticeBlock;
```

A quiet one-line message: the outcome of the last step, a hint. With an `id`, tapping it
sends a `tap` event with that id (needs app 0.9.0).

## Parameters

| Parameter | Type |
| ------ | ------ |
| `text` | `string` |
| `opts?` | \{ `id?`: `string`; \} |
| `opts.id?` | `string` |

## Returns

[`NoticeBlock`](NoticeBlock.md)

## Example

```ts
zk.ui.notice("Discarded. 3 cards skipped for later.");
zk.ui.notice("Kept into Probability", { id: "read-kept" }); // tap: read it
```
