[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / quote

# Function: quote()

```ts
function quote(quote): QuoteBlock;
```

A passage set apart: its text in italics with a rule on its left, at most 5 lines (a longer
one ends with "…"), and its `source` on a line under it. With an `id`, tapping it sends a
`tap` event with that id (open the highlight with [zk.library.open](../library/open.md)'s `annotation`).
Line breaks in `text` are kept; spaces around it are not. May be on Home's body. Needs app
0.11.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `quote` | \{ `id?`: `string`; `source?`: `string`; `text`: `string`; \} |
| `quote.id?` | `string` |
| `quote.source?` | `string` |
| `quote.text` | `string` |

## Returns

[`QuoteBlock`](QuoteBlock.md)

## Examples

```ts
zk.ui.quote({ text: "It is a truth universally acknowledged…", source: "Pride and Prejudice · p. 1" });
```

```ts
zk.ui.quote({ id: "passage", text: a.text ?? "", source: `${item.title} · p. ${a.pageLabel}` });
// onEvent: if (e.kind === "tap" && e.id === "passage") await zk.library.open(item.key, { annotation: a.key });
```
