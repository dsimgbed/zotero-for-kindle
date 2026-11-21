#pragma once
// Settings → Accounts (spec: multiple accounts, Screens): the accounts on this Kindle, + Add account
// (up to three), and one page per account: Switch to, PIN (set / change / remove), Lock on sleep,
// Update key, Remove from this Kindle. A locked account (a PIN, not open) shows its name and a lock
// only, here as everywhere: its page offers Switch to (which asks the PIN) and Remove.
//
// The screens reach the accounts through AccountsHost (main's AccountsUi): what is on the Kindle,
// and the actions, which may switch the account, push Setup or the PIN pad.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "screens/pin_pad.h"
#include "screens/screen.h"

namespace zk {

struct AccountSummary {
    int64_t userId = 0;
    std::string name;  // "" before the name is known: "Account <id>"
    bool active = false;
    bool locked = false;  // a PIN, and not the open account: nothing of it is shown
    bool hasPin = false, lockOnSleep = false;
    // Unlocked accounts only:
    int64_t items = 0;
    int unsent = 0;             // changes made on the Kindle not sent to Zotero
    int64_t syncedWallMs = 0;   // the last finished sync (0: not known)
    int64_t bytes = -1;         // everything of it on the Kindle (-1: not measured)
    std::string keyLine;        // "User ID 11 · WebDAV: dav.example.org"
};

std::string accountName(const AccountSummary&);
// "412 items · synced 10 min ago", "1,280 items · 3 changes not sent", "412 items"; "" when locked.
std::string accountLine(const AccountSummary&, int64_t nowWallMs);
// "1.2 GB", "340 MB", "12 KB".
std::string sizeText(int64_t bytes);

class AccountsHost {
public:
    virtual ~AccountsHost() = default;
    // In their order (counts as each account's session last kept them; the open one's live).
    // `measure`: that account's size on the Kindle too (walks its folder).
    virtual std::vector<AccountSummary> accounts(int64_t measure = 0) = 0;
    virtual int64_t wallMs() = 0;
    virtual void switchTo(int64_t userId) = 0;  // the account switch (the PIN first, if it has one)
    virtual void addAccount(Nav&) = 0;          // Setup for another Zotero user
    virtual void updateKey(Nav&, int64_t userId) = 0;  // Setup for a new key of the same user
    // The PIN pad for one of the PIN's changes (Set, Change, Remove) or to check it (Verify).
    virtual std::unique_ptr<Screen> pinPad(int64_t userId, PinPadScreen::Purpose, std::function<void(Nav&)> done) = 0;
    virtual void setLockOnSleep(int64_t userId, bool on) = 0;
    // Remove from this Kindle: Send now first (the open account, online), then the removal, which
    // leaves the account's screens (a switch, Setup, or back to the list).
    virtual bool online() = 0;
    virtual uint64_t sendNow() = 0;  // a request (0: can't send now)
    virtual bool sent(uint64_t request) = 0;
    virtual void remove(Nav&, int64_t userId) = 0;
};

class AccountsScreen final : public Screen {
public:
    explicit AccountsScreen(AccountsHost&);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }

    const std::vector<AccountSummary>& shown() const { return accounts_; }
    Rect rowRect(size_t i) const;
    Rect addRect() const;  // empty at three accounts

private:
    AccountsHost& host_;
    std::vector<AccountSummary> accounts_;
    int64_t nowWall_ = 0;
};

class AccountPageScreen final : public Screen {
public:
    AccountPageScreen(AccountsHost&, int64_t userId);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }

    const AccountSummary& shown() const { return account_; }
    // Empty when not offered.
    Rect switchRect() const;
    Rect setPinRect() const;     // no PIN yet
    Rect changePinRect() const;  // with a PIN
    Rect removePinRect() const;
    Rect lockToggleRect() const;
    Rect updateKeyRect() const;
    Rect removeRect() const;

private:
    int pinY() const;
    int lockY() const;
    int keyY() const;
    int removeY() const;
    AccountsHost& host_;
    int64_t userId_;
    AccountSummary account_;
    bool gone_ = false;  // removed meanwhile
};

}  // namespace zk
