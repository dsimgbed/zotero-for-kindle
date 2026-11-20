#include "screens/home.h"

#include "app/catalog_service.h"

#include "screens/item_details.h"
#include "screens/plugin_body.h"
#include "screens/plugin_entries.h"
#include "screens/plugin_launcher.h"

#include <algorithm>

#include <ctime>

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }
constexpr int kCardY = 292, kCoverH = 400;
constexpr int kRecentLabelY = kCardY + kCoverH + 80;
constexpr int kRowsY = kRecentLabelY + 56;
constexpr int kBannerH = 150;
constexpr int kBodyBottomGap = 12;  // a plugin's body: above the banner or the tab bar
constexpr int64_t kResyncAfterMs = 2 * 60'000;

}  // namespace

HomeScreen::HomeScreen(SyncStatus& status, HomeActions actions, std::function<int64_t()> wallMs)
    : status_(status), actions_(std::move(actions)), wallMs_(std::move(wallMs)) {}

// Top right: settings, sync, close.
Rect HomeScreen::closeRect() { return {W() - kMargin - 80, 34, 80, 96}; }
Rect HomeScreen::syncIndicatorRect() { return {W() - kMargin - 180, 34, 80, 96}; }
Rect HomeScreen::settingsRect() { return {W() - kMargin - 280, 34, 80, 96}; }
Rect HomeScreen::pluginsRect() { return {W() - kMargin - 380, 34, 80, 96}; }
Rect HomeScreen::searchFieldRect() { return {kMargin, 150, W() - 2 * kMargin, 92}; }
Rect HomeScreen::cardRect() { return {kMargin, kCardY, W() - 2 * kMargin, kCoverH}; }

Rect HomeScreen::bannerRect() const { return {0, H() - kTabBarH - kBannerH, W(), kBannerH}; }

std::optional<std::string> HomeScreen::safeModeBannerText() const {
    if (!safe_ || status_.hasError()) return std::nullopt;
    return "Plugins were turned off because the app stopped. Settings → Plugins";
}

Rect HomeScreen::safeModeBannerRect() const { return safeModeBannerText() ? bannerRect() : Rect{}; }

std::optional<std::string> HomeScreen::updateBannerText() const {
    if (!update_ || status_.hasError() || safe_) return std::nullopt;
    return "Version " + *update_ + " is available";
}

Rect HomeScreen::updateBannerRect() const { return updateBannerText() ? bannerRect() : Rect{}; }

Rect HomeScreen::updateDismissRect() const {
    const Rect b = updateBannerRect();
    return b.empty() ? Rect{} : Rect{b.x + b.w - kMargin - 120, b.y, 120 + kMargin, b.h};
}

Rect HomeScreen::bodyRect() const {
    const int bottom = bannerShown() ? bannerRect().y : H() - kTabBarH;
    return {kMargin, kCardY, W() - 2 * kMargin, bottom - kBodyBottomGap - kCardY};
}

Rect HomeScreen::bodyRegion() const { return {0, kCardY, W(), H() - kCardY - kTabBarH}; }

PluginBody* HomeScreen::body() const { return link_ ? link_->body() : nullptr; }

bool HomeScreen::showsPluginBody() const {
    const PluginBody* b = body();
    return b && b->state() == PluginBody::State::Shown;
}

void HomeScreen::fallBack(Nav& nav) {
    const bool drawn = showsPluginBody();  // else the built-in body shows already (or a failure redrew all)
    link_->drop(nav);
    if (drawn) {
        reload(nav.library());
        nav.damage(bodyRegion(), Waveform::Quality);
    }
}

void HomeScreen::tickBody(Nav& nav) {
    PluginBody* b = body();
    if (!b) return;
    b->onTick(nav);
    if (b->state() == PluginBody::State::Failed || b->closedByPlugin()) return fallBack(nav);
    if (b->state() != PluginBody::State::Loading) return;
    if (nav.nowMs() - waitingSinceMs_ >= kFirstTreeMs) fallBack(nav);  // too slow: the built-in body stays
    else nav.wakeAt(waitingSinceMs_ + kFirstTreeMs);
}

std::vector<Rect> HomeScreen::recentRowRects() const {
    std::vector<Rect> rows;
    const int bottom = bannerShown() ? bannerRect().y : H() - kTabBarH;
    for (int i = 0, y = kRowsY; i < static_cast<int>(recent_.size()) && y + kRowH <= bottom; ++i, y += kRowH)
        rows.push_back({kMargin, y, W() - 2 * kMargin, kRowH});
    return rows;
}

