// Your own Home: the three items you opened last, as cards. Choose it in Settings → Home.
// plugin.json: "permissions": ["library:read"], "minApp": "0.10.0"

zk.screen("home", {
  title: "Reading now",
  home: true, // offered in Settings → Home; render takes no params
  render: async () => {
    const keys: string[] = [];
    // Most recently opened first; stop as soon as there are enough.
    for await (const item of zk.library.items({ opened: true, limit: 3 })) keys.push(item.key);
    // The app draws the items, keeps their progress current and opens them on a tap.
    return zk.ui.column([
      zk.ui.items({ id: "recent", style: "card", items: keys, empty: "Nothing opened yet" }),
    ]);
  },
});
