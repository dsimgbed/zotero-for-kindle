#include "app/session.h"

#include <chrono>
#include <filesystem>

#include "app/app.h"
#include "app/device_settings.h"
#include "app/settings_store.h"
#include "core/log.h"
#include "docs/render_worker.h"
#include "sync/files_sweep.h"

namespace zk {

namespace fs = std::filesystem;

namespace {

int64_t systemWallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

AccountSession::AccountSession(const Paths& p, const AccountEntry& e, const Config& c, DeviceSettings& device, SessionDeps deps)
    : paths_(accountPaths(p, e.userId)), entry_(e), config_(c), device_(device), deps_(std::move(deps)) {
    if (!deps_.wallMs) deps_.wallMs = systemWallMs;
}

std::unique_ptr<AccountSession> AccountSession::open(const Paths& p, const AccountEntry& e, const Config& c,
                                                     DeviceSettings& device, std::string* why, SessionDeps deps) {
    auto fail = [why](std::string text) -> std::unique_ptr<AccountSession> {
        log(LogLevel::Error, "account: %s", text.c_str());
        if (why) *why = std::move(text);
        return nullptr;
    };
    if (e.userId <= 0) return fail("no account");
    if (c.userId != e.userId) return fail("the key is another account's");
    std::unique_ptr<AccountSession> s(new AccountSession(p, e, c, device, std::move(deps)));
    const AccountPaths& a = s->paths_;
    std::error_code ec;
    for (const std::string& dir : {a.filesDir, a.creDir, a.stateDir}) {
        fs::create_directories(dir, ec);
        if (ec) return fail("can't make " + dir + ": " + ec.message());
    }
    try {
        s->db_ = std::make_unique<Db>(Db::open(a.dbPath));
        s->lib_ = std::make_unique<Library>(*s->db_);
        // Device keys still in the library (device.json couldn't be written when they were moved):
        // moved now; nothing happens when none are left.
        moveDeviceKeys(*s->lib_, device);
    } catch (const DbError& err) {
        s->lib_.reset();
        s->db_.reset();
        return fail(std::string("can't open the library: ") + err.what());
    }
    // Before the sync worker can download anything: the folders of attachments deleted in Zotero,
    // and downloads cut short by a kill — in this account's files only. Not on a library never
    // pulled (or just rebuilt), which would take every download for an orphan.
    try {
        if (s->lib_->libraryVersion() > 0) {
            const auto keys = s->lib_->attachmentKeys();
            sweepFiles(a.filesDir, {keys.begin(), keys.end()});
        }
    } catch (const DbError& err) {
        log(LogLevel::Warn, "files: no sweep, the library can't be read: %s", err.what());
    }
    s->previews_ = std::make_unique<PreviewCache>(a.previewDir, s->deps_.wallMs);
    s->homeTree_ = std::make_unique<HomeTreeStore>(a.stateDir + "/home-tree.json");
    log(LogLevel::Info, "account %lld: opened", static_cast<long long>(e.userId));
    return s;
}

AccountSession::~AccountSession() { stop(); }

PluginAccount AccountSession::pluginAccount() const {
    PluginAccount a;
    a.dbPath = paths_.dbPath;
    a.webdavHost = config_.webdavUrl;
    if (!deps_.pluginDataInPluginsDir) {
        a.storageRoot = paths_.pluginDataRoot;
        a.fullTextDir = paths_.fullTextDir;
    }
    return a;
}

EngineEnv AccountSession::engineEnv(EngineEnv device) const {
    device.cacheDir = paths_.creDir;
    return device;
}

void AccountSession::startServices(App& app, PluginHost* host, std::atomic<bool>& pluginsListening) {
    if (stopped_) return;
    listening_ = &pluginsListening;
    startSync();
    if (host) {
        host_ = host;
        host->setBackground(app.inBackground());  // a host starts as the app is
        host->start(pluginAccount());
    }
}

void AccountSession::startSync() {
    if (sync_) sync_->stop();
    sync_.reset();
    SyncDeps d;
    d.makeHttp = deps_.makeHttp;
    d.dbPath = paths_.dbPath;
    d.filesDir = paths_.filesDir;
    d.config = config_;
    d.keepAwake = deps_.keepAwake;
    d.wifiUp = deps_.wifiUp;
    d.intervalMinutes = loadSettings(*lib_, device_).syncIntervalMinutes;
    std::atomic<bool>* listening = listening_;
    d.pluginEventsWanted = [listening] { return listening && listening->load(std::memory_order_relaxed); };
    sync_ = std::make_unique<SyncService>(std::move(d));
    sync_->start();
}

void AccountSession::restartSync(const Config& c) {
    if (stopped_) return;
    config_ = c;
    if (host_) host_->setWebdavHost(c.webdavUrl);  // zk.fetch never reaches it
    startSync();
}

void AccountSession::stopServices() {
    if (sync_) sync_->requestStop();  // downloads cancelled (their .part removed) first
    if (host_) host_->stop();         // no plugin code runs for this account from here (joined)
    host_ = nullptr;
    if (sync_) sync_->stop();  // joined
    sync_.reset();
}

void AccountSession::stop() {
    if (stopped_) return;
    stopped_ = true;
    stopServices();
    // Render workers end with their readers (the screens are left first); what is left finishes
    // its engine call here, before this account's files go away under it.
    RenderWorker::waitForAll(3000);
    previews_.reset();
    homeTree_.reset();
    lib_.reset();
    db_.reset();  // the library closed
    log(LogLevel::Info, "account %lld: closed", static_cast<long long>(entry_.userId));
}

}  // namespace zk
