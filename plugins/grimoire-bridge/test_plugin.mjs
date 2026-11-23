import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const dir = path.dirname(fileURLToPath(import.meta.url));
const assert = (cond, msg) => { if (!cond) throw new Error("FAIL: " + msg); };

// --- plugin.json schema ---
const plugin = JSON.parse(fs.readFileSync(path.join(dir, "plugin.json"), "utf8"));
assert(plugin.id === "grimoire-bridge", "id");
assert(plugin.name === "Grimoire Bridge", "name");
assert(plugin.version === "0.1.0", "version");
assert(plugin.api === 0, "api");
assert(plugin.minApp === "0.14.0", "minApp");
assert(plugin.author === "Grimoire x Zotero Bridge", "author");
assert(plugin.description === "Send citations and quotes to Grimoire and switch seamlessly back to your active essay.", "description");
assert(plugin.main === "main.js", "main");
assert(Array.isArray(plugin.permissions) && plugin.permissions.join(",") === "library:read,grimoire:bridge", "permissions");

function makeZk(bridgeImpl) {
  const reg = { screens: {}, selectionActions: {}, itemActions: {} };
  const ui = {
    text: (t, o) => ({ kind: "text", text: t, ...(o || {}) }),
    notice: (t, o) => ({ kind: "notice", text: t, ...(o || {}) }),
    button: (b) => ({ kind: "button", ...b }),
    column: (children, o) => ({ kind: "column", children: children.filter(Boolean), ...(o || {}) }),
  };
  const zk = {
    bridge: bridgeImpl,
    ui,
    screen: (id, spec) => { reg.screens[id] = spec; },
    selectionAction: (a) => { reg.selectionActions[a.id] = a; },
    itemAction: (a) => { reg.itemActions[a.id] = a; },
  };
  return { zk, reg };
}

function loadMain(zk) {
  globalThis.zk = zk;
  const code = fs.readFileSync(path.join(dir, "main.js"), "utf8");
  const fn = new Function(code);
  fn();
}

// --- happy path bridge ---
let sendCalls = [];
let returnCalls = 0;
let refreshCalls = 0;
const happyBridge = {
  available: () => true,
  sendCitation: (p) => { sendCalls.push(p); return { ok: true, id: "q1" }; },
  returnToGrimoire: (o) => { returnCalls++; return true; },
  getReturnTarget: () => ({ notebook_id: "nb1", page_id: 7, page_index: 3 }),
  refreshIndex: () => { refreshCalls++; return { item_count: 42 }; },
};

const { zk, reg } = makeZk(happyBridge);
loadMain(zk);

assert(reg.selectionActions["cite-to-grimoire"], "selectionAction registered");
assert(reg.itemActions["cite-item-to-grimoire"], "itemAction registered");
assert(reg.screens["grimoire-bridge"], "screen registered");

// format tests via itemAction
const kuhn = { key: "K1", title: "The Structure of Scientific Revolutions", creators: [{ type: "author", last: "Kuhn" }], year: 1962 };
let r = await reg.itemActions["cite-item-to-grimoire"].run(kuhn);
assert(typeof r === "object" && r.text.includes("Kuhn 1962"), "Kuhn short cite: " + r.text);
assert(r.actions && r.actions[0].id === "open-grimoire", "itemAction actions");

const adda = { key: "A1", title: "T", creators: [{ type: "author", last: "Adda" }, { type: "author", last: "Cooper" }], year: 2003 };
r = await reg.itemActions["cite-item-to-grimoire"].run(adda);
assert(r.text.includes("Adda & Cooper 2003"), "Adda & Cooper: " + r.text);

const raissi = { key: "R1", title: "T", creators: [{ type: "author", last: "Raissi" }, { type: "author", last: "X" }, { type: "author", last: "Y" }], year: 2019 };
r = await reg.itemActions["cite-item-to-grimoire"].run(raissi);
assert(r.text.includes("Raissi et al. 2019"), "Raissi et al: " + r.text);

