[Plugin API v0](../README.md) / [zk](../zk.md) / display

# Variable: display

```ts
const display: object;
```

The app's window, for a screen that adapts to its size (a Home that drops a large block on a
short screen). Lengths are layout pixels at 300 ppi, the unit the app lays out in, so the same
physical size on every Kindle: a Paperwhite 5 is 1236 × 1547, a Scribe 1860 × 2379, a basic
Kindle (KT4) 1078 × 1337. Read when asked: `landscape` turns with the device (only the reader
turns; the app's screens, Home and plugin screens, are portrait). No permission. Needs app 0.11.0.

## Type Declaration

| Name | Type | Description |
| ------ | ------ | ------ |
| <a id="height"></a> `height` | `number` | The window's height (the screen under the status bar), in pixels at 300 ppi. |
| <a id="landscape"></a> `landscape` | `boolean` | Wider than tall. |
| <a id="model"></a> `model` | `"pw5"` \| `"kt4"` \| `"scribe"` \| `string` & `object` | The Kindle: `"pw5"`, `"kt4"` or `"scribe"`, else the device's name in lower case without spaces. The desktop build reports the Kindle it imitates. |
| <a id="width"></a> `width` | `number` | The window's width in pixels at 300 ppi. |

## Example

```ts
const short = zk.display.height < 1400; // a basic Kindle: keep Home's top compact
```
