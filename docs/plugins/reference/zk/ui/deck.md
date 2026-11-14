[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / deck

# Function: deck()

```ts
function deck(deck): DeckBlock;
```

Shows one card at a time; swiping it left or right sends a `swipe` event (the app turns the
card with the reader's page-turn feel when the next render shows another). With no card,
shows `empty`.

A deck is the screen's main thing: in a screen's top-level [column](column.md) it takes all the
height the other blocks leave, and the blocks after it sit at the bottom of the screen. Every
card — short, long, or none — has that same size, so nothing around it moves from card to
card; a long body pages inside the card (up/down swipes, "2 / 3" in its corner). Needs app
0.9.0 (before, a deck took its card's height).

Tapping the card sends a `tap` event with the deck's `id` (e.g. to open the paper with
[zk.reader.open](../reader/open.md)). Needs app 0.9.0.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `deck` | `Omit`\<[`DeckBlock`](DeckBlock.md), `"kind"`\> |

## Returns

[`DeckBlock`](DeckBlock.md)

## Example

```ts
zk.ui.deck({ id: "cards", card: top && zk.ui.card({ title: top.title }), empty: "Nothing left to riffle" });
```