// selectionAction initial cite
sendCalls = [];
const longQuote = "a".repeat(5000);
const sel = { item: kuhn, attachment: { key: "ATT1" }, text: longQuote, pageLabel: "42", page: null };
r = await reg.selectionActions["cite-to-grimoire"].run(sel);
assert(typeof r === "object" && r.text.includes("Queued citation for Grimoire:"), "selection returns object");
assert(r.text.includes("Kuhn 1962, p. 42"), "selection short cite with page");
assert(sendCalls.length === 1, "sendCitation called");
const payload = sendCalls[0];
assert(payload.item_key === "K1", "item_key");
assert(payload.attachment_key === "ATT1", "attachment_key");
assert(payload.quote.length <= 4000, "quote truncated: " + payload.quote.length);
assert(payload.page === 42, "page extracted: " + payload.page);
assert(payload.short_cite === "Kuhn 1962, p. 42", "short_cite");

// open-grimoire again
const againRes = await reg.selectionActions["cite-to-grimoire"].run(sel, { action: "open-grimoire", text: "x" });
assert(againRes === "Switching to Grimoire…", "again action: " + againRes);
assert(returnCalls >= 1, "returnToGrimoire called");

// screen render + actions
const screen = reg.screens["grimoire-bridge"];
const tree = screen.render();
assert(tree.kind === "column", "screen renders column");
const txt = JSON.stringify(tree);
assert(txt.includes("Grimoire bridge: ready"), "bridge availability shown");
assert(txt.includes("nb1"), "return target shown");
assert(refreshCalls === 0, "render does not eagerly write library index");
// refresh action
screen.onEvent({ kind: "tap", id: "refresh" });
assert(refreshCalls === 1, "refresh called on tap");
const treeAfter = screen.render();
assert(JSON.stringify(treeAfter).includes("42 items"), "refreshed count shown");
// switch action
const before = returnCalls;
screen.onEvent({ kind: "tap", id: "switch" });
assert(returnCalls === before + 1, "switch calls returnToGrimoire");

// missing bridge graceful handling
const mb = makeZk(null);
// simulate zk.bridge unavailable by passing null; but main reads global zk.bridge at runtime
loadMain(mb.zk);
const sa = mb.reg.selectionActions["cite-to-grimoire"];
const resMissing = await sa.run({ item: kuhn, text: "q", pageLabel: null });
assert(typeof resMissing === "string" && resMissing.includes("not available"), "missing bridge selection: " + resMissing);
const ia = mb.reg.itemActions["cite-item-to-grimoire"];
const resMissing2 = await ia.run(kuhn);
assert(typeof resMissing2 === "string" && resMissing2.includes("not available"), "missing bridge item: " + resMissing2);
const sc = mb.reg.screens["grimoire-bridge"];
const tree2 = sc.render();
assert(JSON.stringify(tree2).includes("unavailable"), "missing bridge screen");

// preview without item
const { zk: zk2, reg: reg2 } = makeZk(happyBridge);
loadMain(zk2);
const resPreview = await reg2.selectionActions["cite-to-grimoire"].run({ item: null, text: "q", pageLabel: null });
assert(typeof resPreview === "string" && resPreview.includes("not in your library"), "preview message: " + resPreview);

// multi-line / tabbed quote normalization & word boundary preservation + EPUB page fallback
sendCalls = [];
const multiLineText = "First line\r\nsecond line\tthird\n\nline   with   spaces\x01\x02\x7f";
const epubSel = {
  item: kuhn,
  attachment: { key: "ATT_EPUB" },
  text: multiLineText,
  pageLabel: "",
  page: 27,
};
const resEpub = await reg2.selectionActions["cite-to-grimoire"].run(epubSel);
assert(typeof resEpub === "object", "epub selection succeeds");
assert(sendCalls.length === 1, "sendCitation called for epub");
const epubPayload = sendCalls[0];
assert(epubPayload.quote === "First line second line third line with spaces", "quote normalization preserved word boundaries: " + JSON.stringify(epubPayload.quote));
assert(epubPayload.page === 27, "epub page extracted: " + epubPayload.page);
assert(epubPayload.locator === "p. 27", "epub locator formatted: " + epubPayload.locator);
assert(epubPayload.short_cite === "Kuhn 1962, p. 27", "epub short_cite with page fallback: " + epubPayload.short_cite);

console.log("ALL TESTS PASSED");
console.log("plugin.json valid; main.js loaded; selection/item/screen/missing-bridge/truncation/multi-author/quoteText/epubPage OK");
