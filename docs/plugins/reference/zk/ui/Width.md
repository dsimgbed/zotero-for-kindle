[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / Width

# Type Alias: Width

```ts
type Width = "fill" | "auto" | Percent;
```

How wide a block is, for any block (it matters in a [row](row.md), and for a cover, a
progress bar or an image anywhere): `"fill"` shares what the siblings leave, `"auto"` is
its natural width (a text's, a button's), `"<n>%"` is that share of the row's width.
However long an `"auto"` sibling, a `"fill"` keeps at least 120 px (or an equal share of a
narrow row) and a percent as much of that as it asks for: the long text wraps instead.
Needs app 0.12.0.
