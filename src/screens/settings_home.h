#pragma once
// Settings → Home: what Home's body is, the built-in one or a plugin's home screen (zk.screen's
// `home`). Offered: "Built-in", then "<Screen title> — <Plugin name>" for each home screen of the
// enabled, running plugins (Nav::pluginEntries). A stored choice no longer offered (the plugin
// removed, turned off, incompatible, plugins not running, the screen gone) is kept, and named in a
// note under the options, "<Plugin name> — not available"; Home shows the built-in body meanwhile.
// A tap on an option keeps it (AppSettings::homeScreen) at once; Back returns to Settings.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "screens/plugins.h"
#include "screens/screen.h"

namespace zk {

struct HomeChoices {
    struct Option {
        std::string value;  // AppSettings::homeScreen: "" built-in, else "<pluginId>/<screenId>"
        std::string label;  // "Built-in", "<Screen title> — <Plugin name>"
    };
    std::vector<Option> options;  // "Built-in" first
    std::string stored;           // AppSettings::homeScreen
    std::string unavailable;      // "<Plugin name> — not available" when `stored` isn't offered, else ""
    int chosen() const;           // the stored option's index; -1 when it isn't offered
    std::string line() const;     // Settings' row: the chosen option's label, else `unavailable`
};

// `host` (may be null) names the plugin of a stored choice that isn't running (else its id).
HomeChoices homeChoices(Nav&, PluginHost* host);

class HomeSettingScreen final : public Screen {
public:
    explicit HomeSettingScreen(PluginHostFn plugins = nullptr);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;  // plugins loading (or going) meanwhile: the options change
    bool showsTabBar() const override { return false; }

    const HomeChoices& choices() const { return choices_; }
    std::vector<Rect> optionRects() const;  // as many options as fit, in order
    Rect noteRect() const;                  // under the options

private:
    void load(Nav&);
    PluginHostFn plugins_;
    HomeChoices choices_;
    std::optional<uint64_t> version_;  // the plugin entries' version at the last load
};

}  // namespace zk