void HomeScreen::reload(Library& lib) {
    ListQuery all;
    total_ = lib.countRows(all);
    card_.reset();
    cardOpened_ = false;
    cardHighlights_ = 0;
    if (auto last = lib.lastOpened()) {
        card_ = lib.rowFor(last->itemKey, last->attachmentKey);  // Continue reading: the very file read last
        cardOpened_ = card_.has_value();
        cardOpenedMs_ = last->openedMs;
    }
    if (!card_) {  // nothing opened yet: suggest the newest readable item
        ListQuery q;
        q.limit = 20;
        for (const auto& r : lib.rows(q))
            if (!r.badge.empty()) {
                card_ = r;
                break;
            }
    }
    if (card_ && !card_->attachmentKey.empty()) cardHighlights_ = lib.annotationsFor(card_->attachmentKey).size();
    // Recent: the books read last (after the one in the card), then the newest items if fewer —
    // as many as fit under the card (3 on the PW5).
    const size_t fit = static_cast<size_t>(std::max(1, (H() - kTabBarH - kRowsY) / kRowH));
    recent_.clear();
    auto seen = [&](const std::string& key) {
        if (card_ && card_->key == key) return true;
        for (const auto& r : recent_)
            if (r.key == key) return true;
        return false;
    };
    for (auto& r : lib.recentlyOpened(static_cast<int>(fit) + 1))
        if (recent_.size() < fit && !seen(r.key)) recent_.push_back(std::move(r));
    if (recent_.size() < fit) {
        ListQuery newest;
        newest.limit = static_cast<int>(2 * fit);
        for (auto& r : lib.rows(newest))
            if (recent_.size() < fit && !seen(r.key)) recent_.push_back(std::move(r));
    }
}

bool HomeScreen::readPlugins(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    const uint64_t v = e ? e->version() : 0;
    const bool updates = nav.catalog() && nav.catalog()->updatesWaiting() > 0;  // plugin updates waiting
    if (e && v == pluginsVersion_ && updates == pluginUpdates_) return false;
    pluginsVersion_ = v;
    pluginUpdates_ = updates;
    const bool icon = e && !e->mainScreens().empty(), dot = icon && (e->anyBadge() || updates);
    if (icon == pluginsIcon_ && dot == pluginsDot_) return false;
    pluginsIcon_ = icon;
    pluginsDot_ = dot;
    return true;
}

void HomeScreen::onShow(Nav& nav) {
    readPlugins(nav);
    nowMs_ = nav.nowMs();
    update_ = actions_.updateNotice ? actions_.updateNotice() : std::nullopt;
    safe_ = actions_.safeModeNotice && actions_.safeModeNotice();
    reload(nav.library());  // also with a plugin's body: the built-in one is ready whenever that can't show
    link_ = actions_.body ? actions_.body() : nullptr;
    if (link_) {
        if (PluginBody* old = body()) {  // kept from the last show: failed or closed while Home was hidden → dropped
            old->onTick(nav);              // here, so shown() reopens it (with its kept tree) in this very show
            if (old->state() == PluginBody::State::Failed || old->closedByPlugin()) link_->drop(nav);
        }
        link_->shown(nav, [this](Nav&) { return bodyRect(); }, [this] { return sheetArea(); });
        if (PluginBody* b = body()) {
            b->onShow(nav);
            if (b->state() == PluginBody::State::Loading) waitingSinceMs_ = nav.nowMs();
            tickBody(nav);  // the kept tree (drawn in this first frame), or what came while Home was hidden
        }
    }
    if (!status_.syncing() && (status_.hasError() || status_.msSinceSync(nav.nowMs()) > kResyncAfterMs)) nav.sync().syncNow();
}

void HomeScreen::onTick(Nav& nav) {
    tickBody(nav);
    if (nav.pluginEntries() && readPlugins(nav)) nav.damage(pluginsRect(), Waveform::Ui);
    auto now = actions_.updateNotice ? actions_.updateNotice() : std::nullopt;
    const bool safe = actions_.safeModeNotice && actions_.safeModeNotice();
    if (now == update_ && safe == safe_) return;
    update_ = std::move(now);
    safe_ = safe;
    nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, Waveform::Ui);  // the banner and the rows above it
    if (PluginBody* b = body()) b->reread(nav);  // laid out again in the body's new rect
}

std::string HomeScreen::cardMeta() const {
    return cardMetaText(*card_, cardHighlights_, cardOpened_ ? cardOpenedMs_ : 0, wallMs_());
}

void HomeScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    const int contentW = W() - 2 * kMargin;
    fillRect(cr, {0, 0, W(), H()}, 255);

    // Header: title, status line, settings, sync, close.
    drawText(cr, "Zotero", {kSans, 17, 700}, {kMargin, 18, contentW - 300, 100}, 1);
    const std::string line = status_.line(nowMs_);
    if (!line.empty()) drawText(cr, line, {kSans, 6.5, 400, false, kTertiary}, {kMargin + 4, 108, contentW - 300, 34}, 1);
    const Rect gear = settingsRect();
    gearIcon(cr, gear.x + gear.w / 2.0, gear.y + gear.h / 2.0, kSecondary);
    if (pluginsIcon_) {
        const Rect pr = pluginsRect();
        puzzleIcon(cr, pr.x + pr.w / 2.0, pr.y + pr.h / 2.0, kSecondary);
        if (pluginsDot_) puzzleBadge(cr, pr.x + pr.w / 2.0, pr.y + pr.h / 2.0);
    }
    status_.drawIndicator(cr, syncIndicatorRect());
    const Rect x = closeRect();
    closeIcon(cr, x.x + x.w / 2.0, x.y + x.h / 2.0, kSecondary);

    // Search field.
    const Rect search = searchFieldRect();
    gray(cr, 170);
    cairo_set_line_width(cr, 3);
    roundedRect(cr, search.x + 1.5, search.y + 1.5, search.w - 3, search.h - 3, search.h / 2.0);
    cairo_stroke(cr);
    searchIcon(cr, search.x + 54, search.y + search.h / 2.0 - 4, kTertiary);
    centredText(cr, "Search your library…", {kSans, 9, 400, false, kTertiary},
                {search.x + 96, search.y, search.w - 120, search.h}, Align::Left);

    const bool plugin = showsPluginBody();
    if (plugin) {
        // The plugin's blocks are drawn after the banners (its item sheet and pop-up go over them).
    } else if (total_ == 0) {
        std::string msg = "Your library is empty here. Tap ⟳ to sync.";
        if (status_.syncing()) {
            const std::string prefix = "Syncing…";
            const std::string now = status_.line(nowMs_);  // a group's pass: "Syncing Lab group… 12 of 40"
            msg = now.rfind(prefix, 0) == 0 ? "Syncing your library…" + now.substr(prefix.size()) : now;
        }
        drawText(cr, msg, {kSerif, 11, 400, false, kSecondary}, {kMargin + 40, 560, contentW - 80, 200}, 3, Align::Center);
    } else if (card_) {
        // Continue reading (or, before anything was opened, the newest readable item).
        drawItemCard(cr, cardRect(), *card_, cardOpened_ ? "CONTINUE READING" : "START READING", cardMeta(),
                     actions_.status ? actions_.status(card_->attachmentKey) : "");
    }
    if (total_ > 0 && !plugin) {
        hRule(cr, kCardY + kCoverH + 44, 0, W());
        drawText(cr, "RECENT", {kSans, 7, 700, false, kSecondary}, {kMargin, kRecentLabelY, contentW, 50}, 1);
        auto rects = recentRowRects();
        for (size_t i = 0; i < rects.size(); ++i)
            drawItemRow(cr, rects[i], recent_[i], actions_.status ? actions_.status(recent_[i].attachmentKey) : "");
    }
    if (status_.hasError()) {
        drawBanner(cr, bannerRect(), status_.error());
    } else if (const auto notice = safeModeBannerText()) {
        const Rect b = bannerRect();
        fillRect(cr, b, 235);
        fillRect(cr, {b.x, b.y, 8, b.h}, kInk);
        drawText(cr, *notice, {kSans, 7.5, 500}, {b.x + kMargin, b.y + 44, b.w - 2 * kMargin, 64}, 1);
    } else if (const auto text = updateBannerText()) {
        const Rect b = bannerRect(), x = updateDismissRect();
        fillRect(cr, b, 235);
        fillRect(cr, {b.x, b.y, 8, b.h}, kInk);
        drawText(cr, *text, {kSans, 8, 500}, {b.x + kMargin, b.y + 26, x.x - b.x - kMargin, 50}, 1);
        drawText(cr, "Tap to see what's new and update.", {kSans, 6.5, 400, false, kSecondary},
                 {b.x + kMargin, b.y + 84, x.x - b.x - kMargin, 40}, 1);
        centredText(cr, "×", {kSans, 12, 400}, {x.x, x.y, x.w - kMargin, x.h});
    }
    if (plugin) body()->draw(cr);
    if (sheet_) sheet_->draw(cr, sheetArea());
}

Rect HomeScreen::sheetArea() const { return {0, 0, metrics().width, metrics().height - kTabBarH}; }

