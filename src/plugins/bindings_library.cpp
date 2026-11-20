// zk.library: reading the library. The plugin thread opens its own connection to the library
// database at the first call (SQLite wants one per thread, as the sync worker has) and reads
// through Library. Answers are plain JSON (the Item shape is identifiers.h's itemJson).
//
// Pages: items() and recentAnnotations() are async iterables over a native that returns one page
// (50 rows) per call, so a library is walked without being loaded.
//
// findByIdentifier uses a small in-memory index (DOI, arXiv id and ISBN → item keys) built on the
// plugin thread the first time it is needed, from one pass over every item's JSON. It is rebuilt
// when the database changed under us: SQLite's data_version moves whenever another connection
// (the sync worker, the screen thread's writes) commits, and costs nothing to ask. The version is
// read before the pass, so a commit during it only makes the next call rebuild. A rebuild is about
// one JSON parse per item, which a library of several thousand does in well under a second.
//
// fullText is host work: the engine takes seconds on a book, so one worker thread (shared by all
// plugins, started at the first use) extracts while the plugin thread serves others. Jobs run one
// at a time (crengine serialises engines anyway); a job whose call was already stopped is skipped.

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "plugins/events.h"
#include "plugins/fulltext.h"
#include "plugins/identifiers.h"
#include "plugins/plugin_ctx.h"
#include "store/library.h"

namespace zk::plugins {

namespace {

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

constexpr int kSearchLimit = 100;

struct LibState {
    std::unique_ptr<Db> db;
    std::unique_ptr<Library> lib;
    bool indexed = false;
    int64_t dataVersion = 0;
    std::unordered_map<std::string, std::vector<std::string>> byDoi, byArxiv, byIsbn;
};

// The shared state, with a connection; null if there is no library.
LibState* openState(PluginCtx& p) {
    auto& slot = librarySlot(p);
    if (!slot) {
        const std::string& path = depsOf(p).dbPath;
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec)) return nullptr;
        auto st = std::make_shared<LibState>();
        st->db = std::make_unique<Db>(Db::open(path));
        st->lib = std::make_unique<Library>(*st->db);
        slot = st;
    }
    return static_cast<LibState*>(slot.get());
}

// The same, with a JS error thrown if there is no library.
LibState* stateOf(JSContext* c, PluginCtx& p) {
    LibState* st = openState(p);
    if (!st) JS_ThrowPlainError(c, "zk.library: the library isn't available");
    return st;
}

struct Op {
    const char* name;
    Permission perm;
    const char* detail;  // how the permission error names the call
};
const Op kOps[] = {
    {"item", Permission::LibraryRead, "zk.library.item()"},
    {"page", Permission::LibraryRead, "zk.library.items()"},
    {"search", Permission::LibraryRead, "zk.library.search()"},
    {"find", Permission::LibraryRead, "zk.library.findByIdentifier()"},
    {"collections", Permission::LibraryRead, "zk.library.collections()"},
    {"libraries", Permission::LibraryRead, "zk.library.libraries()"},
    {"feeds", Permission::LibraryRead, "zk.library.feeds()"},
    {"tags", Permission::LibraryRead, "zk.library.tags()"},
    {"attachments", Permission::LibraryRead, "zk.library.attachments()"},
    {"annotations", Permission::AnnotationsRead, "zk.library.annotations()"},
    {"recent", Permission::AnnotationsRead, "zk.library.recentAnnotations()"},
    {"notes", Permission::NotesRead, "zk.library.notes()"},
    {"fullText", Permission::FulltextRead, "zk.library.fullText()"},
    {"open", Permission::LibraryRead, "zk.library.open()"},
};

const Op* opNamed(const std::string& name) {
    for (const Op& o : kOps)
        if (name == o.name) return &o;
    return nullptr;
}

bool text(JSContext* c, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(c, "zk.library: a string is needed");
        return false;
    }
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) return false;
    out = sanitisedUtf8(s, len);
    JS_FreeCString(c, s);
    return true;
}

