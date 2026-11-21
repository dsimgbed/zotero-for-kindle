#include "app/account_ui.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "app/app.h"
#include "core/log.h"
#include "net/errors.h"
#include "screens/setup.h"
#include "screens/switching.h"
#include "store/library.h"

namespace zk {

namespace fs = std::filesystem;

namespace {

int64_t systemWallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::vector<AccountEntry> inOrder(std::vector<AccountEntry> list) {
    std::stable_sort(list.begin(), list.end(), [](const AccountEntry& a, const AccountEntry& b) { return a.order < b.order; });
    return list;
}

int64_t folderBytes(const std::string& dir) {
    int64_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) {
            const auto size = it->file_size(e2);
            if (!e2) n += static_cast<int64_t>(size);
        }
    }
    return n;
}

std::string keyLineOf(const std::optional<Config>& c, int64_t userId) {
    if (!c) return "User ID " + std::to_string(userId) + " · no key on this Kindle";
    return "User ID " + std::to_string(userId) + " · " + (c->webdavUrl.empty() ? "files from Zotero storage" : "WebDAV: " + hostOf(c->webdavUrl));
}

}  // namespace

AccountsUi::AccountsUi(App& app, AccountSwitcher& switcher, AccountsUiDeps deps)
    : app_(app), switcher_(switcher), deps_(std::move(deps)),
      pins_(deps_.paths.accountsFile(), deps_.wallMs ? deps_.wallMs : systemWallMs, [&app] { return app.nowMs(); }) {
    if (!deps_.wallMs) deps_.wallMs = systemWallMs;
    if (!deps_.online) deps_.online = [] { return probeNetState().wifiConnected; };
    if (!deps_.keyOwner && deps_.checkKey)
        deps_.keyOwner = [check = deps_.checkKey](const Config& c) {
            std::string name;
            return check(c, &name);
        };
    if (!deps_.credentials) deps_.credentials = [paths = deps_.paths](int64_t id) { return loadCredentials(paths, id); };
}

int64_t AccountsUi::openUserId() const {
    const AccountSession* s = switcher_.session();
    return s ? s->userId() : 0;
}

std::string AccountsUi::nameOf(int64_t userId) {
    AccountSummary a;
    a.userId = userId;
    if (const auto e = pins_.entry(userId)) a.name = e->name;
    return accountName(a);
}

// From accounts.json only (each session keeps its summary there), except the open account's,
// read from its open library: no other account's library is ever opened to show it.
std::vector<AccountSummary> AccountsUi::accounts(int64_t measure) {
    std::vector<AccountSummary> out;
    const auto st = readAccounts(deps_.paths.accountsFile());
    if (!st) return out;
    const int64_t open = openUserId();
    for (const AccountEntry& e : inOrder(st->accounts)) {
        AccountSummary a;
        a.userId = e.userId;
        a.name = e.name;
        a.active = e.userId == open;
        a.hasPin = e.pin.has_value();
        a.lockOnSleep = e.lockOnSleep;
        a.locked = a.hasPin && !a.active;  // nothing of it until its PIN
        if (!a.locked) {
            a.items = e.itemCount;
            a.unsent = e.unsentCount;
            a.syncedWallMs = e.syncedWallMs;
            if (a.active) {
                try {
                    a.items = switcher_.session()->itemCount();
                    a.unsent = switcher_.session()->unsentCount();
                } catch (const std::exception& ex) {
                    log(LogLevel::Warn, "accounts: can't count the open library: %s", ex.what());
                }
            }
            a.keyLine = keyLineOf(deps_.credentials(e.userId), e.userId);
            if (measure == e.userId) a.bytes = folderBytes(accountPaths(deps_.paths, e.userId).root);
        }
        out.push_back(std::move(a));
    }
    return out;
}

void AccountsUi::switchTo(int64_t userId) {
    if (userId == openUserId()) return;
    switcher_.switchTo(userId, AccountSwitcher::Why::User);
}

// ---- Setup: Add account, Update key --------------------------------------------------------------

