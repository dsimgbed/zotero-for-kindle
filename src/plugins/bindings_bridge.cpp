// zk.bridge: the Grimoire × Zotero handoff surface (docs/bridge-contract.md). A plugin with the
// "grimoire:bridge" permission may queue a citation, read the page to return to, refresh the read-only
// library index and ask for a clean exit 43. There is deliberately no path, file or URL argument: all
// filesystem access stays inside the bridge module and its fixed, validated schema.

#include "plugins/plugin_ctx.h"

#include <nlohmann/json.hpp>

#include "bridge/grimoire_bridge.h"
#include "store/library.h"

namespace zk::plugins {
namespace {

using json = nlohmann::json;

constexpr const char* kPermission = "grimoire:bridge";

bool hasBridgePermission(PluginCtx& p) {
    return p.manifest.permissions.count(Permission::GrimoireBridge) != 0;
}

JSValue needBridge(PluginCtx& p, const char* fn) {
    if (!hasBridgePermission(p)) return throwPermissionError(p, kPermission, fn);
    if (!bridge::bridgeActive())
        return JS_ThrowPlainError(p.cx.raw(), "%s: the Grimoire bridge isn't available", fn);
    return JS_UNDEFINED;
}

std::string argString(JSContext* c, JSValueConst v) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return {};
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) return {};
    std::string out(s, len);
    JS_FreeCString(c, s);
    return out;
}

const json* field(const json& j, const char* snake, const char* camel) {
    auto it = j.find(snake);
    if (it != j.end()) return &*it;
    it = j.find(camel);
    return it == j.end() ? nullptr : &*it;
}

std::string stringField(const json& j, const char* snake, const char* camel) {
    const json* v = field(j, snake, camel);
    return v && v->is_string() ? v->get<std::string>() : std::string();
}

