#pragma once
// A plugin's screen (zk.screen): the header with the screen's title and the plugin's name (so it
// can't pass for the app's own confirmation), and the plugin's blocks below (plugin_blocks.h).
// The plugin thread runs render/onEvent; this screen only sends what the user did and draws the
// trees that come back (through a ScreenLink: PluginEvents over the PluginHost).
//
// - "Loading…" until the first tree; "<Plugin> can't show this screen right now" with Back when
//   the host says so (ScreenError).
// - Below the header, a PluginBody (plugin_body.h) does the rest: the event queue, "Working…",
//   the action error pop-up, typing (the blocks above the keyboard), diffs, items taps.
// - Back always works: the screen closes at once and the plugin hears `back` afterwards.

#include <cstdint>
#include <string>

#include "screens/plugin_body.h"
#include "screens/screen.h"

namespace zk {

class PluginScreen final : public Screen {
public:
    static constexpr int64_t kPauseMs = PluginBody::kPauseMs;      // typing → `change`
    static constexpr int64_t kWorkingMs = PluginBody::kWorkingMs;  // waiting → "Working…"
    using State = PluginBody::State;

    // `items` resolves the refs of items blocks (null: they draw nothing); it outlives the screen.
    PluginScreen(ScreenLink&, uint64_t instance, std::string pluginName, std::string title, ItemSource* items = nullptr,
                 ItemTap tap = {});

    // The plugin's folder, where its images are (none: images take no room).
    void setPackage(std::string dir) { body_.setPackage(std::move(dir)); }

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onKey(const KeyEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    void onClose(Nav&) override;
    bool wantsKeyboard() const override { return body_.wantsKeyboard(); }
    bool showsTabBar() const override { return false; }
    bool closesWhenHidden() const override { return true; }

    uint64_t instance() const { return body_.instance(); }
    State state() const { return body_.state(); }
    const std::string& errorText() const { return body_.errorText(); }
    bool working() const { return body_.working(); }
    size_t waiting() const { return body_.waiting(); }  // events not sent yet
    bool sending() const { return body_.sending(); }    // an event is with the plugin
    const Laid* laid() const { return body_.laid(); }
    const FocusState& focus() const { return body_.focus(); }
    const PluginResult* errorPopup() const { return body_.errorPopup(); }
    const ItemSheet* sheet() const { return body_.sheet(); }  // a long-press on an item
    Rect area(Nav& nav) const { return body_.area(nav); }    // where the blocks go (above the keyboard)
    static Rect workingRect();
    static Rect backButtonRect();  // the error state's Back button

private:
    void leave(Nav&);
    int nameReserve() const;

    const std::string pluginName_, title_;
    PluginBody body_;
};

}  // namespace zk
