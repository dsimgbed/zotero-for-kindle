#include "app/account_switch.h"

#include <algorithm>
#include <chrono>

#include "app/app.h"
#include "core/log.h"
#include "net/errors.h"
#include "screens/account_error.h"

namespace zk {

AccountSwitcher::AccountSwitcher(App& app, SwitcherDeps deps) : app_(app), deps_(std::move(deps)) {
    if (!deps_.credentials) deps_.credentials = [paths = deps_.paths](int64_t userId) { return loadCredentials(paths, userId); };
    if (!deps_.online) deps_.online = deps_.session.wifiUp ? deps_.session.wifiUp : [] { return probeNetState().wifiConnected; };
}

// (main's hooks may refer to what is already gone here: its exit calls close() itself.)
AccountSwitcher::~AccountSwitcher() { stopOld(); }

AccountEntry AccountSwitcher::entryFor(int64_t userId) const {
    AccountEntry e;
    e.userId = userId;
    if (const auto st = readAccounts(deps_.paths.accountsFile()))
        for (const auto& a : st->accounts)
            if (a.userId == userId) e = a;
    return e;
}

std::string AccountSwitcher::nameOf(int64_t userId) const {
    const AccountEntry e = entryFor(userId);
    return e.name.empty() ? "account " + std::to_string(userId) : e.name;
}

bool AccountSwitcher::switchTo(int64_t userId, Why why) {
    if (busy() || userId <= 0) return false;
    if (session_ && session_->userId() == userId) return true;  // already open
    target_ = userId;
    why_ = why;
    if (session_) previous_ = session_->userId();
    else if (why == Why::Startup) previous_ = 0;  // (after a failure: the account that was open before it)
    targetName_ = nameOf(userId);
    previousName_ = previous_ > 0 ? nameOf(previous_) : "";
    startedMs_ = app_.nowMs();
    skipped_ = false;
    pushRequest_ = 0;
    armed_ = false;
    phase_ = Phase::Leaving;
    log(LogLevel::Info, "accounts: switching to %lld", static_cast<long long>(userId));
    // 1. Every screen left (destroyed after this step): none keeps the old library; then what is
    // bound to it outside the session.
    app_.clearTo(std::make_unique<SwitchingScreen>([this] { return view(); }, [this] { skip(); }, [this](Nav&) { tick(); }));
    if (session_ && leave_) leave_();
    if (!session_) {  // nothing to send or stop
        proceed();
        return true;
    }
    // The rest from the Switching screen's ticks, once the screens left are gone (after this step).
    app_.defer([this] {
        armed_ = true;
        app_.wakeAt(app_.nowMs());
    });
    return true;
}

void AccountSwitcher::skip() {
    if (phase_ != Phase::Pushing) return;
    skipped_ = true;
    app_.wakeAt(app_.nowMs());
}

void AccountSwitcher::tick() {
    const int64_t now = app_.nowMs();
    if (phase_ == Phase::Leaving) {
        if (!armed_) return;
        SyncService* sync = session_ ? session_->sync() : nullptr;
        if (!sync || !deps_.online()) return proceed();
        // 2. What is unsent goes now, for at most the budget.
        pushUntilMs_ = now + deps_.pushBudgetMs;
        pushRequest_ = sync->pushNow(std::chrono::steady_clock::now() + std::chrono::milliseconds(deps_.pushBudgetMs));
        phase_ = Phase::Pushing;
        log(LogLevel::Info, "accounts: sending what account %lld hasn't sent (at most %lld s)",
            static_cast<long long>(previous_), static_cast<long long>(deps_.pushBudgetMs / 1000));
    }
    if (phase_ != Phase::Pushing) return;
    SyncService* sync = session_ ? session_->sync() : nullptr;
    const bool done = !sync || sync->pushDone(pushRequest_);
    if (done || skipped_ || now >= pushUntilMs_) {
        if (!done) log(LogLevel::Info, "accounts: %s; what isn't sent stays queued", skipped_ ? "skipped sending" : "sending took too long");
        return proceed();
    }
    app_.wakeAt(std::min(pushUntilMs_, now + 1000));  // the countdown, and the end of the budget
}

void AccountSwitcher::proceed() {
    stopOld();  // 3.
    // 4. After the PIN, if the account has one.
    const AccountEntry e = entryFor(target_);
    if (!e.pin) return open();
    if (!deps_.unlock) return failed("it is locked and its PIN can't be asked here");
    phase_ = Phase::Unlocking;
    deps_.unlock(target_, [this, target = target_](bool unlocked) {
        if (phase_ != Phase::Unlocking || target != target_) return;  // a stale answer
        if (unlocked) return open();
        phase_ = Phase::Idle;  // the lock screen took the user elsewhere
        log(LogLevel::Info, "accounts: %lld left locked", static_cast<long long>(target));
    });
}

void AccountSwitcher::stopOld() {
    if (!session_) return;
    app_.setSession(nullptr);
    session_->stop();  // downloads, plugin host, sync, workers, caches, library: in that order
    session_.reset();
}

void AccountSwitcher::writeActive() {
    const std::string file = deps_.paths.accountsFile();
    auto st = readAccounts(file);
    if (!st) {
        log(LogLevel::Warn, "accounts: no account list to mark %lld active in", static_cast<long long>(target_));
        return;
    }
    if (st->active == target_) return;
    st->active = target_;
    if (!writeAccounts(file, *st)) log(LogLevel::Error, "accounts: can't write the account list");
}

void AccountSwitcher::open() {
    const std::optional<Config> config = deps_.credentials(target_);
    if (!config) return failed("its key isn't on the Kindle");
    std::string why;
    auto s = AccountSession::open(deps_.paths, entryFor(target_), *config, *deps_.device, &why, deps_.session);
    if (!s) return failed(why);
    session_ = std::move(s);
    app_.setSession(session_.get());
    writeActive();  // 5.: only an account that opened is the next start's
    phase_ = Phase::Idle;
    lastSwitchMs_ = app_.nowMs() - startedMs_;
    log(LogLevel::Info, "accounts: %lld open (%lld ms)", static_cast<long long>(target_), static_cast<long long>(lastSwitchMs_));
    if (opened_) opened_(*session_);
}

void AccountSwitcher::failed(const std::string& why) {
    phase_ = Phase::Idle;
    log(LogLevel::Error, "accounts: %lld can't be opened: %s", static_cast<long long>(target_), why.c_str());
    std::vector<AccountErrorScreen::Choice> choices;
    choices.push_back({"Try again", [this, target = target_, w = why_](Nav&) { switchTo(target, w); }});
    // Every other account on the Kindle: the user is never left with only the broken one.
    std::vector<AccountEntry> others;
    if (const auto st = readAccounts(deps_.paths.accountsFile()))
        for (const auto& a : st->accounts)
            if (a.userId != target_) others.push_back(a);
    std::stable_sort(others.begin(), others.end(), [](const AccountEntry& a, const AccountEntry& b) { return a.order < b.order; });
    for (const auto& a : others)
        choices.push_back({"Switch to " + (a.name.empty() ? "account " + std::to_string(a.userId) : a.name),
                           [this, id = a.userId](Nav&) { switchTo(id, Why::User); }});
    if (others.empty()) {
        if (setup_) choices.push_back({"Set up", [this](Nav&) { setup_(); }});
        else choices.push_back({"Close Zotero", [](Nav& nav) { nav.quit(); }});
    }
    app_.replaceRoot(0, std::make_unique<AccountErrorScreen>("Can't open the library",
                                                             "The library of " + targetName_ + " can't be opened: " + why,
                                                             std::move(choices)));
}

SwitchingScreen::View AccountSwitcher::view() const {
    SwitchingScreen::View v;
    v.title = "Switching to " + targetName_ + "…";
    if (phase_ == Phase::Pushing) {
        const int64_t left = std::max<int64_t>(0, (pushUntilMs_ - app_.nowMs() + 999) / 1000);
        v.progress = "Sending what " + previousName_ + " changed on this Kindle to Zotero… " + std::to_string(left) + " s";
        v.canSkip = true;
    }
    return v;
}

void AccountSwitcher::close() {
    if (!session_) return;
    if (leave_) leave_();
    stopOld();
    phase_ = Phase::Idle;
}

}  // namespace zk
