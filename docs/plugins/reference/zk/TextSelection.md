[Plugin API v0](../README.md) / [zk](../zk.md) / TextSelection

# Interface: TextSelection

What [selectionAction](selectionAction.md) hands to `run`.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="attachment"></a> `attachment` | [`Attachment`](Attachment.md) \| `null` | The file being read; `null` in a preview. |
| <a id="item"></a> `item` | [`Item`](Item.md) \| `null` | The item being read; `null` in a preview ([zk.reader.open](reader/open.md), app 0.9.0). |
| <a id="pagelabel"></a> `pageLabel` | `string` \| `null` | The page label where the selection starts, if the document has pages. |
| <a id="preview"></a> `preview?` | `object` | In a preview (app 0.9.0): what is being read instead of an item. |
| `preview.title` | `string` | - |
| `preview.url` | `string` \| `null` | - |
| <a id="text"></a> `text` | `string` | The selected text. |
