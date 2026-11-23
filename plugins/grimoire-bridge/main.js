// Grimoire Bridge — queue citations and quotes from Zotero for Kindle into Grimoire, and hand
// back to the essay being written there.
//
// Runs in QuickJS-ng as plain ES2020 with no build step: the module-level calls at the bottom
// register this plugin's actions and screen on the `zk` global the app provides.
//
// The native bridge this drives (the app's side of docs/bridge-contract.md):
//   zk.bridge.available()              -> boolean
//   zk.bridge.sendCitation(payload)   -> { ok: boolean, id: string }
//   zk.bridge.returnToGrimoire(opts)  -> boolean
//   zk.bridge.getReturnTarget()        -> { notebook_id, page_id, page_index } | null
//   zk.bridge.refreshIndex()           -> { item_count: number }
//
// Zero-credential invariant: nothing here reads or writes credentials; the bridge owns the
// handoff files (handoff.json, return_context.json, library_index.json, inbox/) and the log.

var MAX_QUOTE = 4000;

function bridge() {
  if (typeof zk === "undefined" || zk === null) return null;
  if (zk.bridge === null || zk.bridge === undefined) return null;
  return zk.bridge;
}

function bridgeAvailable() {
  var b = bridge();
  if (b === null || b === undefined) return false;
  if (typeof b.available !== "function") return false;
  try {
    return b.available() === true;
  } catch (e) {
    return false;
  }
}

function bridgeReturnTarget() {
  var b = bridge();
  if (b === null || b === undefined) return null;
  if (typeof b.getReturnTarget !== "function") return null;
  try {
    var t = b.getReturnTarget();
    if (t && t.notebook_id) return t;
    return null;
  } catch (e) {
    return null;
  }
}

function bridgeRefreshCount() {
  var b = bridge();
  if (b === null || b === undefined) return null;
  if (typeof b.refreshIndex !== "function") return null;
  try {
    var r = b.refreshIndex();
    if (r && typeof r.item_count === "number") return r.item_count;
    return null;
  } catch (e) {
    return null;
  }
}

function bridgeSend(payload) {
  var b = bridge();
  if (b === null || b === undefined) return null;
  if (typeof b.sendCitation !== "function") return null;
  try {
    var r = b.sendCitation(payload);
    if (r && r.ok === true) return r;
    return null;
  } catch (e) {
    return null;
  }
}

function bridgeReturn() {
  var b = bridge();
  if (b === null || b === undefined) return false;
  if (typeof b.returnToGrimoire !== "function") return false;
  try {
    var ok = b.returnToGrimoire();
    return ok === true;
  } catch (e) {
    return false;
  }
}

function attachmentKeyOf(sel) {
  if (!sel) return "";
  var file = sel.attachment;
  if (!file && sel.file) file = sel.file;
  if (file && file.key) return String(file.key);
  return "";
}

function quoteText(text) {
  var q = typeof text === "string" ? text : "";
  var replaced = q.replace(/[\r\n\t]+/g, " ");
  var clean = replaced.replace(/[\u0000-\u001f\u007f]/g, "").replace(/ +/g, " ").trim();
  if (clean.length > MAX_QUOTE) clean = clean.slice(0, MAX_QUOTE);
  return clean;
}

function pageNumber(pageLabel, page) {
  if (typeof pageLabel === "string" && pageLabel.length > 0) {
    var digits = pageLabel.replace(/\s+/g, "");
    if (/^[0-9]+$/.test(digits)) {
      var n = parseInt(digits, 10);
      if (n > 0) return n;
    }
  }
  if (typeof page === "number" && page > 0) return Math.floor(page);
  return 0;
}

function authorNames(item) {
  var names = [];
  var creators = item && Array.isArray(item.creators) ? item.creators : [];
  for (var i = 0; i < creators.length; i++) {
    var c = creators[i];
    if (!c) continue;
    var isAuthor = true;
    if (c.type) isAuthor = String(c.type) === "author";
    if (!isAuthor) continue;
    var full = c.name ? String(c.name) : [c.first, c.last].filter(Boolean).join(" ");
    if (!full) continue;
    var toks = full.split(/\s+/);
    names.push(toks[toks.length - 1]);
  }
  return names;
}

function authorCite(names) {
  if (!names || names.length === 0) return "";
  if (names.length === 1) return names[0];
  if (names.length === 2) return names[0] + " & " + names[1];
  return names[0] + " et al.";
}

function formatShortCite(item, pageLabel, page) {
  var names = authorNames(item);
  var short = authorCite(names);
  var year = item && typeof item.year === "number" ? item.year : null;
  var cite = "";
  if (short && year) cite = short + " " + year;
  else if (item && typeof item.title === "string" && item.title) cite = item.title;
  else cite = "Untitled";
  var p = pageNumber(pageLabel, page);
  if (p > 0) cite = cite + ", p. " + p;
  return cite;
}

