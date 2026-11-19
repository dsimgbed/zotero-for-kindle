[Plugin API v0](../README.md) / [zk](../zk.md) / ScreenSpec

# Interface: ScreenSpec\<P\>

A screen, as [zk.screen](screen.md) registers it.

## Type Parameters

| Type Parameter | Default type |
| ------ | ------ |
| <a id="p"></a> `P` | `undefined` |

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="home"></a> `home?` | `boolean` | Offer this screen as the body of the app's Home (Settings → Home lists it as "<title> — <plugin name>"; the user picks one). Default `false`. Needs app 0.10.0. The app keeps Home's frame: the title and sync line, the Plugins, settings, sync and close icons, the search field, the error, safe-mode and update banners, and the tab bar. The screen draws only the body between the search field and the banners/tab bar, and its `render` takes no params. The body may hold every block except [zk.ui.search](ui/search.md) and [zk.ui.field](ui/field.md); a tree with one is invalid for Home. Home falls back to the app's built-in body, with no message, when plugins are off (safe mode, stopped for an install), the chosen plugin is disabled, removed or incompatible or has no such screen, no tree has been drawn yet and the first one doesn't arrive within 1 s (the last tree is kept and drawn first on the next start), or `render`/`onEvent` fails. The next time Home shows, the plugin is tried again. A periodic sync that changes nothing repaints nothing. **Example** `zk.screen("home", { title: "Reading now", home: true, render: async () => { const keys: string[] = []; for await (const i of zk.library.items({ opened: true, limit: 3 })) keys.push(i.key); return zk.ui.column([zk.ui.items({ id: "recent", style: "card", items: keys, empty: "Nothing opened yet" })]); }, });` |
| <a id="main"></a> `main?` | `boolean` | List this screen in the Plugins launcher (opened there with no params). Default `false`. |
| <a id="title"></a> `title` | `string` | Shown in the header (with the plugin's name), and in the launcher for a main screen. Short. |

## Methods

### onEvent()?

```ts
onEvent?(event, params): void | Promise<void>;
```

Handles what the user did ([zk.ui.Event](ui/Event.md)). After it returns, the screen is rendered
again. Budget at least 30 s. Optional: a screen that only shows things needs none.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `event` | [`Event`](ui/Event.md) |
| `params` | `P` |

#### Returns

`void` \| `Promise`\<`void`\>

***

### render()

```ts
render(params): Block | Promise<Block>;
```

The screen as it should look now: one block, usually a [zk.ui.column](ui/column.md). Called when the
screen opens (after the `open` event), after every event, and on [zk.ui.refresh](ui/refresh.md).
Budget 1 s. The app compares the new tree with the last one and redraws only the top-level
children of the root that changed.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `params` | `P` |

#### Returns

[`Block`](ui/Block.md) \| `Promise`\<[`Block`](ui/Block.md)\>
