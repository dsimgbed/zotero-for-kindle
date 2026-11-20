// examples/reading-desk/src/main.ts
var TABS = ["books", "papers", "pinned", "new"];
var MAX_PINS = 50;
var MAX_OPENED = 30;
var loadPins = async () => (await zk.storage.get("pins") ?? []).slice(0, MAX_PINS);
var loadPrefs = async () => await zk.storage.get("prefs") ?? {};
var savePrefs = (change) => zk.storage.update("prefs", (p = {}) => ({ ...p, ...change }));
function inboxOf(prefs) {
  if (prefs.inbox !== void 0) return prefs.inbox;
  return typeof zk.settings.inbox === "string" && zk.settings.inbox ? { key: zk.settings.inbox } : null;
}
var isBook = (item) => item.type === "book" || item.type === "bookSection" || item.format === "EPUB";
var ref = (i) => ({ key: i.key, library: i.libraryId });
var same = (a, b) => a.key === b.key && (a.library === void 0 || b.library === void 0 || a.library === b.library);
async function inProgress() {
  const out = [];
  for await (const item of zk.library.items({ opened: true, read: false, limit: MAX_OPENED })) out.push(item);
  return out;
}
async function pinnedItems(pins) {
  const out = [];
  for (const p of pins) {
    const item = await zk.library.item(p.key);
    if (item) out.push(item);
  }
  return out;
}
async function inboxItems(inbox) {
  const unread = [];
  const query = { collection: inbox.key, read: false };
  if (inbox.library !== void 0) query.library = inbox.library;
  for await (const item of zk.library.items(query)) unread.push(item);
  return unread.sort((a, b) => a.addedAt < b.addedAt ? 1 : a.addedAt > b.addedAt ? -1 : 0);
}
function today() {
  const d = /* @__PURE__ */ new Date();
  const two = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${two(d.getMonth() + 1)}-${two(d.getDate())}`;
}
function hash(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}
async function passageOfTheDay() {
  const day = today();
  const kept = await zk.storage.get("passage");
  if (kept?.day === day) return kept.pick;
  const candidates = [];
  for await (const a of zk.library.recentAnnotations({ limit: 300 })) {
    const text = a.text?.trim() ?? "";
    if (a.type === "highlight" && text.length >= 40 && text.length <= 400) candidates.push(a);
  }
  let pick;
  for (let n = 0; n < candidates.length && !pick; n++) {
    const a = candidates[(hash(day) + n) % candidates.length];
    const item = await zk.library.item(a.itemKey);
    if (!item) continue;
    const source = [item.title || "Untitled", a.pageLabel ? `p. ${a.pageLabel}` : ""].filter(Boolean).join(" \xB7 ");
    pick = { key: a.key, itemKey: a.itemKey, library: a.libraryId, text: a.text.trim(), source };
  }
  await zk.storage.set("passage", { day, pick });
  return pick;
}
zk.itemAction({
  id: "pin",
  title: "Pin to Home / Unpin",
  run: async (item) => {
    let reply = "";
    await zk.storage.update("pins", (pins = []) => {
      const rest = pins.filter((p) => !same(p, ref(item)));
      if (rest.length < pins.length) {
        reply = "Unpinned";
        return rest;
      }
      if (pins.length >= MAX_PINS) {
        reply = `Home holds ${MAX_PINS} pins: unpin one first (Plugins \u2192 Reading Desk)`;
        return pins;
      }
      reply = "Pinned to Home";
      return [...pins, ref(item)];
    });
    zk.ui.refresh();
    return reply;
  }
});
var CARD_MIN_HEIGHT = 1800;
var display = () => ({ width: zk.display?.width ?? 1236, height: zk.display?.height ?? 1547 });
var shortScreen = () => display().height < CARD_MIN_HEIGHT;
var FRAME_H = 434;
var PAGE_NUMBER_H = 60;
var gapPx = () => shortScreen() ? 18 : 36;
var ROW_H = 186;
var CHIPS_H = 76;
var LABEL_H = 48;
var HERO_ROW_H = LABEL_H + ROW_H;
var HERO_CARD_H = 400;
var MARGINS = 120;
function shelfRowH() {
  const w = display().width - MARGINS;
  const n = Math.max(1, Math.round((w + 36) / 276));
  return Math.floor((w - (n - 1) * 36) / n * 1.5) + 70;
}
function passageH(text) {
  const perLine = Math.floor((display().width - MARGINS - 36) / 17.5);
  const lines = Math.min(5, Math.ceil(text.length / perLine) + (text.match(/\n/g)?.length ?? 0));
  return 8 + lines * 50 + 14 + 34;
}
function pagesOf(units, budget) {
  let pages = 1;
  let used = 0;
  units.forEach((h, i) => {
    if (i > 0 && used + h > budget) {
      pages++;
      used = 0;
    }
    used += h;
  });
  return pages;
}
function pagesFor(fixed, units, tail = 0) {
  const body = display().height - FRAME_H;
  const total = units.reduce((a, b) => a + b, 0);
  if (fixed + total + tail <= body) return 1;
  const room = body - PAGE_NUMBER_H - fixed - tail;
  if (units.length > 0 && units[0] <= room) return pagesOf(units, room);
  return pagesOf([fixed, ...units, ...tail ? [tail] : []], body - PAGE_NUMBER_H);
}
var label = (text) => zk.ui.text(text, { style: "label" });
async function currentTab(hasInbox) {
  const t = await zk.storage.get("tab");
  return t && TABS.includes(t) && (t !== "new" || hasInbox) ? t : "books";
}
function nowReading(hero, kicker) {
  if (!hero) return null;
  if (!shortScreen()) return zk.ui.items({ id: "now", style: "card", items: [{ ...ref(hero), label: kicker }] });
  return zk.ui.column([label(kicker), zk.ui.items({ id: "now", style: "row", items: [ref(hero)] })], { gap: "none" });
}
zk.screen("desk", {
  title: "Reading Desk",
  home: true,
  render: async () => {
    const prefs = await loadPrefs();
    const inbox = inboxOf(prefs);
    const [opened, pins, unread] = await Promise.all([inProgress(), loadPins(), inbox ? inboxItems(inbox) : null]);
    let hero = opened[0];
    let kicker = hero ? isBook(hero) ? "CONTINUE \xB7 BOOK" : "CONTINUE \xB7 PAPER" : "";
    let pinned = null;
    const pinnedNow = async () => pinned ??= await pinnedItems(pins);
    if (!hero && pins.length > 0) {
      hero = [...await pinnedNow()].reverse().find((i) => !i.read);
      kicker = "START HERE";
    }
    const notHero = (i) => !hero || !same(ref(i), ref(hero));
    const tab = await currentTab(unread !== null);
    const newCount = unread?.filter(notHero).length ?? 0;
    const chips = [
      { id: "books", title: "Books" },
      { id: "papers", title: "Papers" },
      { id: "pinned", title: "Pinned" },
      unread && { id: "new", title: newCount > 0 ? `New ${newCount}` : "New" }
    ].filter((c) => !!c);
    const reading = opened.filter(notHero);
    const inProgressKeys = new Set(opened.map((i) => i.key));
    let content = [];
    let units = [];
    let extra = 0;
    const rows = (n) => Array(n).fill(ROW_H);
    const shelfRows = (n) => {
      const per = Math.max(1, Math.round((display().width - MARGINS + 36) / 276));
      const r = Math.ceil(n / per);
      return Array(r).fill(shelfRowH() + 40).map((h, i) => i === r - 1 ? h - 40 : h);
    };
    if (tab === "books") {
      const books = reading.filter(isBook);
      const more = (await pinnedNow()).filter((i) => isBook(i) && !inProgressKeys.has(i.key) && notHero(i));
      const all = [...books, ...more];
      content = [zk.ui.items({ id: "books", style: "shelf", items: all.map(ref), empty: "Books you open show up here." })];
      units = shelfRows(all.length);
    } else if (tab === "papers") {
      const papers = reading.filter((i) => !isBook(i));
      const next = (await pinnedNow()).filter((i) => !isBook(i) && !inProgressKeys.has(i.key) && notHero(i));
      if (papers.length === 0 && next.length === 0) {
        content = [zk.ui.items({ id: "papers", style: "row", items: [], empty: "Papers you open or pin show up here." })];
      } else if (papers.length > 0 && next.length > 0) {
        content = [
          zk.ui.items({ id: "papers", style: "row", items: papers.map(ref) }),
          label("UP NEXT"),
          zk.ui.items({ id: "next", style: "row", items: next.map(ref) })
        ];
        units = rows(papers.length + next.length);
        extra = LABEL_H + 2 * gapPx();
      } else {
        content = [
          next.length > 0 && label("UP NEXT"),
          zk.ui.items({ id: next.length > 0 ? "next" : "papers", style: "row", items: [...papers, ...next].map(ref) })
        ];
        units = rows(papers.length + next.length);
        extra = next.length > 0 ? LABEL_H + gapPx() : 0;
      }
    } else if (tab === "pinned") {
      const all = (await pinnedNow()).filter(notHero);
      content = [
        zk.ui.items({
          id: "pinned",
          style: "shelf",
          items: all.map(ref),
          empty: "Long-press an item \u2192 More\u2026 \u2192 Pin."
        })
      ];
      units = shelfRows(all.length);
    } else {
      const items = (unread ?? []).filter(notHero);
      content = [zk.ui.items({ id: "new", style: "row", items: items.map(ref), empty: "Nothing new in the inbox." })];
      units = rows(items.length);
    }
    let fixed = (hero ? (shortScreen() ? HERO_ROW_H : HERO_CARD_H) + gapPx() : 0) + CHIPS_H + gapPx() + extra;
    if (units.length === 0) units = [112];
    if (tab === "papers" && extra === LABEL_H + 2 * gapPx() && pagesFor(fixed, units) > 1) {
      const all = content.flatMap((c) => c && typeof c === "object" && c.kind === "items" ? c.items : []);
      content = [zk.ui.items({ id: "papers", style: "row", items: all })];
      fixed -= extra;
    }
    const pick = prefs.passage === false ? void 0 : await passageOfTheDay();
    const showPassage = !!pick && pagesFor(fixed, units, passageH(pick.text) + gapPx()) <= pagesFor(fixed, units);
    return zk.ui.column(
      [
        nowReading(hero, kicker),
        zk.ui.chips({ id: "tabs", select: "one", chips, selected: [tab] }),
        ...content,
        showPassage && zk.ui.quote({ id: "passage", text: pick.text, source: pick.source })
      ],
      { gap: shortScreen() ? "small" : "medium" }
    );
  },
  onEvent: async (e) => {
    if (e.kind === "chips" && e.id === "tabs" && TABS.includes(e.chip)) {
      await zk.storage.set("tab", e.chip);
    } else if (e.kind === "tap" && e.id === "passage") {
      const kept = await zk.storage.get("passage");
      const pick = kept?.pick;
      if (!pick) return;
      try {
        await zk.library.open({ key: pick.itemKey, library: pick.library }, { annotation: pick.key });
      } catch (err) {
        if (String(err).includes("Annotation not found")) {
          await zk.storage.delete("passage");
          await zk.library.open({ key: pick.itemKey, library: pick.library });
        } else throw err;
      }
    }
  }
});
var collectionName = (c) => c.path.join(" / ") + (c.libraryId !== 0 ? ` (${c.libraryName})` : "");
var authorsOf = (i) => {
  const names = i.creators.map((c) => c.last ?? c.name ?? "").filter(Boolean);
  const who = names.length > 2 ? `${names[0]} et al.` : names.join(", ");
  return [who, i.year ?? ""].filter(Boolean).join(" \xB7 ");
};
var unpinAsk = null;
var pinId = (p) => `${p.library}:${p.key}`;
zk.screen("settings", {
  title: "Settings",
  main: true,
  render: async () => {
    const prefs = await loadPrefs();
    const inbox = inboxOf(prefs);
    let inboxName = "None: the New tab is hidden";
    if (inbox) {
      const c = (await zk.library.collections()).find((c2) => same({ key: c2.key, library: c2.libraryId }, inbox));
      inboxName = c ? collectionName(c) : "A collection no longer on the Kindle";
    }
    const pins = await loadPins();
    const items = await Promise.all(pins.map((p) => zk.library.item(p.key)));
    const asked = pins.findIndex((p) => pinId(p) === unpinAsk);
    const askedTitle = asked >= 0 ? items[asked]?.title || "this item" : "";
    return zk.ui.column(
      [
        zk.ui.list({
          id: "prefs",
          rows: [
            { id: "inbox", title: "Inbox collection", subtitle: inboxName },
            {
              id: "passage",
              title: "A passage each day",
              subtitle: "One of your highlights under the desk, a new one each day",
              meta: prefs.passage === false ? "Off" : "On"
            }
          ]
        }),
        label(`PINNED (${pins.length})`),
        asked >= 0 && zk.ui.notice(`Unpin \u201C${askedTitle}\u201D?`),
        asked >= 0 && zk.ui.buttons([
          { id: "keep", title: "Keep" },
          { id: "unpin", title: "Unpin", primary: true }
        ]),
        zk.ui.list({
          id: "pins",
          rows: pins.map((p, n) => ({
            id: pinId(p),
            title: items[n]?.title || "An item no longer on the Kindle",
            subtitle: items[n] ? authorsOf(items[n]) : void 0,
            selected: pinId(p) === unpinAsk
          })),
          empty: "Nothing pinned. Long-press an item \u2192 More\u2026 \u2192 Pin to Home / Unpin."
        })
      ],
      { gap: "medium" }
    );
  },
  onEvent: async (e) => {
    if (e.kind === "select" && e.id === "prefs") {
      if (e.row === "inbox") zk.ui.open("inbox");
      else if (e.row === "passage") {
        const prefs = await loadPrefs();
        await savePrefs({ passage: prefs.passage === false });
      }
    } else if (e.kind === "select" && e.id === "pins") {
      unpinAsk = unpinAsk === e.row ? null : e.row;
    } else if (e.kind === "tap" && e.id === "keep") {
      unpinAsk = null;
    } else if (e.kind === "tap" && e.id === "unpin" && unpinAsk) {
      const gone = unpinAsk;
      await zk.storage.update("pins", (pins = []) => pins.filter((p) => pinId(p) !== gone));
      unpinAsk = null;
    } else if (e.kind === "back" || e.kind === "open") {
      unpinAsk = null;
    }
  }
});
var inboxSearch = "";
zk.screen("inbox", {
  title: "Inbox collection",
  render: async () => {
    const inbox = inboxOf(await loadPrefs());
    const words = inboxSearch.toLowerCase().split(/\s+/).filter(Boolean);
    const all = await zk.library.collections();
    const matches = all.map((c) => ({ c, name: collectionName(c) })).filter(({ name }) => words.every((w) => name.toLowerCase().includes(w))).sort((a, b) => a.name.localeCompare(b.name)).slice(0, 300);
    return zk.ui.column([
      zk.ui.search({ id: "find", placeholder: "Find a collection", value: inboxSearch }),
      zk.ui.list({
        id: "collections",
        rows: [
          ...words.length === 0 ? [{ id: "none", title: "None", subtitle: "No New tab", selected: !inbox }] : [],
          ...matches.map(({ c, name }) => ({
            id: `${c.libraryId}:${c.key}`,
            title: name,
            selected: !!inbox && same({ key: c.key, library: c.libraryId }, inbox)
          }))
        ],
        empty: "No collection matches"
      })
    ]);
  },
  onEvent: async (e) => {
    if (e.kind === "open" || e.kind === "back") inboxSearch = "";
    else if ((e.kind === "change" || e.kind === "submit") && e.id === "find") inboxSearch = e.text;
    else if (e.kind === "select" && e.id === "collections") {
      if (e.row === "none") await savePrefs({ inbox: null });
      else {
        const colon = e.row.indexOf(":");
        await savePrefs({ inbox: { library: Number(e.row.slice(0, colon)), key: e.row.slice(colon + 1) } });
      }
      inboxSearch = "";
      zk.ui.close();
    }
  }
});
