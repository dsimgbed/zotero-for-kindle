// examples/reading-desk/src/data.ts
var MAX_PINS = 50;
var PAIR_WALK = 200;
var READING_WALK = 60;
var INBOX_CAP = 100;
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
async function inProgress(limit) {
  const out = [];
  for await (const item of zk.library.items({ opened: true, read: false, limit })) out.push(item);
  return out;
}
async function openedPair() {
  let book;
  let paper;
  for await (const item of zk.library.items({ opened: true, read: false, limit: PAIR_WALK })) {
    if (isBook(item)) book ??= item;
    else paper ??= item;
    if (book && paper) break;
  }
  return { book, paper };
}
async function pinnedItems(pins) {
  const items = await Promise.all(pins.map((p) => zk.library.item(p.key)));
  return items.filter((i) => i !== null);
}
async function inboxItems(inbox) {
  const unread = [];
  const query = { collection: inbox.key, read: false, limit: INBOX_CAP };
  if (inbox.library !== void 0) query.library = inbox.library;
  for await (const item of zk.library.items(query)) unread.push(item);
  return unread.sort((a, b) => a.addedAt < b.addedAt ? 1 : a.addedAt > b.addedAt ? -1 : 0);
}
var countOf = (n, walked) => walked >= INBOX_CAP ? `${INBOX_CAP - 1}+` : String(n);
function authorsOf(i, opts) {
  const authors = i.creators.filter((c) => c.type === "author");
  const people = authors.length > 0 ? authors : i.creators;
  if (opts.full && people.length === 1) {
    const c = people[0];
    return c.name ?? [c.first, c.last].filter(Boolean).join(" ");
  }
  const names = people.map((c) => c.last ?? c.name ?? "").filter(Boolean);
  return names.length > opts.most ? `${names[0]} et al.` : names.join(", ");
}
var bylineOf = (i) => [authorsOf(i, { most: 4 }), i.year ?? ""].filter(Boolean).join(" \xB7 ");
var percentOf = (i) => i.progress === null ? "" : `${Math.round(i.progress * 100)}%`;
var DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
var MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
function whenOf(iso, now = /* @__PURE__ */ new Date()) {
  if (!iso) return "";
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "";
  const midnight = (x) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
  const days = Math.round((midnight(now) - midnight(d)) / 864e5);
  if (days <= 0) return "today";
  if (days === 1) return "yesterday";
  if (days < 7) return DAYS[d.getDay()];
  const date = `${MONTHS[d.getMonth()]} ${d.getDate()}`;
  return d.getFullYear() === now.getFullYear() ? date : `${date}, ${d.getFullYear()}`;
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
async function keptPassage() {
  const kept = await zk.storage.get("passage");
  if (kept?.day !== today()) pickLater();
  return kept?.pick;
}
var picking = false;
function pickLater() {
  if (picking) return;
  picking = true;
  setTimeout(async () => {
    try {
      await pickPassage();
      zk.ui.refresh();
    } catch (err) {
      console.warn("the passage couldn't be picked:", err);
    } finally {
      picking = false;
    }
  }, 0);
}
async function pickPassage() {
  const day = today();
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
}
async function openPassage() {
  const pick = (await zk.storage.get("passage"))?.pick;
  if (!pick) return;
  const why = (err) => err instanceof Error ? err.message : String(err);
  try {
    await zk.library.open({ key: pick.itemKey, library: pick.library }, { annotation: pick.key });
  } catch (err) {
    const reason = why(err);
    if (reason.includes("Cancelled")) return;
    if (reason.includes("Item not found") || reason.includes("Annotation not found")) {
      await zk.storage.delete("passage");
      zk.ui.refresh();
    }
    if (!reason.includes("Annotation not found")) {
      if (!reason.includes("Item not found")) console.warn("the passage didn't open:", reason);
      return;
    }
    try {
      await zk.library.open({ key: pick.itemKey, library: pick.library });
    } catch (again) {
      if (!why(again).includes("Cancelled")) console.warn("the passage's item didn't open:", why(again));
    }
  }
}

// examples/reading-desk/src/pieces.ts
var MARGIN = 60;
var MEDIUM = 36;
var LARGE = 60;
var pageWidth = () => zk.display.width - 2 * MARGIN;
var share = (px, width) => `${Math.min(100, Math.max(1, Math.round(100 * px / (width - MEDIUM))))}%`;
var perRow = (px, width, gap) => Math.max(1, Math.round((width + gap) / (px + gap)));
var label = (text) => zk.ui.text(text, { style: "label", caps: true });
var title = (text, size, lines2) => zk.ui.text(text || "Untitled", { font: "serif", weight: "bold", size, lines: lines2 });
var meta = (text, size = "small", lines2 = 1) => zk.ui.text(text, { font: "sans", size, tone: "secondary", lines: lines2 });
var endLink = (text, id) => zk.ui.text(text, { id, align: "end", font: "sans", size: "body", weight: "medium", tone: "ink" });
function heading(text, link) {
  return zk.ui.row([
    label(text),
    link && zk.ui.text(link.text, { id: link.id, width: "auto", font: "sans", size: "body", weight: "medium", tone: "ink" })
  ]);
}
function pair(side) {
  const { item } = side;
  return zk.ui.column([
    label(side.kicker),
    zk.ui.item(
      ref(item),
      zk.ui.row(
        [
          zk.ui.cover(ref(item), { width: side.cover }),
          zk.ui.column(
            [
              title(item.title, "body", side.titleLines),
              side.authors && meta(side.authors, "small", 2),
              zk.ui.spacer(),
              item.progress !== null && zk.ui.progress(item.progress),
              meta(side.meta)
            ],
            { gap: "xsmall" }
          )
        ],
        { align: "stretch", gap: "medium" }
      )
    )
  ]);
}
function shelfRow(items, columns2) {
  return zk.ui.grid(
    items.map(
      (i) => zk.ui.item(ref(i), zk.ui.column([zk.ui.cover(ref(i)), i.progress !== null && zk.ui.progress(i.progress)]))
    ),
    { columns: columns2, rows: 1, gap: "medium" }
  );
}
function lines(rows) {
  return rows.map(
    ({ item, byline, right }) => zk.ui.item(
      ref(item),
      zk.ui.row(
        [
          zk.ui.column([title(item.title, "body", 1), byline && meta(byline)], { gap: "xsmall" }),
          right && zk.ui.text(right, { width: "auto", font: "sans", size: "small", tone: "secondary" })
        ],
        { align: "top", gap: "medium" }
      )
    )
  );
}
function segmented(segments, selected) {
  return zk.ui.box(
    zk.ui.row(
      segments.map(
        (s) => zk.ui.box(zk.ui.text(s.text, { align: "center", font: "sans", size: "body", weight: "medium" }), {
          id: s.id,
          pad: "small",
          fill: s.id === selected ? "ink" : "none",
          radius: "small"
        })
      ),
      { gap: "none" }
    ),
    { border: "thin", radius: "small" }
  );
}
var barSlot = (item) => zk.ui.row(
  [item.progress !== null && zk.ui.progress(item.progress), zk.ui.box(zk.ui.spacer(), { pad: "xsmall", width: "1%" })],
  { gap: "none", align: "top" }
);
function coverGrid(cells, columns2) {
  return zk.ui.grid(
    cells.map(
      ({ item, caption: caption2 }) => zk.ui.item(
        ref(item),
        zk.ui.column([
          zk.ui.cover(ref(item)),
          barSlot(item),
          zk.ui.column([title(item.title, "small", 1), caption2 && meta(caption2, "xsmall")], { gap: "xsmall" })
        ])
      )
    ),
    { columns: columns2, gap: "large" }
  );
}

// examples/reading-desk/src/desk.ts
var TABS = ["reading", "pinned", "inbox"];
var EMPTY = {
  reading: "Books and papers you open show up here.",
  pinned: "Nothing pinned yet. Long-press an item \u2192 More\u2026 \u2192 Pin to Home / Unpin.",
  inbox: "Nothing new in the inbox."
};
var columns = () => perRow(220, pageWidth(), LARGE);
function caption(i) {
  return [authorsOf(i, { most: 1 }), i.read ? "Read" : percentOf(i)].filter(Boolean).join(" \xB7 ");
}
var picked = null;
function registerDesk() {
  zk.screen("all", {
    title: "Your desk",
    render: async (params) => {
      const inbox = inboxOf(await loadPrefs());
      const unread = inbox ? await inboxItems(inbox) : null;
      const asked = picked ?? params?.tab ?? "reading";
      const tab = TABS.includes(asked) && (asked !== "inbox" || unread) ? asked : "reading";
      const items = tab === "reading" ? await inProgress(READING_WALK) : tab === "pinned" ? await pinnedItems(await loadPins()) : unread;
      const segments = [
        { id: "tab-reading", text: "Reading" },
        { id: "tab-pinned", text: "Pinned" },
        unread && { id: "tab-inbox", text: `Inbox ${countOf(unread.length, unread.length)}` }
      ].filter((s) => !!s);
      return zk.ui.column(
        [
          segmented(segments, `tab-${tab}`),
          items.length > 0 ? coverGrid(items.map((item) => ({ item, caption: caption(item) })), columns()) : zk.ui.text(EMPTY[tab], { tone: "secondary" })
        ],
        { gap: "large" }
      );
    },
    onEvent: (e) => {
      if (e.kind === "open" || e.kind === "back") picked = null;
      else if (e.kind === "tap" && e.id.startsWith("tab-")) {
        const tab = e.id.slice(4);
        if (TABS.includes(tab)) picked = tab;
      }
    }
  });
}

// examples/reading-desk/src/home.ts
var COVER = 190;
var COVER_TURNED = 160;
var tall = () => zk.display.height >= 2e3;
var bandGap = () => zk.display.height < 1400 ? "medium" : "large";
var pinsPerRow = () => perRow(COVER, pageWidth(), MEDIUM);
var FIRST_USE = "Open a book or a paper and it will wait for you here. Pin things from an item's menu: long-press \u2192 More\u2026 \u2192 Pin to Home.";
function sideMeta(s) {
  if (!s.started) return "Not started";
  return [percentOf(s.item), whenOf(s.item.openedAt)].filter(Boolean).join(" \xB7 ");
}
function registerHome() {
  zk.screen("desk", {
    title: "Reading Desk",
    home: true,
    render: async () => {
      const prefs = await loadPrefs();
      const inbox = inboxOf(prefs);
      const [opened, pins, unread] = await Promise.all([openedPair(), loadPins(), inbox ? inboxItems(inbox) : null]);
      const pinned = await pinnedItems(pins);
      const upNext = (book) => pinned.find((i) => isBook(i) === book && i.openedAt === null && !i.read);
      const side = (item, kicker, book) => {
        if (item) return { kicker, item, started: true };
        const next = upNext(book);
        return next && { kicker: "Up next", item: next, started: false };
      };
      const sides = [side(opened.book, "Book", true), side(opened.paper, "Paper", false)].filter((s) => !!s);
      const inPair = (i) => sides.some((s) => same(ref(s.item), ref(i)));
      const turned = zk.display.landscape && zk.display.height < 1400;
      const sideWidth = sides.length === 1 ? pageWidth() : (pageWidth() - MEDIUM) / 2;
      const pairBand = sides.length > 0 && zk.ui.row(
        sides.map(
          (s) => pair({
            kicker: s.kicker,
            item: s.item,
            authors: authorsOf(s.item, { most: 2, full: true }),
            meta: sideMeta(s),
            cover: share(turned ? COVER_TURNED : COVER, sideWidth),
            titleLines: turned ? 2 : 3
          })
        ),
        { align: "top", gap: "medium" }
      );
      const columns2 = pinsPerRow();
      const shelf = pinned.filter((i) => !inPair(i)).slice(0, columns2);
      const pinnedBand = shelf.length > 0 && zk.ui.column([heading("Pinned", { id: "all-pinned", text: `All ${pinned.length} \u203A` }), shelfRow(shelf, columns2)]);
      const fresh = (unread ?? []).filter((i) => !inPair(i));
      const inboxBand = fresh.length > 0 && zk.ui.column(
        [
          heading(`Inbox \xB7 ${countOf(unread.length, unread.length)} new`, { id: "all-inbox", text: "All \u203A" }),
          ...lines(fresh.slice(0, tall() ? 3 : 2).map((item) => ({ item, byline: bylineOf(item), right: whenOf(item.addedAt) })))
        ],
        { gap: "small" }
      );
      const pick = prefs.passage === false || inboxBand && !tall() ? void 0 : await keptPassage();
      const firstUse = !pairBand && !pinnedBand && !inboxBand;
      const deskLink = !pinnedBand && !inboxBand && endLink("Your desk \u203A", "your-desk");
      return zk.ui.column(
        [
          pairBand,
          pinnedBand,
          inboxBand,
          firstUse && zk.ui.text(FIRST_USE, { tone: "secondary" }),
          deskLink,
          pick && zk.ui.quote({ id: "passage", text: pick.text, source: pick.source })
        ],
        { gap: bandGap(), fit: "trim" }
      );
    },
    onEvent: async (e) => {
      if (e.kind !== "tap") return;
      if (e.id === "all-pinned") zk.ui.open("all", { tab: "pinned" });
      else if (e.id === "all-inbox") zk.ui.open("all", { tab: "inbox" });
      else if (e.id === "your-desk") zk.ui.open("all", { tab: "reading" });
      else if (e.id === "passage") await openPassage();
    }
  });
}

// examples/reading-desk/src/settings.ts
var collectionName = (c) => c.path.join(" / ") + (c.libraryId !== 0 ? ` (${c.libraryName})` : "");
var authorsLine = (i) => {
  const names = i.creators.map((c) => c.last ?? c.name ?? "").filter(Boolean);
  const who = names.length > 2 ? `${names[0]} et al.` : names.join(", ");
  return [who, i.year ?? ""].filter(Boolean).join(" \xB7 ");
};
var label2 = (text) => zk.ui.text(text, { style: "label" });
var unpinAsk = null;
var pinId = (p) => `${p.library}:${p.key}`;
var inboxSearch = "";
function registerSettings() {
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
  zk.screen("settings", {
    title: "Settings",
    main: true,
    render: async () => {
      const prefs = await loadPrefs();
      const inbox = inboxOf(prefs);
      let inboxName = "None: no inbox on Home";
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
                subtitle: "One of your highlights on Home, a new one each day",
                meta: prefs.passage === false ? "Off" : "On"
              }
            ]
          }),
          label2(`PINNED (${pins.length})`),
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
              subtitle: items[n] ? authorsLine(items[n]) : void 0,
              meta: "Unpin\u2026",
              // a tap asks first
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
            ...words.length === 0 ? [{ id: "none", title: "None", subtitle: "No inbox on Home", selected: !inbox }] : [],
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
}

// examples/reading-desk/src/main.ts
registerHome();
registerDesk();
registerSettings();
