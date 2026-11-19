// Show items with their reading progress, and open one from your own button.
// plugin.json: "permissions": ["library:read"], "minApp": "0.10.0"

let keys: string[] = [];
let message = "";

zk.screen("unread", {
  title: "Unread",
  main: true,
  render: async () => {
    keys = [];
    for await (const item of zk.library.items({ read: false, limit: 5 })) keys.push(item.key);
    return zk.ui.column([
      // A tap on a row opens the item and a long-press shows its menu; the plugin hears neither.
      zk.ui.items({ id: "unread", style: "row", items: keys, empty: "Everything is read" }),
      keys.length > 0 && zk.ui.button({ id: "first", title: "Open the first", primary: true }),
      message && zk.ui.notice(message),
    ]);
  },
  onEvent: async (e) => {
    if (e.kind === "open") message = "";
    else if (e.kind === "tap" && e.id === "first" && keys[0]) {
      // Only in answer to the user. Resolves when the reader shows, rejects if it can't open.
      try {
        await zk.library.open(keys[0]);
      } catch (err) {
        message = `Couldn't open it: ${err instanceof Error ? err.message : err}`;
      }
    } else if (e.kind === "reader") message = "Welcome back";
  },
});