bool arg(JSContext* c, int argc, JSValueConst* argv, int i, std::string& out) {
    if (i >= argc) {
        JS_ThrowTypeError(c, "zk.library: an argument is missing");
        return false;
    }
    return text(c, argv[i], out);
}

bool intArg(JSContext* c, int argc, JSValueConst* argv, int i, int& out) {
    int32_t v = 0;
    if (i >= argc || JS_ToInt32(c, &v, argv[i]) < 0) {
        JS_ThrowTypeError(c, "zk.library: a number is needed");
        return false;
    }
    out = v;
    return true;
}

JSValue toJs(JSContext* c, const json& j) {
    const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    return JS_ParseJSON(c, s.data(), s.size(), "<library>");
}

std::string lowerOf(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

json nullable(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

json itemOf(Library& lib, const ItemRecord& r) { return itemJson(r, lib.isRead(r.key), lib.libraryName(r.lib), lib.readingInfo(r.key)); }

void rebuildIndex(LibState& st) {
    st.dataVersion = st.lib->dataVersion();
    st.byDoi.clear();
    st.byArxiv.clear();
    st.byIsbn.clear();
    st.lib->forEachItemData([&](const std::string& key, const std::string& data) {
        ItemRecord r;
        r.key = key;
        r.json = data;
        const Identifiers id = identifiersOf(r);
        if (!id.doi.empty()) st.byDoi[lowerOf(id.doi)].push_back(key);
        if (!id.arxiv.empty()) st.byArxiv[lowerOf(id.arxiv)].push_back(key);
        for (const std::string& isbn : isbnKeys(id.isbn)) st.byIsbn[isbn].push_back(key);
    });
    st.indexed = true;
}

// ---- Natives ------------------------------------------------------------------------------

// n.need(op): the permission check alone (so an iterable can refuse at the call).
bool need(JSContext* c, PluginCtx& p, const char* opName) {
    const Op* op = opNamed(opName);
    return op && requirePermission(p, op->perm, op->detail);
}

JSValue needNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string name;
    if (!arg(c, argc, argv, 0, name)) return JS_EXCEPTION;
    return need(c, ctxOf(c), name.c_str()) ? JS_UNDEFINED : JS_EXCEPTION;
}

// n.call(op, a, b, c, d, ...): one synchronous read; the arguments are strings or numbers by op.
JSValue callNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string op;
    if (!arg(c, argc, argv, 0, op)) return JS_EXCEPTION;
    if (!need(c, p, op.c_str())) return JS_EXCEPTION;
    LibState* st = stateOf(c, p);
    if (!st) return JS_EXCEPTION;
    Library& lib = *st->lib;
    std::string a, b, d;
    // A key of a hidden group (switched off, gone, unreadable): as if it weren't there.
    auto hidden = [&lib](const std::string& key) {
        const auto in = lib.libraryOf(key);
        return in && !lib.libraryShown(*in);
    };

    if (op == "item") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        auto r = hidden(a) ? std::nullopt : lib.item(a);
        return toJs(c, r ? itemOf(lib, *r) : json(nullptr));
    }
    if (op == "page") {  // collection, tag, offset, limit, library (-1 all), opened (0/1), read (-1 any, 0 unread, 1 read)
        int offset = 0, limit = 0, library = -1, opened = 0, read = -1;
        if (!arg(c, argc, argv, 1, a) || !arg(c, argc, argv, 2, b) || !intArg(c, argc, argv, 3, offset) ||
            !intArg(c, argc, argv, 4, limit) || (argc > 5 && !intArg(c, argc, argv, 5, library)) ||
            (argc > 6 && !intArg(c, argc, argv, 6, opened)) || (argc > 7 && !intArg(c, argc, argv, 7, read)))
            return JS_EXCEPTION;
        offset = std::max(0, offset);
        limit = std::clamp(limit, 1, 200);
        json items = json::array();
        if (opened) {  // the last opened first: the keys, then this page's items as rows() would give them
            const auto keys = lib.openedItemKeys(library < 0 ? kAllLibraries : library, offset + limit);
            size_t rows = 0;
            for (size_t i = static_cast<size_t>(offset); i < keys.size(); ++i, ++rows) {
                auto r = lib.item(keys[i]);
                if (!r || (!a.empty() && std::find(r->collections.begin(), r->collections.end(), a) == r->collections.end()) ||
                    (!b.empty() && std::find(r->tags.begin(), r->tags.end(), b) == r->tags.end()))
                    continue;
                if (read >= 0 && lib.isRead(keys[i]) != (read == 1)) continue;
                items.push_back(itemOf(lib, *r));
            }
            return toJs(c, json{{"rows", rows}, {"items", items}});
        }
        ListQuery q;
        q.collectionKey = a;
        q.tag = b;
        q.lib = library < 0 ? kAllLibraries : library;
        q.read = read < 0 ? ReadFilter::Any : read == 1 ? ReadFilter::Read : ReadFilter::Unread;
        q.offset = offset;
        q.limit = limit;
        const auto rows = lib.rows(q);
        for (const ItemRow& row : rows)
            if (auto r = lib.item(row.key)) items.push_back(itemOf(lib, *r));
        return toJs(c, json{{"rows", rows.size()}, {"items", items}});
    }
    if (op == "search") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        json items = json::array();
        for (const ItemRecord& r : lib.search(a, kSearchLimit)) items.push_back(itemOf(lib, r));
        return toJs(c, items);
    }
    if (op == "find") {  // doi, arxiv, isbn ("" for none)
        if (!arg(c, argc, argv, 1, a) || !arg(c, argc, argv, 2, b) || !arg(c, argc, argv, 3, d)) return JS_EXCEPTION;
        if (!st->indexed || lib.dataVersion() != st->dataVersion) rebuildIndex(*st);
        std::vector<std::string> keys;
        auto add = [&](const std::unordered_map<std::string, std::vector<std::string>>& index, const std::string& k) {
            auto it = index.find(k);
            if (k.empty() || it == index.end()) return;
            for (const std::string& key : it->second)
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
        };
        add(st->byDoi, doiKey(a));
        add(st->byArxiv, lowerOf(normaliseArxiv(b)));
        for (const std::string& isbn : isbnKeys(d)) add(st->byIsbn, isbn);
        json items = json::array();
        for (const std::string& key : keys)
            if (auto r = lib.item(key); r && lib.libraryShown(r->lib)) items.push_back(itemOf(lib, *r));
        return toJs(c, items);
    }
    if (op == "feeds") {  // Zotero's "feeds" setting as sync keeps it: {url: [name, ...]}
        const json all = json::parse(lib.metaValue("zotero:feeds"), nullptr, false);
        std::vector<std::pair<std::string, std::string>> rows;  // name, url
        if (all.is_object())
            for (const auto& [url, v] : all.items())
                rows.push_back({v.is_array() && !v.empty() && v[0].is_string() ? v[0].get<std::string>() : url, url});
        std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) {
            return std::make_tuple(lowerOf(x.first), x.first, x.second) < std::make_tuple(lowerOf(y.first), y.first, y.second);
        });
        json out = json::array();
        for (const auto& [name, url] : rows) out.push_back({{"url", url}, {"name", name}});
        return toJs(c, out);
    }
    if (op == "libraries") {  // My Library, then the groups shown (switched on, still there), by name
        json out = json::array();
        for (const LibraryInfo& l : lib.libraries(false))
            out.push_back({{"id", l.id}, {"name", l.name}, {"editable", l.id == kMyLibrary || lib.libraryEditable(l.id)},
                           {"items", l.items}});
        return toJs(c, out);
    }
    if (op == "collections") {
        std::map<std::string, CollectionRecord> byKey;
        const auto all = lib.allCollections();
        for (const CollectionEntry& e : all) byKey[e.collection.key] = e.collection;
        struct Row {
            std::vector<std::string> path, sortPath;
            CollectionRecord c;
            std::string library;  // its library's name
        };
        std::map<int64_t, std::string> names;
        std::vector<Row> rows;
        for (const CollectionEntry& e : all) {
            if (!names.count(e.collection.lib)) names[e.collection.lib] = lib.libraryName(e.collection.lib);
            Row row{{}, {}, e.collection, names[e.collection.lib]};
            std::set<std::string> seen;
            for (const CollectionRecord* cur = &e.collection; cur;) {
                if (!seen.insert(cur->key).second) break;  // a loop in the data: stop
                row.path.insert(row.path.begin(), cur->name);
                auto up = byKey.find(cur->parentKey);
                cur = up == byKey.end() ? nullptr : &up->second;
            }
            for (const std::string& n : row.path) row.sortPath.push_back(lowerOf(n));
            rows.push_back(std::move(row));
        }
        // My Library's first, then each group's (by its name), each by path.
        auto order = [](const Row& r) { return std::make_tuple(r.c.lib != kMyLibrary, lowerOf(r.library), r.c.lib); };
        std::sort(rows.begin(), rows.end(), [&](const Row& x, const Row& y) {
            return std::tuple_cat(order(x), std::tie(x.sortPath, x.path, x.c.key)) <
                   std::tuple_cat(order(y), std::tie(y.sortPath, y.path, y.c.key));
        });
        json out = json::array();
        for (const Row& row : rows)
            out.push_back({{"key", row.c.key}, {"name", row.c.name}, {"parent", nullable(row.c.parentKey)}, {"path", row.path},
                           {"libraryId", row.c.lib}, {"libraryName", row.library}});
        return toJs(c, out);
    }
    if (op == "tags") {
        auto tags = lib.topTags(1 << 30);
        std::sort(tags.begin(), tags.end(), [](const auto& x, const auto& y) {
            const std::string lx = lowerOf(x.first), ly = lowerOf(y.first);
            return lx != ly ? lx < ly : x.first < y.first;
        });
        json out = json::array();
        for (const auto& [name, count] : tags) out.push_back({{"name", name}, {"count", count}});
        return toJs(c, out);
    }
    if (op == "attachments") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        json out = json::array();
        if (!hidden(a))
            for (const AttachmentRecord& at : lib.attachmentsOf(a)) out.push_back(attachmentJson(at));
        return toJs(c, out);
    }
    if (op == "annotations") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        const std::string itemKey = itemKeyOfAttachment(lib, a);
        json out = json::array();
        if (hidden(a)) return toJs(c, out);
        for (const AnnotationRecord& an : lib.annotationsFor(a))
            if (an.conflictOf.empty()) out.push_back(annotationJson(an, itemKey, lib.libraryName(an.lib)));
        return toJs(c, out);
    }
    if (op == "recent") {  // offset, limit
        int offset = 0, limit = 0;
        if (!intArg(c, argc, argv, 1, offset) || !intArg(c, argc, argv, 2, limit)) return JS_EXCEPTION;
        const auto rows = lib.recentAnnotations(std::max(0, offset), std::clamp(limit, 1, 200));
        json items = json::array();
        for (const NoteRow& row : rows)
            if (row.annotation.conflictOf.empty())
                items.push_back(annotationJson(row.annotation, row.itemKey, lib.libraryName(row.annotation.lib)));
        return toJs(c, json{{"rows", rows.size()}, {"items", items}});
    }
    if (op == "notes") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        auto notes = hidden(a) ? decltype(lib.notes(a)){} : lib.notes(a);
        std::vector<std::pair<std::string, const NoteRecord*>> order;  // oldest first
        for (const NoteRecord& n : notes) order.emplace_back(createdOf(n.json, n.dateModified), &n);
        std::stable_sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        json out = json::array();
        for (const auto& [created, n] : order)
            out.push_back({{"key", n->key}, {"itemKey", n->parentKey}, {"html", n->html}, {"createdBy", nullable(n->createdBy)},
                           {"created", created}, {"modified", n->dateModified}});
        return toJs(c, out);
    }
    return JS_ThrowTypeError(c, "zk.library: unknown call");
}

