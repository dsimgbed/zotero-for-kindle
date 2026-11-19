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
| `label?` | `string` | A short line shown with the item (e.g. "Up next"). |
| `library?` | `number` | 0 My Library (the default), else a group's id ([zk.library.libraries](../library/libraries.md)). |
