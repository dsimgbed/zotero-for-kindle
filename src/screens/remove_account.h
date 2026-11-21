#pragma once
// Remove from this Kindle (spec: multiple accounts, Screens): what goes — items, what it takes on
// the Kindle, changes not sent to Zotero — and, with unsent changes, Send now first (the open
// account, online). The button says what it does ("Remove Anna and delete 1.2 GB"); an account with
// a PIN asks it first, but a locked one can be removed without it, behind a warning that its data
// on this Kindle is deleted. The removal itself (AccountsHost::remove) leaves for another account
// or Setup if it was the open one.

#include <cstdint>

#include "screens/accounts.h"
#include "screens/screen.h"

namespace zk {

class RemoveAccountScreen final : public Screen {
public:
    RemoveAccountScreen(AccountsHost&, int64_t userId);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    const AccountSummary& shown() const { return account_; }
    std::string removeLabel() const;  // "Remove Anna and delete 1.2 GB"
    std::string unsentLine() const;   // "" without unsent changes
    Rect sendRect() const;            // Send now (empty when not offered)
    Rect removeRect() const;
    bool sending() const { return request_ != 0; }

private:
    void load();
    AccountsHost& host_;
    int64_t userId_;
    AccountSummary account_;
    bool online_ = false;
    uint64_t request_ = 0;
    bool sendFailed_ = false;
};

}  // namespace zk
