// examples/reading-desk/src/main.ts
var heading = (text) => zk.ui.text(text, { style: "label" });
var isBook = (item) => item.type === "book" || item.type === "bookSection" || item.format === "EPUB";
async function findContinue() {
  const found = {};
  for await (const item of zk.library.items({ opened: true, read: false })) {
    const slot = isBook(item) ? "book" : "paper";
    found[slot] ??= item;
    if (found.book && found.paper) break;
  }
  return found;
}
var loadPins = async () => await zk.storage.get("pins") ?? [];
zk.itemAction({
  id: "pin",
  title: "Pin to Home / Unpin",
  run: async (item) => {
    let pinned = false;
    await zk.storage.update("pins", (pins = []) => {
      const rest = pins.filter((p) => p.key !== item.key);
      pinned = rest.length === pins.length;
      return pinned ? [...rest, { key: item.key, library: item.libraryId }] : rest;
    });
    return pinned ? "Pinned to Home" : "Unpinned";
  }
});
async function findInbox(collection) {
  const unread = [];
  for await (const item of zk.library.items({ collection, read: false })) unread.push(item);
  unread.sort((a, b) => a.addedAt < b.addedAt ? 1 : a.addedAt > b.addedAt ? -1 : 0);
  return unread;
}
zk.screen("desk", {
  title: "Reading Desk",
  home: true,
  main: true,
  render: async () => {
    const { book, paper } = await findContinue();
    const blocks = [];
    if (book) {
      blocks.push(zk.ui.items({ id: "book", style: "card", items: [{ key: book.key, library: book.libraryId, label: "BOOK \xB7 CONTINUE" }] }));
    }
    if (paper) {
      blocks.push(zk.ui.items({ id: "paper", style: "card", items: [{ key: paper.key, library: paper.libraryId, label: "PAPER \xB7 CONTINUE" }] }));
    }
    const shown = /* @__PURE__ */ new Set([book?.key, paper?.key]);
    const pins = (await loadPins()).filter((p) => !shown.has(p.key));
    for (const p of pins) shown.add(p.key);
    if (pins.length > 0) {
      blocks.push(heading("PINNED"));
      blocks.push(zk.ui.items({ id: "pinned", style: "row", items: pins }));
    }
    const inbox = zk.settings.inbox;
    if (inbox) {
      const unread = (await findInbox(inbox)).filter((i) => !shown.has(i.key));
      if (unread.length > 0) {
        blocks.push(heading(`NEW IN INBOX (${unread.length})`));
        blocks.push(
          zk.ui.items({
            id: "inbox",
            style: "row",
            items: unread.map((i) => ({ key: i.key, library: i.libraryId }))
          })
        );
      }
    }
    if (blocks.length === 0) blocks.push(zk.ui.text("Nothing to continue yet. Open a book or pin an item.", { style: "meta" }));
    return zk.ui.column(blocks, { gap: "medium" });
  }
});