// ---- Full text: one worker for every plugin ------------------------------------------------

// A plugin's fullText calls queued or running: more are refused, so one plugin that doesn't await
// its calls can't fill the queue in front of everyone else's (whose budgets would run out waiting).
constexpr int kFullTextMaxPending = 4;

struct FullTextJob {
    std::string owner;  // the plugin (its folder: plugins of two hosts never share a count)
    AttachmentRecord attachment;
    std::string pluginsDir;
    EngineEnv env;
    std::shared_ptr<Work> work;
};

// Started at the first use and never stopped: the thread sleeps between jobs, and a job in flight
// at exit is the process's own business. Everything a job needs is copied into it, so no plugin
// or host object is touched from here (Work::finish is safe from any thread, and a no-op once the
// host is gone). Plugins take turns: the next job is another plugin's than the last one, if any waits.
class FullTextWorker {
public:
    static FullTextWorker& get() {
        static FullTextWorker* w = new FullTextWorker;
        return *w;
    }
    // False (nothing queued) when the plugin already has kFullTextMaxPending under way.
    bool submit(FullTextJob job) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            // Jobs of calls stopped meanwhile (or of a plugin unloaded) free their place now.
            std::erase_if(jobs_, [this](const FullTextJob& j) {
                if (!j.work->cancelled()) return false;
                done(j.owner);
                return true;
            });
            int& mine = pending_[job.owner];
            if (mine >= kFullTextMaxPending) return false;
            ++mine;
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
        return true;
    }