bool uintFieldOptional(const json& j, const char* snake, const char* camel, uint32_t& out) {
    const json* v = field(j, snake, camel);
    if (!v || v->is_null()) {
        out = 0;
        return true;
    }
    if (!v->is_number_unsigned() || v->get<uint64_t>() > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(v->get<uint64_t>());
    return true;
}

std::string yearField(const json& j) {
    const json* v = field(j, "year", "year");
    if (!v || v->is_null()) return {};
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return std::to_string(v->get<int64_t>());
    return {};
}

JSValue jsAvailable(JSContext* c, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(c, bridge::bridgeActive());
}

JSValue jsSendCitation(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    if (JSValue denied = needBridge(p, "zk.bridge.sendCitation"); JS_IsException(denied)) return denied;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(c, "zk.bridge.sendCitation: expected a citation object");
    const std::string raw = argString(c, argv[0]);
    const json j = json::parse(raw, nullptr, false);
    if (!j.is_object()) return JS_ThrowTypeError(c, "zk.bridge.sendCitation: expected a citation object");

    bridge::CitationPayload cite;
    cite.id = stringField(j, "id", "id");
    cite.itemKey = stringField(j, "item_key", "itemKey");
    cite.attachmentKey = stringField(j, "attachment_key", "attachmentKey");
    cite.shortCite = stringField(j, "short_cite", "shortCite");
    cite.title = stringField(j, "title", "title");
    cite.authors = stringField(j, "authors", "authors");
    cite.year = yearField(j);
    cite.locator = stringField(j, "locator", "locator");
    cite.quote = stringField(j, "quote", "quote");
    cite.note = stringField(j, "note", "note");
    if (const json* v = field(j, "created_at", "createdAt"); v && v->is_number_integer())
        cite.createdAt = v->get<int64_t>();
    if (!uintFieldOptional(j, "page", "page", cite.page))
        return JS_ThrowRangeError(c, "zk.bridge.sendCitation: page must be a non-negative integer");

    std::string id;
    std::string why;
    if (!bridge::queueCitation(cite, {}, &id, &why))
        return JS_ThrowRangeError(c, "zk.bridge.sendCitation: %s", why.c_str());
    JSValue out = JS_NewObject(c);
    if (JS_IsException(out)) return out;
    JS_SetPropertyStr(c, out, "ok", JS_TRUE);
    JS_SetPropertyStr(c, out, "id", JS_NewString(c, id.c_str()));
    return out;
}

JSValue jsGetReturnTarget(JSContext* c, JSValueConst, int, JSValueConst*) {
    PluginCtx& p = ctxOf(c);
    if (JSValue denied = needBridge(p, "zk.bridge.getReturnTarget"); JS_IsException(denied)) return denied;
    const auto ctx = bridge::readReturnContext({}, nullptr);
    if (!ctx || ctx->notebookId.empty()) return JS_NULL;
    JSValue out = JS_NewObject(c);
    if (JS_IsException(out)) return out;
    JS_SetPropertyStr(c, out, "notebook_id", JS_NewString(c, ctx->notebookId.c_str()));
    JS_SetPropertyStr(c, out, "page_id", JS_NewUint32(c, ctx->pageId));
    JS_SetPropertyStr(c, out, "page_index", JS_NewUint32(c, ctx->pageIndex));
    return out;
}

JSValue jsReturnToGrimoire(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    if (JSValue denied = needBridge(p, "zk.bridge.returnToGrimoire"); JS_IsException(denied)) return denied;
    bridge::ReturnContext target = bridge::readReturnContext({}, nullptr).value_or(bridge::ReturnContext{});
    if (argc >= 1 && JS_IsString(argv[0])) {
        const json j = json::parse(argString(c, argv[0]), nullptr, false);
        if (j.is_object()) {
            const std::string nb = stringField(j, "notebook_id", "notebookId");
            if (!nb.empty()) {
                target.notebookId = nb;
                uintFieldOptional(j, "page_id", "pageId", target.pageId);
                uintFieldOptional(j, "page_index", "pageIndex", target.pageIndex);
            }
        }
    }
    std::string why;
    if (!bridge::writeReturnToGrimoireHandoff(target, {}, &why))
        return JS_ThrowPlainError(c, "zk.bridge.returnToGrimoire: %s", why.c_str());
    PluginHostEvent ev{};
    ev.kind = PluginHostEvent::Kind::BridgeReturn;
    post(p, std::move(ev));
    return JS_TRUE;
}

JSValue jsRefreshIndex(JSContext* c, JSValueConst, int, JSValueConst*) {
    PluginCtx& p = ctxOf(c);
    if (JSValue denied = needBridge(p, "zk.bridge.refreshIndex"); JS_IsException(denied)) return denied;
    if (!requirePermission(p, Permission::LibraryRead, "zk.bridge.refreshIndex")) return JS_EXCEPTION;
    std::string why;
    Library* lib = sharedLibrary(p, &why);
    if (!lib) return JS_ThrowPlainError(c, "zk.bridge.refreshIndex: %s", why.c_str());
    size_t count = 0;
    if (!bridge::exportLibraryIndex(*lib, {}, &why, &count))
        return JS_ThrowPlainError(c, "zk.bridge.refreshIndex: %s", why.c_str());
    JSValue out = JS_NewObject(c);
    if (JS_IsException(out)) return out;
    JS_SetPropertyStr(c, out, "item_count", JS_NewInt64(c, static_cast<int64_t>(count)));
    return out;
}

constexpr char kBridgeJs[] = R"JS((function (n, zk) {
"use strict";
const bridge = Object.freeze({
  available() { return !!n.available(); },
  sendCitation(citation) {
    let s;
    try { s = JSON.stringify(citation); } catch (e) { s = undefined; }
    return n.sendCitation(typeof s === "string" ? s : "null");
  },
  returnToGrimoire(opts) {
    if (opts === undefined || opts === null) return n.returnToGrimoire();
    let s;
    try { s = JSON.stringify(opts); } catch (e) { s = undefined; }
    return n.returnToGrimoire(typeof s === "string" ? s : "null");
  },
  getReturnTarget() { return n.getReturnTarget(); },
  refreshIndex() { return n.refreshIndex(); },
});
Object.defineProperty(zk, "bridge", { value: bridge, enumerable: true });
}))JS";

}  // namespace

void installBridge(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction<jsAvailable>(c, natives.raw(), "available", 0);
    defineFunction<jsSendCitation>(c, natives.raw(), "sendCitation", 1);
    defineFunction<jsGetReturnTarget>(c, natives.raw(), "getReturnTarget", 0);
    defineFunction<jsReturnToGrimoire>(c, natives.raw(), "returnToGrimoire", 0);
    defineFunction<jsRefreshIndex>(c, natives.raw(), "refreshIndex", 0);
    const char* name = "<zk bridge>";
    js::Value fn(c, JS_Eval(c, kBridgeJs, sizeof kBridgeJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) throw std::runtime_error(std::string(name) + ": the prelude doesn't compile");
    p.cx.call(fn, {natives, p.zk});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
}

}  // namespace zk::plugins
