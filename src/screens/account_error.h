#pragma once
// What shows when the app can't open an account's library (spec: multiple accounts, Switching: "a
// failure building the new session … shows an error with Try again / Switch to <other>"): the
// reason in plain words and the ways on. Without choices (the move into the account layout cut
// short) it offers Close Zotero only: the next start tries again. Nothing of the account is touched.

#include <functional>
#include <string>
#include <vector>

#include "screens/screen.h"

namespace zk {

class AccountErrorScreen final : public Screen {
public:
    struct Choice {
        std::string label;  // "Try again", "Switch to Pavel", "Close Zotero"
        std::function<void(Nav&)> act;
    };
    AccountErrorScreen(std::string title, std::string reason, std::vector<Choice> choices = {});

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    bool showsTabBar() const override { return false; }

    const std::string& title() const { return title_; }
    const std::string& reason() const { return reason_; }
    const std::vector<Choice>& choices() const { return choices_; }
    static Rect choiceRect(size_t i);
    static Rect closeRect() { return choiceRect(0); }  // without choices: Close Zotero

private:
    std::string title_, reason_;
    std::vector<Choice> choices_;
    bool restartNote_;  // no choices given: the next start tries again
};

}  // namespace zk
