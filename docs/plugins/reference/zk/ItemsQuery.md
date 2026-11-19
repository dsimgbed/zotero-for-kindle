[Plugin API v0](../README.md) / [zk](../zk.md) / ItemsQuery

# Interface: ItemsQuery

Filters for [zk.library.items](library/items.md). They combine: an item must match all that are given.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="collection"></a> `collection?` | `string` | Only items in this collection (its key; not its subcollections). |
| <a id="library"></a> `library?` | `number` | Only items of this library: 0 My Library, else a group's id ([zk.library.libraries](library/libraries.md)). Needs app 0.8.0. |
| <a id="limit"></a> `limit?` | `number` | Stop after this many items. |
| <a id="opened"></a> `opened?` | `true` | `true`: only items opened on this Kindle, the most recently opened first (the order of the app's Recent list). Needs app 0.10.0. **Example** `for await (const item of zk.library.items({ opened: true, limit: 5 })) console.log(item.title, item.progress);` |
| <a id="read"></a> `read?` | `boolean` | Only items marked read (`true`) or not marked read (`false`): the read mark, never progress. Needs app 0.10.0. |
| <a id="tag"></a> `tag?` | `string` | Only items with this tag (its name, exact). |
