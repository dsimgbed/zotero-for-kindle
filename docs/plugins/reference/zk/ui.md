[Plugin API v0](../README.md) / [zk](../zk.md) / ui

# ui

Building blocks for [zk.screen](screen.md), and the calls that open and close screens.

Each helper returns a plain object — `zk.ui.text("Hi")` is `{ kind: "text", text: "Hi" }` —
so a plugin may write the objects directly. Blocks have no pixel sizes, colours, fonts or
images: the app lays them out on every Kindle (paging what doesn't fit, as its own lists do)
and picks the e-ink refresh. Every block the user can act on has an `id`, unique within the
screen; the events it causes name it. Where blocks take a list of children, falsy values
(`null`, `undefined`, `false`, `""`, `0`) are skipped, so `cond && block` works.

Caps: 2,000 blocks (a list row, a chip or a button counts as one) and 256 KB of text a tree.

## Interfaces

- [BoxBlock](ui/BoxBlock.md)
- [ButtonBlock](ui/ButtonBlock.md)
- [ButtonsBlock](ui/ButtonsBlock.md)
- [CardBlock](ui/CardBlock.md)
- [ChipsBlock](ui/ChipsBlock.md)
- [ColumnBlock](ui/ColumnBlock.md)
- [CoverBlock](ui/CoverBlock.md)
- [DeckBlock](ui/DeckBlock.md)
- [DividerBlock](ui/DividerBlock.md)
- [FieldBlock](ui/FieldBlock.md)
- [GridBlock](ui/GridBlock.md)
- [ImageBlock](ui/ImageBlock.md)
- [ItemBlock](ui/ItemBlock.md)
- [ItemsBlock](ui/ItemsBlock.md)
- [ListBlock](ui/ListBlock.md)
- [ListRow](ui/ListRow.md)
- [NoticeBlock](ui/NoticeBlock.md)
- [ProgressBlock](ui/ProgressBlock.md)
- [QuoteBlock](ui/QuoteBlock.md)
- [RowBlock](ui/RowBlock.md)
- [SearchBlock](ui/SearchBlock.md)
- [SpaceBlock](ui/SpaceBlock.md)
- [SpacerBlock](ui/SpacerBlock.md)
- [TextBlock](ui/TextBlock.md)
- [TextOptions](ui/TextOptions.md)

## Type Aliases

- [Block](ui/Block.md)
- [Border](ui/Border.md)
- [Child](ui/Child.md)
- [Event](ui/Event.md)
- [Falsy](ui/Falsy.md)
- [Fill](ui/Fill.md)
- [Gap](ui/Gap.md)
- [ItemRef](ui/ItemRef.md)
- [Percent](ui/Percent.md)
- [Radius](ui/Radius.md)
- [RowAlign](ui/RowAlign.md)
- [Size](ui/Size.md)
- [Span](ui/Span.md)
- [TextStyle](ui/TextStyle.md)
- [Width](ui/Width.md)

## Functions

- [box](ui/box.md)
- [button](ui/button.md)
- [buttons](ui/buttons.md)
- [card](ui/card.md)
- [chips](ui/chips.md)
- [close](ui/close.md)
- [column](ui/column.md)
- [cover](ui/cover.md)
- [deck](ui/deck.md)
- [divider](ui/divider.md)
- [field](ui/field.md)
- [grid](ui/grid.md)
- [image](ui/image.md)
- [item](ui/item.md)
- [items](ui/items.md)
- [list](ui/list.md)
- [notice](ui/notice.md)
- [open](ui/open.md)
- [progress](ui/progress.md)
- [quote](ui/quote.md)
- [refresh](ui/refresh.md)
- [row](ui/row.md)
- [search](ui/search.md)
- [space](ui/space.md)
- [spacer](ui/spacer.md)
- [text](ui/text.md)
