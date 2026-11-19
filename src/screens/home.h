#pragma once
// Home (mockup C, "Hybrid"): title with sync status, search field, Continue reading card,
// Recent items; an error banner above the tab bar when the last sync failed.
//
// A plugin may write the body (Settings → Home; HomeActions::body): the frame stays the app's
// (header, icons, search, banners, tab bar) and the plugin's blocks go in bodyRect(), where the
// card and Recent were. The built-in body is kept ready and shows, with no message, whenever the
// plugin's doesn't: while the body waits for its first tree (no kept tree), and after it failed,
// was closed by the plugin, or had no tree within kFirstTreeMs; the next show tries the plugin
// again. A sync that changed nothing repaints nothing; one that changed an item repaints only
// the blocks showing it.

#include <functional>
#include <optional>
#include <vector>

#include "screens/item_sheet.h"
#include "screens/screen.h"
#include "screens/sync_status.h"

namespace zk {

class PluginBody;

// Home's body written by a plugin (main implements it over the plugin host: app/home_tree.h).
class HomeBodyLink {
public:
    virtual ~HomeBodyLink() = default;
    // The plugin's body (null: the built-in one). Owned by the link; it lives until drop().
    virtual PluginBody* body() = 0;
    // Home shows: the chosen screen opens if it has no body (its kept tree shown at once, if any),
    // else renders again. `area`: where the blocks go; `window`: what the item sheet and the error
    // pop-up cover. Both are asked again at each use, so the latest Home's are used.
    virtual void shown(Nav&, std::function<Rect(Nav&)> area, std::function<Rect()> window) = 0;
    // A sync changed the library: the body renders again.
    virtual void synced() = 0;
    // Home gives up on the body (it failed, the plugin closed it, no tree in time): it goes, and
    // Home shows its built-in body until the next shown().
    virtual void drop(Nav&) = 0;
};

struct HomeActions {
    std::function<void()> openSearch;
    std::function<void()> openSettings;
    std::function<void(const ItemRow&)> openItem;
    StatusFn status;  // download progress/errors for the Recent rows
    // "Version X is available" (UpdateNotice): the version, asked on show and every tick (so it
    // must be cheap: UpdateNotice reads the library only when the updater has news).
    std::function<std::optional<std::string>()> updateNotice;
    std::function<void()> openUpdate;
    std::function<void()> dismissUpdate;  // ×
    // Safe mode, the automatic case: plugins were turned off because the app stopped. True until
    // the user opens Settings → Plugins (here or by hand). A tap on the notice calls openPlugins.
    std::function<bool()> safeModeNotice;
    std::function<void()> openPlugins;
    // Home's body written by a plugin; unset or null: the built-in body only.
    std::function<HomeBodyLink*()> body;
};

class HomeScreen final : public Screen {
public:
    HomeScreen(SyncStatus&, HomeActions, std::function<int64_t()> wallMs);

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;

    // With no kept tree, how long the plugin's body may take to its first tree before Home gives up on it.
    static constexpr int64_t kFirstTreeMs = 1000;
    // Where a plugin's body goes: under the search field, above the banner (when one shows) or the tab bar.
    Rect bodyRect() const;
    bool showsPluginBody() const;  // the plugin's blocks show (else the built-in card and Recent)
    const PluginBody* pluginBody() const { return body(); }

    static Rect syncIndicatorRect();
    static Rect settingsRect();
    // The puzzle-piece icon left of the gear (Plugins, the launcher): only when some plugin has a
    // main screen; a dot on it when a plugin has a badge or plugin updates are waiting.
    static Rect pluginsRect();
    bool pluginsIcon() const { return pluginsIcon_; }
    bool pluginsDot() const { return pluginsDot_; }
    static Rect closeRect();  // "×": close Zotero, back to the Kindle
    static Rect searchFieldRect();
    static Rect cardRect();
    std::vector<Rect> recentRowRects() const;
    // The safe-mode notice: in the error banner's place when there's no error, before the update banner.
    std::optional<std::string> safeModeBannerText() const;
    Rect safeModeBannerRect() const;
    // The update banner: in the error banner's place when there's no error (and no safe-mode notice).
    std::optional<std::string> updateBannerText() const;
    Rect updateBannerRect() const;
    Rect updateDismissRect() const;
    const std::vector<ItemRow>& recent() const { return recent_; }
    const ItemSheet* sheet() const { return sheet_ ? &*sheet_ : nullptr; }

private:
    void reload(Library&);
    // What the header and the content (card, rows, banner) show: a finished sync repaints only
    // what it changed. Most periodic syncs change nothing, and a full-screen repaint every 10
    // minutes (a flash every 6th) costs battery for nothing.
    struct Header {
        std::string line;
        bool syncing = false, error = false;
        bool operator==(const Header&) const = default;
    };
    struct Content {
        std::optional<ItemRow> card;
        bool opened = false;
        int64_t openedMs = 0, total = 0;
        size_t highlights = 0;
        std::vector<ItemRow> recent;
        std::string error;  // the banner's
        bool hasError = false;
        bool operator==(const Content&) const = default;
    };
    Header header() const;
    Content content() const;
    void pluginSyncEvent(const SyncEvent&, PluginBody&, bool headerChanged, const Content& shown, Nav&);
    Rect bannerRect() const;
    bool bannerShown() const {
        return status_.hasError() || safeModeBannerText().has_value() || updateBannerText().has_value();
    }
    std::string cardMeta() const;

    SyncStatus& status_;
    HomeActions actions_;
    std::function<int64_t()> wallMs_;
    int64_t nowMs_ = 0;  // Nav clock at the last event, for the status line
    std::optional<ItemRow> card_;
    bool cardOpened_ = false;
    int64_t cardOpenedMs_ = 0;
    size_t cardHighlights_ = 0;
    std::vector<ItemRow> recent_;
    std::optional<ItemSheet> sheet_;  // long-press on the card or a Recent row
    Rect sheetArea() const;
    int64_t total_ = 0;
    bool safe_ = false;                  // the safe-mode notice is wanted
    std::optional<std::string> update_;  // the version the banner names
    bool pluginsIcon_ = false, pluginsDot_ = false;
    uint64_t pluginsVersion_ = 0;
    bool pluginUpdates_ = false;  // the catalog has updates waiting: the dot too
    bool readPlugins(Nav&);  // true if the icon changed
    // The plugin's body: asked of the link each time (it may have dropped it).
    PluginBody* body() const;
    HomeBodyLink* link_ = nullptr;  // from HomeActions::body at the last show
    int64_t waitingSinceMs_ = 0;    // the body has had no tree since then
    void tickBody(Nav&);            // takes its answers; gives up on it when it can't show
    void fallBack(Nav&);
    Rect bodyRegion() const;        // what a change of the body repaints (the card and rows region)
};

}  // namespace zk