std::unique_ptr<Screen> AccountsUi::addSetup(bool canGoBack) {
    auto name = std::make_shared<std::string>();  // written by the check, read once it is done
    auto check = [check = deps_.checkKey, name](const Config& c) { return check ? check(c, name.get()) : UserError{}; };
    auto connected = [this, name](const Config& c, Nav&) { app_.defer([this, c, n = *name] { added(c, n); }); };
    auto setup = std::make_unique<SetupScreen>(Config{}, false, deps_.paths.configDir, check, connected, canGoBack);
    SetupPurpose p;
    p.kind = SetupPurpose::Kind::Add;
    p.present = [this](int64_t userId) { return pins_.entry(userId) ? nameOf(userId) : std::string(); };
    setup->setPurpose(std::move(p));
    return setup;
}

std::unique_ptr<Screen> AccountsUi::firstAccountSetup(const Config& initial, bool imported) {
    auto name = std::make_shared<std::string>();
    auto check = [check = deps_.checkKey, name](const Config& c) { return check ? check(c, name.get()) : UserError{}; };
    auto connected = [this, name](const Config& c, Nav&) { app_.defer([this, c, n = *name] { added(c, n); }); };
    auto setup = std::make_unique<SetupScreen>(initial, imported, deps_.paths.configDir, check, connected, false);
    SetupPurpose p;  // as at first: no account on the Kindle; no config.json (the account list is there)
    p.keepsConfigFile = false;
    setup->setPurpose(std::move(p));
    return setup;
}

void AccountsUi::addAccount(Nav& nav) {
    const auto st = readAccounts(deps_.paths.accountsFile());
    if (st && st->accounts.size() >= kMaxAccounts) {
        nav.toast("This Kindle has three accounts already");
        return;
    }
    nav.push(addSetup(true));
}

// Identity first: the key was checked and names its user; only now is anything of it made.
void AccountsUi::added(const Config& c, const std::string& name) {
    if (pins_.entry(c.userId)) {
        app_.toast(nameOf(c.userId) + " is already on this Kindle");
        return;
    }
    if (!saveCredentials(deps_.paths, c) || !zk::addAccount(deps_.paths, c.userId, name)) {
        std::error_code ec;
        if (!pins_.entry(c.userId)) fs::remove(deps_.paths.credentialsFile(c.userId), ec);  // nothing left behind
        app_.toast("Can't add the account on the Kindle");
        return;
    }
    log(LogLevel::Info, "accounts: %lld added", static_cast<long long>(c.userId));
    added_ = c.userId;
    if (!switcher_.switchTo(c.userId, openUserId() ? AccountSwitcher::Why::User : AccountSwitcher::Why::Startup))
        app_.toast("Can't switch now; the account is in Settings → Accounts");
}

void AccountsUi::keepKey(const Config& c) {
    if (!saveCredentials(deps_.paths, c)) {
        app_.toast("Can't save the key on the Kindle");
        return;
    }
    if (c.userId == openUserId() && deps_.keyChanged) deps_.keyChanged(c);
}

void AccountsUi::updateKey(Nav& nav, int64_t userId) {
    Config initial = deps_.credentials(userId).value_or(Config{});
    initial.userId = userId;
    auto check = [check = deps_.checkKey](const Config& c) {
        std::string name;
        return check ? check(c, &name) : UserError{};
    };
    auto connected = [this](const Config& c, Nav&) {
        app_.defer([this, c] {
            keepKey(c);
            app_.pop();  // Setup
            app_.toast("The new key is kept");
        });
    };
    auto setup = std::make_unique<SetupScreen>(initial, false, deps_.paths.configDir, check, connected, true);
    SetupPurpose p;
    p.kind = SetupPurpose::Kind::Update;
    p.userId = userId;
    p.name = nameOf(userId);
    setup->setPurpose(std::move(p));
    nav.push(std::move(setup));
}

// ---- the PIN ---------------------------------------------------------------------------------------

std::unique_ptr<PinPadScreen> AccountsUi::pad(int64_t userId, PinPadScreen::Purpose purpose, std::function<void(Nav&)> done,
                                              std::function<void(Nav&)> back, std::function<void(int64_t, Nav&)> useAnother) {
    PinPadDeps d;
    d.userId = userId;
    d.name = nameOf(userId);
    d.pins = &pins_;
    d.credentials = [this, userId] { return deps_.credentials(userId); };
    d.resetFile = deps_.resetFile;
    d.checkKey = deps_.keyOwner;
    d.online = deps_.online;
    d.keepKey = [this](const Config& c) { keepKey(c); };
    d.done = std::move(done);
    d.back = std::move(back);
    if (useAnother) {
        d.others = [this, userId] { return rowsExcept(userId); };
        d.useAnother = std::move(useAnother);
    }
    return std::make_unique<PinPadScreen>(purpose, std::move(d));
}

