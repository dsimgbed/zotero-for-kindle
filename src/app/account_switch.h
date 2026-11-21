#pragma once
// Going from one account to another in-process (spec: multiple accounts, The account session →
// Switching), and the one path by which any account's session is opened or closed: at startup
// (switchTo(active, Startup)), on a switch, on removal, at exit (close()). In a fixed order:
//   1. every screen is left: the navigation is cleared to the neutral Switching screen;
//   2. online, the old account's unsent changes are sent, for at most the push budget (15 s; Skip
//      ends it at once); what isn't sent stays queued in that account's library;
//   3. the old session stops (downloads cancelled, plugin host, sync, workers, caches, library)
//      and is destroyed;
//   4. the new session is opened (after the PIN, if the account has one; from an open account, the
//      PIN is asked before step 1, so a wrong or cancelled PIN leaves the open account as it was);
//   5. once it is open, `active` is written to accounts.json, its services start and its Home
//      shows (main's onOpened).
// `active` names only an account that opened, so a library that can't be opened is never the one
// every later start tries first. A switch can't start while one runs; while it runs, plugins may
// put nothing over the Switching screen (PluginEvents::setRefusing). If the new session can't be
// opened, an error offers Try again, Switch to <name> for each other account on the Kindle, or —
// with none — Set up (main's onSetup); nothing of any account's files is touched. Screen thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/accounts.h"
#include "app/session.h"
#include "screens/switching.h"
#include "sync/config.h"

namespace zk {

class App;
class DeviceSettings;

struct SwitcherDeps {
    Paths paths;
    DeviceSettings* device = nullptr;  // required
    SessionDeps session;                // what each session is opened with
    // The account's key (default: loadCredentials(paths, userId)).
    std::function<std::optional<Config>(int64_t userId)> credentials;
    // Asks the account's PIN (the lock: the PIN pad); answers true once unlocked, false if the user
    // went elsewhere. Unset: an account with a PIN can't be opened. From an open account it is asked
    // before anything is left; with none open (startup), before the account is opened.
    std::function<void(int64_t userId, std::function<void(bool unlocked)>)> unlock;
    // Whether the old account's changes can be sent now (default: session.wifiUp, else the device's).
    std::function<bool()> online;
    int64_t pushBudgetMs = 15'000;
};

class AccountSwitcher {
public:
    enum class Why { User, Remove, Startup };

    AccountSwitcher(App&, SwitcherDeps);
    ~AccountSwitcher();  // the session stopped (without onLeave: call close() first)
    AccountSwitcher(const AccountSwitcher&) = delete;
    AccountSwitcher& operator=(const AccountSwitcher&) = delete;

    // main's wiring around a session. onLeave: what is bound to the open session outside it (Home's
    // plugin body, the items blocks, a background round) let go, at step 1. onOpened: the new
    // session bound, its services started and its screens set (Home last), at step 5.
    void onLeave(std::function<void()> f) { leave_ = std::move(f); }
    void onOpened(std::function<void(AccountSession&)> f) { opened_ = std::move(f); }
    // The error's way out when no other account is on the Kindle: Setup. Unset: Close Zotero.
    void onSetup(std::function<void()> f) { setup_ = std::move(f); }

    // Starts going to `userId`'s account; false (nothing done) while a switch runs. With no account
    // open (startup, after a failure) it is done before returning, unless a PIN is asked; else it
    // goes on from the Switching screen's ticks.
    bool switchTo(int64_t userId, Why);
    bool busy() const { return phase_ != Phase::Idle; }
    // Another account's PIN is being asked, nothing left yet; cancelAsking drops that question
    // (its answer, when it comes, is ignored) and the open account stays.
    bool asking() const { return phase_ == Phase::Asking; }
    void cancelAsking();
    // The open account left and stopped as a switch does (steps 1 and 3, without the push: every
    // screen goes first, and the session stops a step later, once they are destroyed), then
    // `then` (removing the last account: Setup). The Switching screen says `title`. False while a
    // switch runs.
    bool closeAll(std::string title, std::function<void()> then);
    // Skip on the Switching screen: the push ends now; the switch goes on.
    void skip();
    // Moves a running switch on: from every loop step (main's poll hook) and the Switching screen's
    // tick, so nothing shown can hold it up. Nothing while no switch runs.
    void tick();

    AccountSession* session() const { return session_.get(); }
    // At exit: the open session left and stopped as in steps 1 and 3 (no push, no screen change
    // beyond what the caller did). Idempotent.
    void close();

    // How long the last switch took from switchTo to the new session's onOpened (ms of App's clock).
    int64_t lastSwitchMs() const { return lastSwitchMs_; }

private:
    enum class Phase { Idle, Asking, Leaving, Pushing, Unlocking, Closing };

    void proceed();    // steps 3–5
    void stopOld();    // step 3
    void writeActive();  // once open
    void open();       // step 5, after the PIN
    void failed(const std::string& why);
    SwitchingScreen::View view() const;
    AccountEntry entryFor(int64_t userId) const;
    std::string nameOf(int64_t userId) const;

    App& app_;
    SwitcherDeps deps_;
    std::function<void()> leave_;
    std::function<void(AccountSession&)> opened_;
    std::function<void()> setup_;
    std::unique_ptr<AccountSession> session_;
    Phase phase_ = Phase::Idle;
    int64_t target_ = 0, previous_ = 0;
    Why why_ = Why::User;
    uint64_t pushRequest_ = 0;
    int64_t pushUntilMs_ = 0, startedMs_ = 0, lastSwitchMs_ = -1;
    bool skipped_ = false;
    int64_t unlockedAhead_ = 0;  // its PIN was right before the switch began
    std::string closingTitle_;
    std::function<void()> closed_;
    bool armed_ = false;  // the screens left are gone: the Switching screen's ticks go on with it
    std::string targetName_, previousName_;
};

}  // namespace zk
