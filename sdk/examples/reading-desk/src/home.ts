// Home's body (screen `desk`, the id 0.2 users' Settings → Home already names). Three bands, top
// to bottom, in a column the app trims to the page (never a pager):
//
//   BOOK  [cover] title / authors / bar / 13% · today     PAPER  [cover] …       the pair
//   PINNED                                     All 9 ›     one row of covers
//   INBOX · 4 NEW                              All ›       text lines            (or the passage)
//
// What doesn't fit is left out from the end, which is the order of sacrifice wanted: the passage,
// then inbox lines, then the pinned row. zk.display only decides what trimming can't: how many
// covers a row holds, and whether a Scribe's extra room takes a third line and the passage.

import {
  authorsOf,
  bylineOf,
  countOf,
  inboxItems,
  inboxOf,
  isBook,
  keptPassage,
  loadPins,
  loadPrefs,
  openPassage,
  openedPair,
  percentOf,
  pinnedItems,
  ref,
  same,
  whenOf,
} from "./data";
import { heading, lines, pair, shelfRow } from "./pieces";

/** Covers a row passes the app (it draws what fits: 5 on a Paperwhite, 7 on a Scribe). */
const PINS_SHOWN = 8;

/** A Scribe's height: room for a third inbox line and the passage under the inbox. */
const tall = () => zk.display.height >= 2000;
/** Between the bands: closer on a basic Kindle's short screen, so the inbox keeps a line. */
const bandGap = (): zk.ui.Gap => (zk.display.height < 1400 ? "medium" : "large");
/** About 200 px a cover: 5 on a Paperwhite or a basic Kindle, 7 on a Scribe. */
const coverMinWidth = (): zk.ui.Percent => (zk.display.width >= 1600 ? "14%" : "17%");

const FIRST_USE =
  "Open a book or a paper and it will wait for you here. Pin things from an item's menu: long-press → More… → Pin to Home.";

interface Side {
  kicker: string;
  item: zk.Item;
  started: boolean;
}

/** The pair's meta line: "13% · today" once opened, "Not started" for a pin up next. */
function sideMeta(s: Side): string {
  if (!s.started) return "Not started";
  return [percentOf(s.item), whenOf(s.item.openedAt)].filter(Boolean).join(" · ");
}

export function registerHome() {
  zk.screen("desk", {
    title: "Reading Desk",
    home: true,
    render: async () => {
      const prefs = await loadPrefs();
      const inbox = inboxOf(prefs);
      const [opened, pins, unread] = await Promise.all([openedPair(), loadPins(), inbox ? inboxItems(inbox) : null]);
      const pinned = await pinnedItems(pins);

      // The pair: what is being read of each kind; else that kind's first pin never opened and not read.
      const upNext = (book: boolean) => pinned.find((i) => isBook(i) === book && i.openedAt === null && !i.read);
      const side = (item: zk.Item | undefined, kicker: string, book: boolean): Side | undefined => {
        if (item) return { kicker, item, started: true };
        const next = upNext(book);
        return next && { kicker: "Up next", item: next, started: false };
      };
      const sides = [side(opened.book, "Book", true), side(opened.paper, "Paper", false)].filter((s): s is Side => !!s);
      const inPair = (i: zk.Item) => sides.some((s) => same(ref(s.item), ref(i)));
      const wide = sides.length === 1;
      const pairBand =
        sides.length > 0 &&
        zk.ui.row(
          sides.map((s) =>
            pair({ kicker: s.kicker, item: s.item, authors: authorsOf(s.item, { most: 2, full: true }), meta: sideMeta(s), wide }),
          ),
          { align: "top", gap: "medium" },
        );

      const shelf = pinned.filter((i) => !inPair(i)).slice(0, PINS_SHOWN);
      const pinnedBand =
        shelf.length > 0 &&
        zk.ui.column([heading("Pinned", { id: "all-pinned", text: `All ${pinned.length} ›` }), shelfRow(shelf, coverMinWidth())]);

      const fresh = (unread ?? []).filter((i) => !inPair(i));
      const inboxBand =
        fresh.length > 0 &&
        zk.ui.column(
          [
            heading(`Inbox · ${countOf(fresh.length, unread!.length)} new`, { id: "all-inbox", text: "All ›" }),
            ...lines(fresh.slice(0, tall() ? 3 : 2).map((item) => ({ item, byline: bylineOf(item), right: whenOf(item.addedAt) }))),
          ],
          { gap: "small" },
        );

      // The passage takes the inbox's band when there is none, and goes under it on a Scribe.
      const pick = prefs.passage === false || (inboxBand && !tall()) ? undefined : await keptPassage();
      const firstUse = !pairBand && !pinnedBand && !inboxBand;
      return zk.ui.column(
        [
          pairBand,
          pinnedBand,
          inboxBand,
          firstUse && zk.ui.text(FIRST_USE, { tone: "secondary" }),
          pick && zk.ui.quote({ id: "passage", text: pick.text, source: pick.source }),
        ],
        { gap: bandGap(), fit: "trim" },
      );
    },
    onEvent: async (e) => {
      if (e.kind !== "tap") return;
      if (e.id === "all-pinned") zk.ui.open("all", { tab: "pinned" });
      else if (e.id === "all-inbox") zk.ui.open("all", { tab: "inbox" });
      else if (e.id === "passage") await openPassage();
    },
  });
}