private:
    FullTextWorker() { std::thread([this] { run(); }).detach(); }
    void done(const std::string& owner) {
        if (--pending_[owner] <= 0) pending_.erase(owner);
    }
    void run() {
        lowerThreadPriority();
        for (;;) {
            FullTextJob job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return !jobs_.empty(); });
                auto next = std::find_if(jobs_.begin(), jobs_.end(), [this](const FullTextJob& j) { return j.owner != last_; });
                if (next == jobs_.end()) next = jobs_.begin();
                job = std::move(*next);
                jobs_.erase(next);
                last_ = job.owner;
            }
            if (!job.work->cancelled()) {  // else its call was stopped while it waited
                UserError err;
                auto out = extractFullText(job.attachment, job.pluginsDir, job.env, &err);
                if (out) {
                    auto textPtr = std::make_shared<std::string>(std::move(*out));
                    job.work->finish([textPtr](JSContext* c) {
                        const std::string clean = sanitisedUtf8(textPtr->data(), textPtr->size());
                        return JS_NewStringLen(c, clean.data(), clean.size());
                    });
                } else {
                    job.work->reject(err.message.empty() ? "zk.library.fullText: the text can't be read" : err.message);
                }
            }
            std::lock_guard<std::mutex> lock(mu_);
            done(job.owner);
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<FullTextJob> jobs_;
    std::map<std::string, int> pending_;  // owner → queued or running
    std::string last_;                    // whose job ran last
};