std::unique_ptr<Screen> AccountsUi::pinPad(int64_t userId, PinPadScreen::Purpose purpose, std::function<void(Nav&)> done) {
    return pad(userId, purpose, std::move(done), [](Nav& nav) { nav.pop(); });
}

void AccountsUi::setLockOnSleep(int64_t userId, bool on) {
    if (!pins_.setLockOnSleep(userId, on)) app_.toast("Can't change the accounts on the Kindle");
}

std::vector<AccountSheet::Row> AccountsUi::rowsExcept(int64_t userId) {
    std::vector<AccountSheet::Row> rows;
    const int64_t now = deps_.wallMs();
    for (const auto& a : accounts())
        if (a.userId != userId) rows.push_back({a.userId, accountName(a), a.active, a.locked, accountLine(a, now)});
    return rows;
}

// Over the open account, no pad but its own lock pad ever takes the lock away: while the open
// account is locked (openLocked_), leaving a nested pad (Back, Use another account → itself)
// shows its lock pad again; only its right PIN (or the reset) clears it.
void AccountsUi::backToOpen() {
    if (openLocked_) showOpenLock();
    else app_.unlock();
}

void AccountsUi::unlock(int64_t userId, std::function<void(bool)> done) {
    // Not unlocked: a removal waiting for this switch doesn't happen either (and says so).
    auto notUnlocked = [this, done] {
        if (removing_) {
            app_.toast(nameOf(removing_) + " wasn't removed");
            removing_ = 0;
        }
        done(false);
    };
    // From an open account (asked before the switch begins) the pad can go back to it — to its
    // lock pad if it is locked.
    std::function<void(Nav&)> back;
    if (openUserId())
        back = [this, notUnlocked](Nav&) {
            app_.defer([this, notUnlocked] {
                notUnlocked();
                backToOpen();
            });
        };
    app_.lock(pad(
        userId, PinPadScreen::Purpose::Unlock,
        [this, done](Nav&) {
            app_.defer([this, done] {
                if (!openUserId()) app_.unlock();  // (from an open account, the switch's clearTo ends the lock)
                done(true);
                if (app_.locked() && openUserId()) backToOpen();  // the switch didn't start: as it was
            });
        },
        std::move(back),
        [this, notUnlocked](int64_t other, Nav&) {  // this one stays closed; the switch goes to the one chosen
            app_.defer([this, notUnlocked, other] {
                notUnlocked();
                if (other == openUserId()) return backToOpen();  // the open account: its own lock, never a way past it
                if (!openLocked_) app_.unlock();
                if (!switcher_.switchTo(other, AccountSwitcher::Why::User) && openUserId()) backToOpen();
            });
        }));
}

void AccountsUi::showOpenLock() {
    const int64_t open = openUserId();
    if (!open) return;
    app_.lock(pad(
        open, PinPadScreen::Purpose::Unlock,
        [this](Nav&) {
            app_.defer([this] {
                openLocked_ = false;  // its right PIN, or the reset: the only way the lock goes
                app_.unlock();
            });
        },
        nullptr,
        [this](int64_t other, Nav&) {
            app_.defer([this, other] {
                if (!switcher_.switchTo(other, AccountSwitcher::Why::User)) showOpenLock();
            });
        }));
}

void AccountsUi::lockNow() {
    const int64_t open = openUserId();
    if (!open) return;
    const auto e = pins_.entry(open);
    if (!e || !e->pin || !e->lockOnSleep) return;
    // Another account's PIN asked over this one: that question goes; this one is what locks.
    if (switcher_.asking()) switcher_.cancelAsking();
    else if (switcher_.busy()) return;  // it is being left: nothing of it shows any more
    if (removing_) {
        app_.toast(nameOf(removing_) + " wasn't removed");
        removing_ = 0;
    }
    log(LogLevel::Info, "accounts: locked on sleep");
    openLocked_ = true;
    showOpenLock();
}

