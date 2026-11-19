[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / ItemRef

# Type Alias: ItemRef

```ts
type ItemRef = 
  | string
  | {
  key: string;
  label?: string;
  library?: number;
};
```

An item for an [items](items.md) block: its key, or the key with its library and a label.

## Union Members

`string`

***

### Type Literal

```ts
{
  key: string;
  label?: string;
  library?: number;
}
```

| Name | Type | Description |
| ------ | ------ | ------ |
| `key` | `string` | - |
| `label?` | `string` | The kicker line above a `card`'s title (e.g. "BOOK · CONTINUE"); a `row` ignores it. |
| `library?` | `number` | 0 My Library, else a group's id ([zk.library.libraries](../library/libraries.md)). Omitted: the app looks the key up, as it does today. |
