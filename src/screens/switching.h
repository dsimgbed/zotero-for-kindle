#pragma once
// The neutral screen shown while the app goes from one account to another (spec: multiple
// accounts, Switching, step 1): it holds nothing of either account. While the old account's unsent
// changes are being sent it says so, with Skip. The switch is driven from its tick.

#include <functional>
#include <string>

#include "screens/screen.h"

namespace zk {

class SwitchingScreen final : public Screen {
public:
    struct View {
        std::string title;     // "Switching to Anna…"
        std::string progress;  // "Sending changes to Zotero… 12 s" ("" while not sending)
        bool canSkip = false;
        bool operator==(const View&) const = default;
    };
    // `view`: what to show now; `skip`: Skip was tapped; `tick`: every loop step while shown.
    SwitchingScreen(std::function<View()> view, std::function<void()> skip, std::function<void(Nav&)> tick);

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    static Rect skipRect();
    const View& shown() const { return shown_; }

private:
    std::function<View()> view_;
    std::function<void()> skip_;
    std::function<void(Nav&)> tick_;
    View shown_;
};

}  // namespace zk
