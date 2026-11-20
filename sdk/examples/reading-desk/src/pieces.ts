// The pieces Reading Desk is composed of, each from the app's primitives (zk.ui): a section
// label, a heading with a link, an item beside its cover (the pair's side), a row of covers, a
// list of text lines, a segmented switch, and a grid of captioned covers. The app draws them in
// its own type and greys; nothing here is a pixel.

import { ref } from "./data";

type Block = zk.ui.Block;

/** A section label: small sans caps, tracked, secondary ("PINNED", "BOOK"). */
export const label = (text: string) => zk.ui.text(text, { style: "label", caps: true });

/** A serif bold title, cut at `lines`. */
const title = (text: string, size: "xsmall" | "small" | "body", lines: number) =>
  zk.ui.text(text || "Untitled", { font: "serif", weight: "bold", size, lines });

/** Secondary sans text, one line unless said. */
const meta = (text: string, size: "xsmall" | "small" = "small", lines = 1) =>
  zk.ui.text(text, { font: "sans", size, tone: "secondary", lines });

/** A heading: its label on the left, a link on the right ("All 9 ›"), tapped by `link.id`. */
export function heading(text: string, link?: { id: string; text: string }): Block {
  return zk.ui.row([
    label(text),
    link && zk.ui.text(link.text, { id: link.id, width: "auto", font: "sans", size: "body", weight: "medium", tone: "ink" }),
  ]);
}

/**
 * The pair's side: a kicker over the item's cover, and beside it the title (up to 3 lines), the
 * authors, and at the bottom the bar (once started) and a meta line. `wide`: the side has the
 * whole width, so its cover takes a smaller share of it.
 */
export function pair(side: { kicker: string; item: zk.Item; authors: string; meta: string; wide: boolean }): Block {
  const { item } = side;
  return zk.ui.column([
    label(side.kicker),
    zk.ui.item(
      ref(item),
      zk.ui.row(
        [
          zk.ui.cover(ref(item), { width: side.wide ? "18%" : "37%" }),
          zk.ui.column(
            [
              title(item.title, "body", 3),
              side.authors && meta(side.authors, "small", 2),
              zk.ui.spacer(),
              item.progress !== null && zk.ui.progress(item.progress),
              meta(side.meta),
            ],
            { gap: "xsmall" },
          ),
        ],
        { align: "stretch", gap: "medium" },
      ),
    ),
  ]);
}

/**
 * One row of small covers (never more: what doesn't fit isn't drawn), each an item with a bar
 * under it once it was opened; no captions. `minWidth`: the narrowest a tile gets.
 */
export function shelfRow(items: zk.Item[], minWidth: zk.ui.Percent): Block {
  return zk.ui.grid(
    items.map((i) =>
      zk.ui.item(ref(i), zk.ui.column([zk.ui.cover(ref(i)), i.progress !== null && zk.ui.progress(i.progress)])),
    ),
    { columns: "auto", minWidth, rows: 1, gap: "medium" },
  );
}

/** Text lines, one an item: its title (one line) over its byline, and on the right a short word. */
export function lines(rows: { item: zk.Item; byline: string; right: string }[]): Block[] {
  return rows.map(({ item, byline, right }) =>
    zk.ui.item(
      ref(item),
      zk.ui.row(
        [
          zk.ui.column([title(item.title, "body", 1), byline && meta(byline)], { gap: "xsmall" }),
          right && zk.ui.text(right, { width: "auto", font: "sans", size: "small", tone: "secondary" }),
        ],
        { align: "top", gap: "medium" },
      ),
    ),
  );
}

/** A segmented switch: one framed row of segments, the selected one in ink; a tap sends its id. */
export function segmented(segments: { id: string; text: string }[], selected: string): Block {
  return zk.ui.box(
    zk.ui.row(
      segments.map((s) =>
        zk.ui.box(zk.ui.text(s.text, { align: "center", font: "sans", size: "body", weight: "medium" }), {
          id: s.id,
          pad: "small",
          fill: s.id === selected ? "ink" : "none",
          radius: "small",
        }),
      ),
      { gap: "none" },
    ),
    { border: "thin", radius: "small" },
  );
}

/** Covers in a grid that pages, each with its bar once opened, its title and a caption under it. */
export function coverGrid(cells: { item: zk.Item; caption: string }[]): Block {
  return zk.ui.grid(
    cells.map(({ item, caption }) =>
      zk.ui.item(
        ref(item),
        zk.ui.column([
          zk.ui.cover(ref(item)),
          item.progress !== null && zk.ui.progress(item.progress),
          zk.ui.column([title(item.title, "small", 1), caption && meta(caption, "xsmall")], { gap: "xsmall" }),
        ]),
      ),
    ),
    { columns: "auto", minWidth: "22%", gap: "large" },
  );
}
