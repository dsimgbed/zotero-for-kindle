#pragma once
// One account's session (spec: multiple accounts, The account session): its credentials, its
// library (UI connection), its paths (AccountPaths: files, caches, state, plugin data), the files
// sweep, its sync worker, its preview cache and Home's kept tree, and the binding of the plugin
// host to it. Nothing of an account is opened anywhere else; App reaches the library and the sync
// through the session it is given (App::setSession).
//
// open() opens the library and sweeps the account's files (its files/ only); startServices() starts
// the sync worker and the plugin host for the account; stop() ends it all in a fixed order:
// downloads cancelled, plugin host stopped (joined), sync stopped (joined), render workers ended,
// caches dropped, the library closed. stop() is idempotent and the destructor calls it. Screen
// thread only.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "app/accounts.h"
#include "app/home_tree.h"
#include "docs/document.h"
#include "net/http.h"
#include "plugins/host.h"
#include "screens/preview.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/config.h"
#include "sync/sync_service.h"

namespace zk {

class App;
class DeviceSettings;
class KeepAwake;

// What a session needs from the device (all optional: tests leave most unset).
struct SessionDeps {
    std::function<std::unique_ptr<Http>()> makeHttp;  // the sync worker's network
    KeepAwake* keepAwake = nullptr;                   // the sync worker's hold while it works
    std::function<bool()> wifiUp;                     // default: the device's (SyncDeps::wifiUp)
    std::function<int64_t()> wallMs;                  // the preview cache's clock; default: the system's
    // The desktop developer loop (--plugins-dir): plugin data stays in the plugins' own folders
    // (docs/plugins/getting-started.md), the library is still the account's.
    bool pluginDataInPluginsDir = false;
};

class AccountSession {
public:
    // The account's library opened (its folders made), device keys left in it moved to `device`
    // (moveDeviceKeys: a retry after an earlier failed device.json write; nothing when none are
    // left) and its files swept (a library pulled at least once). nullptr with *why when the
    // library can't be opened or `config` is another user's; the account's files are then left as
    // they were.
    static std::unique_ptr<AccountSession> open(const Paths&, const AccountEntry&, const Config&, DeviceSettings&,
                                                std::string* why, SessionDeps = {});
    ~AccountSession();
    AccountSession(const AccountSession&) = delete;
    AccountSession& operator=(const AccountSession&) = delete;

    // Starts the sync worker and (`host` non-null: plugins aren't off) the plugin host for this
    // account. `pluginsListening`: whether pulls build events for plugins (outlives the session).
    // Called again (after stopServices, an update's failed install) it starts them again.
    void startServices(App&, PluginHost* host, std::atomic<bool>& pluginsListening);
    // A new key for the account (Setup): the sync worker starts again with it; the host is told
    // the WebDAV server zk.fetch refuses.
    void restartSync(const Config&);
    // The sync worker and the host stopped (in stop()'s order); the library stays open (an update's
    // install backs it up meanwhile).
    void stopServices();
    void stop();

    // What the account sheet shows of it while it isn't open (items, unsent changes; `synced`: a
    // good sync just ended, so also when), kept in its accounts.json entry. Also done by stop().
    void keepSummary(bool synced);
    int64_t itemCount();   // its libraries' top-level items, as the lists count them
    int unsentCount();     // changes made on the Kindle a sync would still send

    // Not after stop().
    Library& library() { return *lib_; }
    PreviewCache& previews() { return *previews_; }
    HomeTreeStore& homeTree() { return *homeTree_; }

    const AccountPaths& paths() const { return paths_; }
    const AccountEntry& entry() const { return entry_; }
    const Config& config() const { return config_; }
    int64_t userId() const { return entry_.userId; }
    SyncService* sync() { return sync_.get(); }  // nullptr while services don't run
    // What the plugin host serves for this account.
    PluginAccount pluginAccount() const;
    // `device`'s engines with this account's crengine cache (AccountPaths::creDir).
    EngineEnv engineEnv(EngineEnv device) const;

private:
    AccountSession(const Paths&, const AccountEntry&, const Config&, DeviceSettings&, SessionDeps);
    void startSync();

    AccountPaths paths_;
    std::string accountsFile_;
    AccountEntry entry_;
    Config config_;
    DeviceSettings& device_;
    SessionDeps deps_;
    std::unique_ptr<Db> db_;
    std::unique_ptr<Library> lib_;
    std::unique_ptr<PreviewCache> previews_;
    std::unique_ptr<HomeTreeStore> homeTree_;
    std::unique_ptr<SyncService> sync_;
    PluginHost* host_ = nullptr;  // bound by startServices, until stopServices
    std::atomic<bool>* listening_ = nullptr;
    bool stopped_ = false;
};

}  // namespace zk