void HomeScreen::onGesture(const Gesture& g, Nav& nav) {
    if (sheet_) {
        const Rect area = sheetArea();
        switch (sheet_->onGesture(g, area, nav)) {
            case ItemSheet::Action::Close:
                sheet_.reset();
                nav.damage(area, Waveform::Ui);
                break;
            case ItemSheet::Action::Open: {
                const ItemRow row = sheet_->row();
                sheet_.reset();
                nav.damage(area, Waveform::Ui);
                if (actions_.openItem) actions_.openItem(row);
                break;
            }
            case ItemSheet::Action::Changed:
                reload(nav.library());
                nav.damage(area, Waveform::Ui);
                break;
            case ItemSheet::Action::More: {
                const ItemRow row = sheet_->row();
                sheet_.reset();
                nav.damage(area, Waveform::Ui);
                nav.push(std::make_unique<ItemDetailsScreen>(row, actions_.openItem));
                break;
            }
            case ItemSheet::Action::None: break;
        }
        return;
    }
    PluginBody* b = body();
    const bool plugin = showsPluginBody();
    if (b && (b->sheet() || b->errorPopup())) {  // they take every gesture until closed
        b->onGesture(g, nav);
        return;
    }
    if (plugin && g.type != GestureType::Tap) {  // long-press on an item: the item sheet; swipes turn its pages
        if (bodyRect().contains(g.type == GestureType::Swipe ? g.start : g.pos)) b->onGesture(g, nav);
        return;
    }
    if (g.type == GestureType::LongPress) {
        std::optional<ItemRow> hit;
        if (card_ && total_ > 0 && cardRect().contains(g.pos)) hit = card_;
        auto rects = recentRowRects();
        for (size_t i = 0; i < rects.size() && !hit; ++i)
            if (rects[i].contains(g.pos)) hit = recent_[i];
        if (hit)
            if (auto d = nav.library().details(hit->key)) {
                sheet_.emplace(*hit, std::move(*d));
                nav.damage(sheetArea(), Waveform::Ui);
            }
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (safeModeBannerRect().contains(g.pos)) {
        if (actions_.openPlugins) actions_.openPlugins();
        return;
    }
    if (updateBannerRect().contains(g.pos)) {
        if (updateDismissRect().contains(g.pos)) {
            if (actions_.dismissUpdate) actions_.dismissUpdate();
            update_.reset();
            nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, Waveform::Quality);
        } else if (actions_.openUpdate) {
            actions_.openUpdate();
        }
        return;
    }
    if (closeRect().contains(g.pos)) {
        nav.quit();  // the launcher returns to the Kindle home
        return;
    }
    if (syncIndicatorRect().contains(g.pos)) {
        nav.sync().syncNow();
        return;
    }
    if (settingsRect().contains(g.pos)) {
        if (actions_.openSettings) actions_.openSettings();
        return;
    }
    if (pluginsIcon_ && pluginsRect().contains(g.pos)) {
        nav.push(std::make_unique<PluginLauncherScreen>());
        return;
    }
    if (searchFieldRect().contains(g.pos)) {
        if (actions_.openSearch) actions_.openSearch();
        return;
    }
    if (plugin) {
        if (bodyRect().contains(g.pos)) b->onGesture(g, nav);
        return;
    }
    if (card_ && total_ > 0 && cardRect().contains(g.pos)) {
        if (actions_.openItem) actions_.openItem(*card_);
        return;
    }
    auto rects = recentRowRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(g.pos) && actions_.openItem) actions_.openItem(recent_[i]);
}

HomeScreen::Header HomeScreen::header() const {
    return {status_.line(nowMs_), status_.syncing(), status_.hasError()};
}

HomeScreen::Content HomeScreen::content() const {
    Content c{card_, cardOpened_, cardOpenedMs_, total_, cardHighlights_, recent_, "", status_.hasError()};
    if (c.hasError) c.error = status_.error().message;
    return c;
}

std::vector<Rect> HomeScreen::downloadRects(const SyncEvent& e) const {
    std::vector<Rect> out;
    auto shows = [&](const ItemRow& r) {
        return (!e.attachmentKey.empty() && r.attachmentKey == e.attachmentKey) || (!e.itemKey.empty() && r.key == e.itemKey);
    };
    if (card_ && total_ > 0 && shows(*card_)) out.push_back(cardRect());
    auto rects = recentRowRects();
    for (size_t i = 0; i < rects.size(); ++i)
        if (shows(recent_[i])) out.push_back(rects[i]);
    return out;
}

void HomeScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    const Header shownHeader = header();  // as drawn (with the time it was drawn at)
    const Content shownContent = content();
    nowMs_ = nav.nowMs();
    status_.onEvent(e, nowMs_);
    const bool headerChanged = header() != shownHeader;
    PluginBody* b = body();
    if (showsPluginBody()) return pluginSyncEvent(e, *b, headerChanged, shownContent, nav);
    switch (e.kind) {
        case SyncEvent::Kind::SyncStarted:
        case SyncEvent::Kind::SyncProgress:
            if (headerChanged) nav.damage({0, 0, W(), kHeaderH}, Waveform::Ui);
            if (total_ == 0) {
                reload(nav.library());  // the first items may have arrived
                nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, total_ ? Waveform::Quality : Waveform::Ui);
            }
            break;
        case SyncEvent::Kind::SyncFinished:
            // Only what changed: the status line, and the card, rows and banner if the sync
            // brought something (or failed differently). Never the whole screen. The status line
            // rests here, so it gets the quality waveform: the quick one left ghosts of
            // "Syncing…" behind the final text.
            if (headerChanged) nav.damage({0, 0, W(), kHeaderH}, Waveform::Quality);
            reload(nav.library());
            if (content() != shownContent) nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, Waveform::Quality);
            break;
        case SyncEvent::Kind::DownloadProgress:
            for (const Rect& r : downloadRects(e)) nav.damage(r, Waveform::Ui);
            break;
        case SyncEvent::Kind::DownloadFinished:
            // Kept collections download in the background, one file after another: repaint only
            // when one of them is on this screen. A failed one changes no item, only its status
            // (the error now shown on its card or row).
            reload(nav.library());
            if (content() != shownContent) nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, Waveform::Quality);
            else
                for (const Rect& r : downloadRects(e)) nav.damage(r, Waveform::Quality);
            break;
        case SyncEvent::Kind::AnnotationsReady:
            reload(nav.library());
            nav.damage({0, kCardY, W(), H() - kCardY - kTabBarH}, Waveform::Quality);
            break;
        case SyncEvent::Kind::PushStarted:  // "Uploading 3 changes…", then the header as before
            nav.damage({0, 0, W(), kHeaderH}, Waveform::Ui);
            break;
        case SyncEvent::Kind::PushFinished:
            nav.damage({0, 0, W(), kHeaderH}, Waveform::Quality);
            break;
        default: break;
    }
}

// The same as the built-in body's, for a plugin's: the blocks that draw an item repaint when the
// item changed (PluginBody::reread compares the rows they draw), nothing else; a sync that
// changed the library renders the body again (its tree compared with the last).
void HomeScreen::pluginSyncEvent(const SyncEvent& e, PluginBody& b, bool headerChanged, const Content& shown, Nav& nav) {
    const auto bannerChanged = [&] {
        const Content now = content();
        return now.hasError != shown.hasError || now.error != shown.error;
    };
    switch (e.kind) {
        case SyncEvent::Kind::SyncStarted:
        case SyncEvent::Kind::SyncProgress:
            if (headerChanged) nav.damage({0, 0, W(), kHeaderH}, Waveform::Ui);
            if (total_ == 0) reload(nav.library());  // the built-in body stays ready
            break;
        case SyncEvent::Kind::SyncFinished: {
            if (headerChanged) nav.damage({0, 0, W(), kHeaderH}, Waveform::Quality);
            reload(nav.library());
            if (bannerChanged()) nav.damage(bodyRegion(), Waveform::Quality);  // the body ends at the banner
            b.reread(nav);
            const SyncReport& r = e.report;
            if (r.itemsAdded || r.itemsUpdated || r.itemsRemoved || r.annotations || !r.pluginEvents.empty()) link_->synced();
            break;
        }
        case SyncEvent::Kind::DownloadProgress:
            for (const Rect& r : b.itemRects(e.itemKey, e.attachmentKey)) nav.damage(r, Waveform::Ui);
            break;
        case SyncEvent::Kind::DownloadFinished:
            reload(nav.library());
            for (const Rect& r : b.itemRects(e.itemKey, e.attachmentKey)) nav.damage(r, Waveform::Quality);  // its download line
            b.reread(nav);
            break;
        case SyncEvent::Kind::AnnotationsReady:
            reload(nav.library());
            b.reread(nav);
            break;
        case SyncEvent::Kind::PushStarted: nav.damage({0, 0, W(), kHeaderH}, Waveform::Ui); break;
        case SyncEvent::Kind::PushFinished: nav.damage({0, 0, W(), kHeaderH}, Waveform::Quality); break;
        default: break;
    }
}

}  // namespace zk