JSValue fullTextNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    if (!need(c, p, "fullText")) return JS_EXCEPTION;
    std::string key;
    if (!arg(c, argc, argv, 0, key)) return JS_EXCEPTION;
    LibState* st = stateOf(c, p);
    if (!st) return JS_EXCEPTION;
    auto attachment = st->lib->attachment(key);
    JSValue promise;
    auto work = startWork(p, &promise);
    if (JS_IsException(promise) || work->cancelled()) return promise;
    if (!attachment) {
        work->reject("There is no attachment " + key + " in the library.");
        return promise;
    }
    const PluginHostDeps& deps = depsOf(p);
    if (!FullTextWorker::get().submit({deps.pluginsDir + "/" + p.manifest.id, std::move(*attachment), deps.pluginsDir, deps.engineEnv, work}))
        work->reject("zk.library.fullText: this plugin already has " + std::to_string(kFullTextMaxPending) +
                     " texts being read; await some before asking for more");
    return promise;
}

// n.open(json): zk.library.open (0.10), the item opened in the reader in answer to the user. The screen
// thread resolves the item and opens it (ItemOpen, see host.h) and answers with PluginHost::answerReader.
JSValue openNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string raw;
    if (!arg(c, argc, argv, 0, raw)) return JS_EXCEPTION;
    uint64_t screen = 0;
    if (const std::string err = userAnswerCheck(p, "zk.library.open", &screen); !err.empty())
        return JS_ThrowPlainError(c, "%s", err.c_str());
    auto value = nlohmann::json::parse(raw, nullptr, false);
    JSValue promise = JS_UNDEFINED;
    auto work = startWork(p, &promise);
    if (work->cancelled()) return promise;  // already rejected (the call was stopped)
    PluginHostEvent ev{PluginHostEvent::Kind::ItemOpen, "", "", "", {}, 0, ""};
    ev.instance = screen;
    ev.value = std::move(value);
    const uint64_t id = waitForUi(p, std::move(ev), [work](const std::string& error) {
        if (error.empty()) work->finish([](JSContext*) { return JS_UNDEFINED; });
        else work->reject(error);
    });
    if (id == 0) work->reject("zk.library.open: the call was stopped");
    return promise;
}

