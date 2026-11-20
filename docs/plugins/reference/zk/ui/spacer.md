[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / spacer

# Function: spacer()

```ts
function spacer(): SpacerBlock;
```

Flexible empty space: in a column that is as tall as its row (a row with `align:
"stretch"`) it takes the height the other children leave; anywhere else it takes no room.
Needs app 0.12.0.

## Returns

[`SpacerBlock`](SpacerBlock.md)

## Example

```ts
zk.ui.row([cover, zk.ui.column([title, zk.ui.spacer(), zk.ui.progress(0.4)])], { align: "stretch" });
```
