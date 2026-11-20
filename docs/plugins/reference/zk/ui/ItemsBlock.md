[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / ItemsBlock

# Interface: ItemsBlock

See [items](items.md).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="empty"></a> `empty?` | `string` | Shown when none of the items can be read. |
| <a id="id"></a> `id` | `string` | - |
| <a id="items"></a> `items` | [`ItemRef`](ItemRef.md)[] | - |
| <a id="kind"></a> `kind` | `"items"` | - |
| <a id="style"></a> `style` | `"card"` \| `"row"` \| `"shelf"` | `"card"`: a large entry per item; `"row"`: a compact line per item; `"shelf"`: cover tiles in rows. A shelf's tile is the item's cover as the app draws it (title and authors on a dark cover; "PDF" or "WEB" in its corner for a file that isn't an EPUB), a thin progress bar once it was opened, and one line under it: "13%", "New" or "Read", or the download's progress while it downloads. The tiles per row follow the screen's width (4 on a Paperwhite or a Kindle, 6 on a Scribe), and the shelf pages a row at a time. A ref's `label` isn't shown on a shelf. `"shelf"` needs app 0.11.0 (older apps refuse the tree). **Example** `zk.ui.items({ id: "shelf", style: "shelf", items: keys });` |