var screenState = { available: false, target: null, count: null };

zk.screen("grimoire-bridge", {
  title: "Grimoire Bridge",
  main: true,
  render: function () {
    var available = bridgeAvailable();
    var target = available ? bridgeReturnTarget() : null;
    screenState = { available: available, target: target, count: screenState.count };
    var children = [
      zk.ui.text(available ? "Grimoire bridge: ready" : "Grimoire bridge: unavailable", { style: "title" }),
      zk.ui.text("Citations and quotes you queue go to Grimoire, and back to your essay.", { style: "meta" }),
      zk.ui.notice(
        target
          ? "Grimoire: " + (target.notebook_id || "") + ", page " + ((target.page_index || 0) + 1)
          : "No Grimoire essay to return to yet."
      ),
      zk.ui.text(
        "Library index: " + (screenState.count === null ? "tap Refresh to update" : screenState.count + " items"),
        { style: "meta" }
      ),
      zk.ui.button({ id: "switch", title: "Switch to Grimoire", primary: true }),
      zk.ui.button({ id: "refresh", title: "Refresh Library Index" }),
    ];
    return zk.ui.column(children, { gap: "small" });
  },
  onEvent: function (e) {
    if (e.kind !== "tap") return;
    if (e.id === "switch") {
      bridgeReturn();
      return;
    }
    if (e.id === "refresh") {
      screenState = {
        available: bridgeAvailable(),
        target: bridgeReturnTarget(),
        count: bridgeRefreshCount(),
      };
      if (typeof zk !== "undefined" && zk.ui && typeof zk.ui.refresh === "function") {
        zk.ui.refresh();
      }
      return;
    }
  },
});

zk.selectionAction({
  id: "cite-to-grimoire",
  title: "Cite to Grimoire",
  run: async function (target, again) {
    var sel = target;
    if (again && again.action === "open-grimoire") {
      bridgeReturn();
      return "Switching to Grimoire…";
    }
    var item = sel ? sel.item : null;
    if (!item) {
      return "This passage is not in your library yet: open the book or article in your library, then cite it from there.";
    }
    if (!bridgeAvailable()) {
      return "The Grimoire bridge is not available: start Grimoire and try again.";
    }
    var text = "";
    var pageLabel = null;
    var page = 0;
    if (sel) {
      text = quoteText(sel.text);
      pageLabel = sel.pageLabel ? String(sel.pageLabel) : null;
      page = pageNumber(pageLabel, sel.page);
    }
    var shortCite = formatShortCite(item, pageLabel, page);
    var loc = "";
    if (pageLabel && String(pageLabel).trim().length > 0) loc = String(pageLabel).trim();
    else if (page > 0) loc = "p. " + page;
    var payload = {
      item_key: item.key,
      attachment_key: attachmentKeyOf(sel),
      short_cite: shortCite,
      title: item.title ? String(item.title) : "",
      authors: authorCite(authorNames(item)),
      year: item.year != null ? String(item.year) : "",
      page: page,
      locator: loc,
      quote: text,
      note: "",
    };
    var queued = bridgeSend(payload);
    if (!queued) {
      return "Grimoire could not take the citation: the bridge queue is not accepting.";
    }
    var out = "Queued citation for Grimoire:\n" + shortCite;
    if (text) out = out + "\n\n" + text;
    return { text: out, actions: [{ id: "open-grimoire", title: "Open Grimoire" }] };
  },
});

zk.itemAction({
  id: "cite-item-to-grimoire",
  title: "Cite to Grimoire",
  run: async function (item, again) {
    if (again && again.action === "open-grimoire") {
      bridgeReturn();
      return "Switching to Grimoire…";
    }
    if (!item) {
      return "No item to cite.";
    }
    if (!bridgeAvailable()) {
      return "The Grimoire bridge is not available: start Grimoire and try again.";
    }
    var shortCite = formatShortCite(item, null, 0);
    var payload = {
      item_key: item.key,
      attachment_key: "",
      short_cite: shortCite,
      title: item.title ? String(item.title) : "",
      authors: authorCite(authorNames(item)),
      year: item.year != null ? String(item.year) : "",
      page: 0,
      locator: "",
      quote: "",
      note: "",
    };
    var queued = bridgeSend(payload);
    if (!queued) {
      return "Grimoire could not take the citation: the bridge queue is not accepting.";
    }
    return { text: "Queued citation for Grimoire:\n" + shortCite, actions: [{ id: "open-grimoire", title: "Open Grimoire" }] };
  },
});