// ---- What plugins see ---------------------------------------------------------------------

const char kLibraryJs[] = R"JS((function (n) {
"use strict";
const PAGE = 50;

function key(v, what) {
  if (typeof v !== "string") throw new TypeError(`zk.library.${what}: the key must be a string`);
  return v;
}

function limitOf(v, what) {
  if (v === undefined) return Infinity;
  if (!Number.isInteger(v) || v < 1) throw new TypeError(`zk.library.${what}: limit must be a positive whole number`);
  return v;
}

// An async iterable over pages: fetchPage(offset, size) -> {rows, items}.
function paged(fetchPage, limit, stopAt) {
  return {
    async *[Symbol.asyncIterator]() {
      let offset = 0, given = 0;
      for (;;) {
        const size = Math.min(PAGE, limit - given);
        if (size <= 0) return;
        const page = fetchPage(offset, size);
        for (const item of page.items) {
          if (stopAt && stopAt(item)) return;
          yield item;
          if (++given >= limit) return;
        }
        if (page.rows < size) return;
        offset += page.rows;
      }
    },
  };
}

const library = {
  items(query) {
    n.need("page");
    if (query !== undefined && (query === null || typeof query !== "object")) throw new TypeError("zk.library.items: the query must be an object");
    const q = query || {};
    for (const f of ["collection", "tag"])
      if (q[f] !== undefined && typeof q[f] !== "string") throw new TypeError(`zk.library.items: ${f} must be a string`);
    if (q.library !== undefined && !(Number.isInteger(q.library) && q.library >= 0))
      throw new TypeError("zk.library.items: library must be a library's id (0 for My Library)");
    if (q.opened !== undefined && q.opened !== true) throw new TypeError("zk.library.items: opened can only be true");
    if (q.read !== undefined && typeof q.read !== "boolean") throw new TypeError("zk.library.items: read must be a boolean");
    const limit = limitOf(q.limit, "items");
    const opened = q.opened === true ? 1 : 0;
    const read = q.read === undefined ? -1 : q.read ? 1 : 0;
    const collection = q.collection === undefined ? "" : q.collection;
    const tag = q.tag === undefined ? "" : q.tag;
    const lib = q.library === undefined ? -1 : q.library;
    return paged((offset, size) => n.call("page", collection, tag, offset, size, lib, opened, read), limit);
  },
  async item(k) { n.need("item"); return n.call("item", key(k, "item")); },
  async findByIdentifier(id) {
    n.need("find");
    if (id === null || typeof id !== "object") throw new TypeError("zk.library.findByIdentifier: pass { doi, arxiv, isbn }");
    for (const f of ["doi", "arxiv", "isbn"])
      if (id[f] !== undefined && typeof id[f] !== "string") throw new TypeError(`zk.library.findByIdentifier: ${f} must be a string`);
    return n.call("find", id.doi || "", id.arxiv || "", id.isbn || "");
  },
  async collections() { n.need("collections"); return n.call("collections"); },
  async libraries() { n.need("libraries"); return n.call("libraries"); },
  async feeds() { n.need("feeds"); return n.call("feeds"); },
  async tags() { n.need("tags"); return n.call("tags"); },
  async search(text) {
    n.need("search");
    if (typeof text !== "string") throw new TypeError("zk.library.search: the text must be a string");
    return n.call("search", text);
  },
  async attachments(itemKey) { n.need("attachments"); return n.call("attachments", key(itemKey, "attachments")); },
  async annotations(attachmentKey) { n.need("annotations"); return n.call("annotations", key(attachmentKey, "annotations")); },
  recentAnnotations(query) {
    n.need("recent");
    if (query !== undefined && (query === null || typeof query !== "object")) throw new TypeError("zk.library.recentAnnotations: the query must be an object");
    const q = query || {};
    const limit = limitOf(q.limit, "recentAnnotations");
    let since = null;
    if (q.since !== undefined) {
      since = typeof q.since === "string" ? Date.parse(q.since) : NaN;
      if (Number.isNaN(since)) throw new TypeError("zk.library.recentAnnotations: since must be an ISO date string");
    }
    return paged((offset, size) => n.call("recent", offset, size), limit,
                 since === null ? undefined : (a) => Date.parse(a.modified) < since);
  },
  async notes(itemKey) { n.need("notes"); return n.call("notes", key(itemKey, "notes")); },
  open(item, options) {
    n.need("open");
    const ref = typeof item === "string" ? { key: item } : item;
    if (ref === null || typeof ref !== "object" || typeof ref.key !== "string")
      throw new TypeError("zk.library.open: pass an item's key, or { key, library? }");
    if (ref.library !== undefined && !(Number.isInteger(ref.library) && ref.library >= 0))
      throw new TypeError("zk.library.open: library must be a library's id (0 for My Library)");
    if (options !== undefined && (options === null || typeof options !== "object"))
      throw new TypeError("zk.library.open: the options must be an object");
    const at = options === undefined ? undefined : options.attachment;
    if (at !== undefined && typeof at !== "string") throw new TypeError("zk.library.open: attachment must be a key");
    const v = { key: ref.key };
    if (ref.library !== undefined) v.library = ref.library;
    if (at !== undefined) v.attachment = at;
    return n.open(JSON.stringify(v));
  },
  async fullText(attachmentKey) { n.need("fullText"); return n.fullText(key(attachmentKey, "fullText")); },
};
return library;
}))JS";

}  // namespace

Library* sharedLibrary(PluginCtx& p, std::string* why) {
    LibState* st = openState(p);
    if (!st && why) *why = "the library isn't available";
    return st ? st->lib.get() : nullptr;
}

void installLibrary(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction<needNative>(c, natives.raw(), "need", 1);
    defineFunction<callNative>(c, natives.raw(), "call", 6);
    defineFunction<fullTextNative>(c, natives.raw(), "fullText", 1);
    defineFunction<openNative>(c, natives.raw(), "open", 1);

    const char* name = "<zk library>";
    js::Value fn(c, JS_Eval(c, kLibraryJs, sizeof kLibraryJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) {
        js::Value ex(c, JS_GetException(c));
        js::Value msg = ex.get("message");
        size_t len = 0;
        const char* s = JS_ToCStringLen(c, &len, msg.raw());
        std::string t = s ? std::string(s, len) : "?";
        if (s) JS_FreeCString(c, s);
        throw std::runtime_error(std::string(name) + ": " + t);
    }
    js::Value library = p.cx.call(fn, {natives});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
    JS_SetPropertyStr(c, p.zk.raw(), "library", JS_DupValue(c, library.raw()));
}

}  // namespace zk::plugins
