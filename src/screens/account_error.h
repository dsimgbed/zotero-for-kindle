#pragma once
// What shows when the app can't open an account's library at startup (spec: multiple accounts,
// Switching: "a failure building the new session … shows an error"): the reason in plain words and
// Close Zotero. Nothing of the account is touched; the next start tries again. (The account
// screens add Try again / Switch to another account.)

#include <string>

#include "screens/screen.h"

namespace zk {

class AccountErrorScreen final : public Screen {
public:
    AccountErrorScreen(std::string title, std::string reason);

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    bool showsTabBar() const override { return false; }

    const std::string& title() const { return title_; }
    const std::string& reason() const { return reason_; }
    static Rect closeRect();

private:
    std::string title_, reason_;
};

}  // namespace zk
