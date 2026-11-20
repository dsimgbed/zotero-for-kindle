#pragma once
// Home's body written by a plugin (Settings → Home; spec 2026-11-19-plugin-home §4).
//
// HomeTreeStore keeps the last tree the chosen home screen rendered, so the next start draws it in
// its very first frame (plugins load lazily, after the first screen): one file, <stateDir>/home-tree.json,
// {pluginId, screenId, tree}, the tree as render returned it. A new valid tree is written at most
// every kKeepEveryMs (a refreshing plugin would otherwise write it each second), the newest one
// waiting meanwhile; what waits is written when Home hides, the body goes, or the app ends. The
// file goes when the host says the chosen screen is missing from its plugin (removed in an update).
//
// PluginHome is HomeScreen's HomeBodyLink over PluginEvents: it owns the PluginBody and is its
// ScreenLink (passing everything to PluginEvents; the kept tree comes first, and each new tree is
// kept). On shown():
//  - the chosen screen ("<pluginId>/<screenId>") opens when there is no body and the plugin can show
//    it now (PluginEvents::homeAvailable: plugins run, it is on and loads; no flash of a kept tree
//    for a plugin that is off). The host parses its trees as TreeUse::HomeBody.
//  - a body of another choice (Settings → Home changed) is dropped first;
//  - an open body renders again.
// A ScreenError (render failed, invalid tree, the plugin off or reloaded, plugins stopped for an
// install) fails the body; HomeScreen then drop()s it and shows the built-in body until the next show.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "screens/home.h"
#include "screens/plugin_body.h"

namespace zk {

class PluginEvents;

class HomeTreeStore {
public:
    explicit HomeTreeStore(std::string file) : file_(std::move(file)) {}
    // The kept tree of that screen (nullopt: none, another screen's, or unreadable).
    std::optional<nlohmann::json> load(const std::string& pluginId, const std::string& screenId) const;
    // Replaces the file (written whole and synced, then renamed over the old one); failures are logged only.
    void save(const std::string& pluginId, const std::string& screenId, const nlohmann::json& tree) const;
    void forget() const;  // no kept tree any more

private:
    std::string file_;
};

class PluginHome final : public HomeBodyLink, public ScreenLink {
public:
    // `choice`: Settings → Home (AppSettings::homeScreen), asked at each show. `items` and `tap`:
    // as plugin screens' (PluginEvents::setItemUi).
    // `clock`: steady ms (default: the system's), for kKeepEveryMs.
    PluginHome(PluginEvents&, HomeTreeStore&, std::function<std::string()> choice, ItemSource* items, ItemTap tap,
               std::function<int64_t()> clock = {});
    ~PluginHome() override;

    static constexpr int64_t kKeepEveryMs = 30000;

    // HomeBodyLink
    PluginBody* body() override { return body_.get(); }
    void shown(Nav&, std::function<Rect(Nav&)> area, std::function<Rect()> window) override;
    void synced() override;
    void drop(Nav&) override;
    void hidden() override { keep(true); }

    // ScreenLink (the body's)
    void send(uint64_t instance, nlohmann::json event) override;
    void left(uint64_t instance) override;
    bool take(uint64_t instance, PluginHostEvent& out) override;
    bool takeItemOpened(uint64_t instance) override;
    void rerender(uint64_t instance) override;

private:
    PluginEvents& events_;
    HomeTreeStore& store_;
    std::function<std::string()> choice_;
    ItemSource* items_;
    ItemTap tap_;
    std::function<Rect(Nav&)> area_;
    std::function<Rect()> window_;
    std::unique_ptr<PluginBody> body_;
    std::string pluginId_, screenId_;       // the body's
    std::optional<PluginHostEvent> kept_;   // the kept tree, not taken by the body yet
    nlohmann::json saved_;                  // the tree last kept (null: none)
    std::function<int64_t()> clock_;
    nlohmann::json waiting_;                // a newer tree than the file's, not written yet (null: none)
    std::optional<int64_t> writtenMs_;      // when the file was last written
    void keep(bool now);                    // writes what waits: `now`, or if kKeepEveryMs have passed
};

}  // namespace zk
