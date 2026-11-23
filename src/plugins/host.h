#pragma once
// The plugin host: one plugin thread running every enabled plugin, each in its own QuickJS runtime
// and context. The screen thread never waits on JavaScript: it queues commands (dispatch,
// runBackground, answer, reload, setEnabled) and polls PluginHostEvents, like sync and render
// events. See plugin_ctx.h for how bindings (zk.*) plug in.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "docs/document.h"
#include "net/http.h"
#include "plugins/events.h"
#include "plugins/manifest.h"
#include "plugins/ui_tree.h"
#include "update/version.h"

namespace zk {

class Http;

namespace plugins {
struct PluginCtx;
// The plugin thread and its workers (fetch, full text) call this first: they run below the screen
// and sync threads (nice 10 on Linux; 10 below the default scheduling priority on macOS), so reading never stutters
// for a plugin.
void lowerThreadPriority();
}

// The account the host serves while it runs (spec: multiple accounts, Plugins): the library its
// plugins read, where their zk.storage and zk.files live (<storageRoot>/<pluginId>/storage.db and
// data/; AccountPaths::pluginDataRoot), the full-text cache (<fullTextDir>/<attKey>-<md5>.txt) and
// the WebDAV server zk.fetch refuses. Plugins themselves (code, on/off, state, logs) are the device's.
struct PluginAccount {
    std::string dbPath;       // "" or missing: zk.library isn't available
    // <storageRoot>/<pluginId>/storage.db and data/; "": <pluginsDir>/<pluginId>/ (no account: the
    // desktop's screenshot tools, tests, before Setup)
    std::string storageRoot;
    std::string fullTextDir;  // "": <pluginsDir>/.cache/fulltext (as storageRoot)
    std::string webdavHost;   // the account's WebDAV server (host or URL), "" for none; later: setWebdavHost
};

struct PluginHostDeps {
    std::string pluginsDir;  // the device's plugins: <pluginsDir>/<id>/ (code, settings.json, plugin.log), .state.json
    // The account start() serves (start(PluginAccount) replaces it). Plugins open their own
    // connection to its library database (on the plugin thread), like the sync worker.
    PluginAccount account;
    // Where the engines find their data (zk.library.fullText opens its own engine).
    EngineEnv engineEnv;
    // Tests: makes zk.library.fullText's engine instead of loadEngine (plugins/fulltext.h's EngineLoader).
    std::function<std::unique_ptr<DocumentEngine>(DocType, const EngineEnv&, UserError*)> fullTextLoader;
    // Every account's plugin data root (AccountPaths::pluginDataRoot): remove() deletes the plugin's
    // <root>/<id> in each (removePluginData). Empty: only the account served.
    std::function<std::vector<std::string>()> pluginDataRoots;
    // Names the plugin whose code is running (written before a plugin's code runs, removed when
    // the plugin thread goes idle). Empty: no marker.
    std::string crashMarker;
    Version appVersion;
    std::function<std::unique_ptr<Http>()> makeHttp;
    std::function<int64_t()> clock;  // wall clock, ms since the epoch (log times); default: system
    // The desktop developer loop (--plugins-dir): start() loads every enabled plugin at once, and
    // reload(id) loads an enabled plugin that wasn't loaded yet. Off: plugins load lazily, at their
    // first event (the Kindle).
    bool loadEagerly = false;
    size_t memoryPerPlugin = 32 << 20;
    // What all plugins together may use (actual use, shared): an allocation that would pass it
    // fails, stopping the call that made it.
    size_t memoryTotal = 96u << 20;
    int handlerBudgetMs = 5000, backgroundBudgetMs = 30000;
    // A screen's onEvent (and an action's run): user-caused, at least 30 s. A screen's render: 1 s.
    int actionBudgetMs = 30000, renderBudgetMs = 1000;
    // Quiet run time: outside user-caused calls and the background run, a plugin's JavaScript may
    // run at most quietRunMs in any quietWindowMs; a timer or continuation whose call has ended
    // waits (not failed) until the window allows it.
    int quietRunMs = 2000, quietWindowMs = 60000;
    // setTimeout's floor: a shorter delay is raised to it (each wake costs battery).
    int minTimerMs = 100;
    // zk.ui.refresh outside user-caused calls (a timer, a fetch that came back): merged into at most
    // one render of the screen this often, each a redraw on e-ink. One from a user-caused call renders at once.
    int quietRefreshMs = 1000;
    // Free bytes on the partition holding a path (statvfs; tests inject their own): plugin writes
    // (zk.files, zk.storage) are refused when it is under 50 MB.
    std::function<uint64_t(const std::string& path)> freeSpace;
    // Each line written to a plugin's log is also handed here (on the plugin thread): the desktop
    // build mirrors it to stderr when asked to.
    std::function<void(const std::string& pluginId, const std::string& line)> logSink;
    // More bindings installed into every plugin's `zk` after the built-in ones (tests use this to
    // give plugins host work they control).
    std::vector<std::function<void(plugins::PluginCtx&)>> extraBindings;
};

// Confirm: the app's own confirmation of a removal (zk.zotero): `text` the question, `lines` what
// goes when there are several, `action` the yes button ("Move to trash"); answer it with
// PluginHost::answer(requestId). Withdrawn: that request no longer waits (its call was stopped, or
// the plugin turned off or reloaded): take the question away without answering.
// BackgroundDone: the tasks of a runBackground() have all ended (also when none was due): the
// screen thread's keep-awake hold may end; `requestId` is the round it answers. Badge: `text` is the plugin's new badge ("" cleared).
//
// Screens (zk.screen; `instance` names one open screen, see PluginHost::openScreen):
// ScreenTree: `tree` is what render returned, checked; `requestId` counts the events of the screen
// sent with screenEvent() that were handled before it (an event is answered by the first tree
// whose requestId reaches its number, 1 for the first); for Home's body (TreeUse::HomeBody)
// `value` is the tree's JSON as render returned it. ScreenOpen: the plugin opened `screenId`
// (title in `text`) over `parent` with zk.ui.open (parent 0: from an action's run); `value` the
// params (null: none). ScreenClose: the plugin closed the screen (zk.ui.close); `value` the result.
// ScreenError: the screen can't be shown (render failed, an invalid tree, the plugin turned off or
// restarted): `text` "<Plugin> can't show this screen right now"; nothing more comes for it. `value`
// {missing: true} when the plugin runs but has no screen of that id (gone in an update).
// ScreenActionError: its onEvent failed: `text` "<Plugin> couldn't do that: <message>"; a tree follows.
//
// Actions (zk.selectionAction / zk.itemAction; `requestId` is runAction's number):
// ActionResult: `value` {text, actions: [{id, title}] (at most 2)} for the result pop-up, or null
// (run returned nothing: no pop-up). ActionError: `text` "<Plugin> couldn't do that: <message>"
// (run threw, went over its budget, returned what can't be shown, or the plugin stopped).
// A run stopped with stopAction() gets neither.
//
// Previews (zk.reader.open, 0.9): ReaderOpen: open `value` ({url, title?} or {html, title,
// baseUrl?}, checked) in the reader as a preview, over `instance` (the plugin's screen; 0: from an
// action's run); answer with PluginHost::answerReader(requestId) once the reader shows (or it
// failed or was cancelled). A Withdrawn with that requestId: nobody waits for the answer any more.
// ReaderClosed is the app's own (never the host's): the preview over `instance` closed.
// ItemOpen (zk.library.open, 0.10): open the library item `value` ({key, library?, attachment?,
// annotation? (0.11)}) in the reader, `instance` the screen it was asked from (0: an action's run);
// answered with answerReader as ReaderOpen is ("" once it shows, else "Item not found", "Annotation
// not found", "Cancelled", the download's error); a
// Withdrawn with that requestId: nobody waits.
struct PluginHostEvent {
    enum class Kind {
        Confirm, Badge, Disabled, ChangesQueued, Log, Withdrawn, BackgroundDone,
        ScreenTree, ScreenOpen, ScreenClose, ScreenError, ScreenActionError,
        ActionResult, ActionError, ReaderOpen, ReaderClosed, ItemOpen, BridgeReturn
    } kind;
    std::string pluginId, pluginName, text;
    std::vector<std::string> lines;
    uint64_t requestId = 0;
    std::string action;
    uint64_t instance = 0, parent = 0;
    std::string screenId = {};
    nlohmann::json value = nullptr;
    std::shared_ptr<const Block> tree = nullptr;
};

// What a plugin registered with zk.selectionAction / zk.itemAction / zk.screen.
// kind: "selectionAction", "itemAction" or "screen"; `main`: a screen listed in the launcher;
// `home`: a screen that can be Home's body (Settings → Home).
struct PluginEntry {
    std::string kind, id, title;
    bool main = false, home = false;
};

struct PluginStatus {
    PluginFolder folder;
    bool enabled = false, loaded = false;
    // everEnabled: turned on at some time (a copied plugin isn't until its permissions were shown).
    // stopped: the app turned it off (three failures in a row, or the app stopped while it ran).
    bool everEnabled = false, stopped = false;
    int failures = 0;      // in a row, not cleared by a success yet
    std::string problem;   // why it couldn't load this time ("" if none; also the last of errors)
    std::string badge;
    std::vector<std::string> errors;  // newest last: past failures, then a current problem
    std::vector<PluginEntry> entries;  // registered by the loaded plugin, in order; empty when not loaded
    std::string withdrawn;  // the catalog withdrew it: why ("" when it didn't); it is off and can't be turned on
};

// Removing a plugin: deletes <root>/<id> (its storage.db and data/) under each root. Ignored for an
// id that isn't a plain plugin folder name ("", ".", "..", a path, a dot-name): nothing else in a
// root is ever touched.
void removePluginData(const std::vector<std::string>& roots, const std::string& id);

class PluginHost {
public:
    // Scans the plugins folder (nothing runs yet). If the crash marker names a plugin, the app
    // stopped while it was running: it is turned off, with a note, and the marker removed.
    explicit PluginHost(PluginHostDeps);
    ~PluginHost();  // stop()
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Starts the plugin thread for the account it served last (deps.account at first), with
    // deps.loadEagerly loading every enabled plugin. Plugins load lazily: each enabled plugin when
    // the first event or background run is due. Commands queued before the first start() wait for it.
    void start();
    // Starts it for `account` (a running host is stopped first): the switch of accounts. Plugins
    // load afresh, so nothing of the account before is in them.
    void start(const PluginAccount& account);
    // Interrupts whatever plugin code is running and joins the thread, then zk.fetch's workers and
    // the full-text worker (a text being extracted is cancelled at its next page or chapter, caching
    // nothing); the library connection is closed. Returns within 2 s. Calls cut short by stop() are
    // not counted as failures. Commands asked while stopped are dropped, remove() and resetData()
    // included (they were for the account before; before the first start() they wait for it).
    // Events queued for poll() and not taken yet are dropped too (a background round stopped
    // midway reports nothing: the app abandons its hold). Idempotent; start() again after it.
    void stop();
    // Unloads and rescans; plugins that were loaded are loaded again at once.
    void reload();
    void reload(const std::string& id);
    void dispatch(PluginEvent);
    // Runs the background task of each plugin that has one and hasn't run, failed or gone over its
    // budget in the last hour (PluginState::lastBackgroundMs, persisted). Calls cut short (stop(),
    // cancelBackground(), the plugin turned off) don't count: that plugin is due again. One
    // BackgroundDone event follows when the tasks have ended; its requestId is `round` (the caller's
    // number for this run, so a late Done of an earlier round can be told apart). A task whose
    // zk.fetch died of a lost connection isn't counted either.
    void runBackground(uint64_t round = 0);
    // Is some enabled plugin's background task due (see above)? Cheap and any thread: loads nothing.
    // A plugin not loaded yet whose manifest has the background permission counts as possibly due.
    bool backgroundDue() const;
    // Some installed plugin that can load is turned on (cheap; any thread): with none, the app
    // builds no events for plugins.
    bool anyEnabled() const;
    // The Kindle is going to sleep: stops background tasks still running. Not failures.
    void cancelBackground();
    // The app went to the background (true) or came back (false). Meanwhile timers whose call has
    // ended don't fire (the plugin thread isn't woken for them); they run, late, once back.
    void setBackground(bool);
    // Empties the plugin's data/ folder and its storage (Settings → Plugins → Reset its data). Works
    // whether or not the plugin is loaded; if it is, its queued storage writes are dropped.
    void resetData(const std::string& id);
    // Opens a plugin's screen (the launcher, a test): returns its instance number at once; the
    // plugin gets an `open` event (if it has onEvent), then render runs and a ScreenTree (or a
    // ScreenError) follows. `params` (null: none) are given to render and onEvent. `use` HomeBody:
    // Home's body: its trees are parsed as such (a search or field is an invalid tree: a ScreenError,
    // counted as a failure), and each ScreenTree carries the tree's JSON in `value` (Home keeps it).
    uint64_t openScreen(const std::string& pluginId, const std::string& screenId, nlohmann::json params = nullptr,
                        TreeUse use = TreeUse::Screen);
    // What the user did on an open screen ({kind:"tap", id}, …: zk.d.ts's zk.ui.Event). Events are
    // handled one at a time, in order; each is answered by a ScreenTree (see PluginHostEvent).
    void screenEvent(uint64_t instance, nlohmann::json event);
    // The user left the screen (Back, or it went with its tab): it is closed now; the plugin gets
    // a `back` event, and the screen below a `result` event.
    void closeScreen(uint64_t instance);
    // Runs an open screen's render again, with no event (Home's body when Home shows, after a sync):
    // a ScreenTree follows (or a ScreenError), as after an event: the render budget, its time free.
    // While the screen's onEvent or render runs, once that has ended (one render for any number of
    // asks). Ignored for a screen closed or failed.
    void rerender(uint64_t instance);
    // Runs a plugin's action (kind "selectionAction" or "itemAction") on `target` (zk.d.ts's
    // TextSelection or Item); `again` ({action, text}: a pop-up button) or null. User-caused, with
    // deps.actionBudgetMs (waiting for the user's confirmation is free). Returns its number at
    // once; an ActionResult or ActionError with it follows. zk.ui.open from the run opens its
    // screens (ScreenOpen, parent 0) when the run ends well.
    uint64_t runAction(const std::string& pluginId, const std::string& kind, const std::string& actionId,
                       nlohmann::ordered_json target, nlohmann::json again = nullptr);
    // The user closed the result pop-up: the run is stopped (not a failure); nothing more comes for it.
    void stopAction(uint64_t run);
    // Loads every enabled plugin not loaded yet (so their entry points are known), as an event would.
    void wake();
    // Changes whenever the entry points, badges, or which plugins are on or loaded may have changed
    // (cheap; any thread).
    uint64_t entriesVersion() const;
    // The user's answer to a Confirm event.
    void answer(uint64_t requestId, bool yes);
    // A ReaderOpen's outcome: "" the reader shows the document (zk.reader.open resolves), else why
    // not ("Cancelled", a download's error): it rejects with that message.
    void answerReader(uint64_t requestId, std::string error);
    // zk.reader.open's download of `req.url` (into req.outFile, with req.onProgress and req.cancel)
    // under the plugin's net rules (as zk.fetch: https, its declared hosts, never zotero.org or the
    // WebDAV server), on the caller's thread with an Http of its own; at most `maxBytes`, within
    // `timeoutMs`. Any thread. Throws plugins::FetchError (also when the plugin isn't listed).
    HttpResponse previewDownload(const std::string& pluginId, HttpRequest req, size_t maxBytes, int timeoutMs) const;
    bool poll(PluginHostEvent&);
    // Set when a plugin posts BridgeReturn (zk.bridge.returnToGrimoire). The screen thread sees it
    // without waiting for JavaScript and starts the clean exit 43 handoff.
    bool bridgeReturnRequested() const;
    std::vector<PluginStatus> status() const;
    // A plugin's package: the folder its files are in (<pluginsDir>/<id>), where its screens'
    // images are. Any thread.
    std::string packageDir(const std::string& id) const;
    // Turning a plugin on clears its failures; turning it off unloads it. Ignored for an id that
    // isn't a listed folder, and turning on for a folder with a problem (broken, incompatible).
    void setEnabled(const std::string& id, bool);
    // Settings → Plugins → Remove: off at once and no longer listed by status(); then, on the plugin
    // thread, unloaded and its folder deleted (settings and all), and its data in every account
    // (deps.pluginDataRoots). What it remembered goes
    // too, so a copy put back is new again. Its changes for Zotero stay (pending ones are sent).
    void remove(const std::string& id);
    // Off at once and no longer listed, its state forgotten and its log closed; its files are left
    // for the caller to delete (the catalog's remove, on its worker thread).
    void forget(const std::string& id);
    // What the app remembers of an id (on/off, ever turned on, failures), even one not listed: a
    // catalog install into a missing folder starts as new. Any thread.
    void forgetState(const std::string& id);
    // The catalog swaps a plugin's files: hold() stops it (unloaded on the plugin thread, waiting up to
    // timeoutMs; call it from a worker, never the screen or plugin thread) and keeps it from loading
    // until release(), which rescans its folder (on the plugin thread; here when it isn't running,
    // as in safe mode). True when it is stopped. Its on/off, storage and state stay as they are.
    bool hold(const std::string& id, int timeoutMs = 40000);
    void release(const std::string& id);
    // The catalog's withdrawn ids (id → reason): each newly named one is turned off and unloaded, and
    // can't be turned on while it is named. Any thread.
    void setWithdrawn(std::map<std::string, std::string>);
    // The WebDAV server changed (host or URL; "" for none): zk.fetch refuses the new one from the
    // next request on. Any thread.
    void setWebdavHost(std::string);

    // Blocks until no command is queued and no call is running or awaiting (timers may be
    // pending), or `timeoutMs` passes. True if idle. For tests and the background keep-awake hold.
    bool waitIdle(int timeoutMs);

    struct Impl;

private:
    void forgetPlugin(const std::string& id, bool deleteFolder);
    std::unique_ptr<Impl> impl_;
};

}  // namespace zk