// ---- Remove from this Kindle -----------------------------------------------------------------------

uint64_t AccountsUi::sendNow() {
    AccountSession* s = switcher_.session();
    SyncService* sync = s ? s->sync() : nullptr;
    if (!sync) return 0;
    return sync->pushNow(std::chrono::steady_clock::now() + std::chrono::seconds(60));
}

bool AccountsUi::sent(uint64_t request) {
    AccountSession* s = switcher_.session();
    SyncService* sync = s ? s->sync() : nullptr;
    return !sync || sync->pushDone(request);
}

void AccountsUi::remove(Nav& nav, int64_t userId) {
    const std::string name = nameOf(userId);
    if (userId != openUserId()) {  // nothing of it is open: it goes now
        if (!removeAccount(deps_.paths, userId)) {
            nav.toast("Can't remove " + name + " from the Kindle");
            return;
        }
        nav.pop();  // Remove (its page, shown again, finds it gone and leaves too)
        nav.toast(name + " was removed from this Kindle");
        return;
    }
    std::vector<AccountEntry> others;
    if (const auto st = readAccounts(deps_.paths.accountsFile()))
        for (const auto& e : inOrder(st->accounts))
            if (e.userId != userId) others.push_back(e);
    if (!others.empty()) {  // it goes once the next one is open (the switch stops it first)
        // An account without a PIN first: no other account's PIN in the way of a removal.
        const auto unlocked = std::find_if(others.begin(), others.end(), [](const AccountEntry& e) { return !e.pin; });
        removing_ = userId;
        if (!switcher_.switchTo((unlocked != others.end() ? *unlocked : others.front()).userId, AccountSwitcher::Why::Remove))
            removing_ = 0;
        return;
    }
    // The last one: every screen left and destroyed, its session stopped (as a switch does), then
    // it goes; Setup for a first account takes its place.
    switcher_.closeAll("Removing " + name + "…", [this, userId] {
        if (!removeAccount(deps_.paths, userId)) log(LogLevel::Error, "accounts: %lld can't be removed", static_cast<long long>(userId));
        if (deps_.noAccount) deps_.noAccount(firstAccountSetup());
    });
}

void AccountsUi::finishRemoval() {
    const int64_t id = removing_;
    removing_ = 0;
    const std::string name = nameOf(id);
    if (removeAccount(deps_.paths, id)) app_.toast(name + " was removed from this Kindle");
    else app_.toast("Can't remove " + name + " from the Kindle");
}

void AccountsUi::opened(AccountSession& s) {
    openLocked_ = false;  // opened through its own PIN (or it has none)
    if (removing_ == s.userId()) {  // it was opened again instead (the error screen's choice)
        app_.toast(nameOf(removing_) + " wasn't removed");
        removing_ = 0;
    }
    if (removing_) finishRemoval();
    if (added_ == s.userId()) {
        added_ = 0;
        if (SyncService* sync = s.sync()) sync->syncNow();
    }
}

void AccountsUi::onSyncEvent(const SyncEvent& e) {
    if (e.kind != SyncEvent::Kind::SyncFinished || e.error) return;
    if (AccountSession* s = switcher_.session()) s->keepSummary(true);
}

// ---- Home --------------------------------------------------------------------------------------------

std::string AccountsUi::chip() {
    const auto st = readAccounts(deps_.paths.accountsFile());
    const int64_t open = openUserId();
    if (!st || st->accounts.size() < 2 || !open) return {};
    return nameOf(open);
}

AccountSheet AccountsUi::sheet() {
    auto rows = rowsExcept(0);
    const bool canAdd = rows.size() < kMaxAccounts;
    return AccountSheet(std::move(rows), canAdd, true);
}

void AccountsUi::sheetChoice(AccountSheet::Action a, int64_t userId) {
    switch (a) {
        case AccountSheet::Action::Switch: return switchTo(userId);
        case AccountSheet::Action::Add: return addAccount(app_);
        case AccountSheet::Action::Manage: return app_.push(std::make_unique<AccountsScreen>(*this));
        default: return;
    }
}

}  // namespace zk
