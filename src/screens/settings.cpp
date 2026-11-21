#include "screens/settings.h"

#include <algorithm>
#include <filesystem>

#include "app/catalog_service.h"
#include "app/settings_store.h"
#include "screens/group_libraries.h"
#include "screens/plugin_entries.h"
#include "screens/read_tag.h"
#include "screens/settings_home.h"
#include "net/errors.h"
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
const int kIntervals[] = {0, 5, 10, 30};
const char* kIntervalLabels[] = {"Off", "5 min", "10 min", "30 min"};

// Section rows, top to bottom; `rule`: the line above a section, this far above it. A window
// shorter than the PW5's (the KT4's, 1337 at 300 ppi) takes the compact spacing. With the Home
// row (some plugin has a home screen, or one was chosen), the rows above it close up to make room
// on the PW5 and the KT4; the Scribe has room below.
struct Rows {
    int account, webDav, readTag, interval, highlights, storage, flash, plugins, home, about, rule;
};
constexpr Rows kRows{170, 320, 470, 620, 790, 1000, 1130, 1260, 0, 1380, 30};
constexpr Rows kCompactRows{150, 290, 410, 540, 710, 870, 985, 1100, 0, 1185, 20};
constexpr Rows kTallHomeRows{170, 320, 470, 620, 790, 1000, 1130, 1260, 1380, 1500, 30};
constexpr Rows kHomeRows{160, 295, 430, 565, 735, 930, 1055, 1175, 1285, 1395, 25};
constexpr Rows kCompactHomeRows{145, 265, 385, 505, 665, 820, 930, 1045, 1115, 1185, 20};
const Rows& rows(bool home) {
    if (!home) return H() < 1500 ? kCompactRows : kRows;
    return H() < 1500 ? kCompactHomeRows : H() < 2000 ? kHomeRows : kTallHomeRows;
}

std::string mb(int64_t bytes) {
    if (bytes < 1024 * 1024) return std::to_string((bytes + 1023) / 1024) + " KB";
    return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
}

}  // namespace

SettingsScreen::SettingsScreen(Config c, std::function<void(Nav&)> changeAccount, Updater* updater,
                               std::function<void(Nav&)> openUpdate,
                               std::function<std::optional<std::string>()> pendingUpdate, PluginHostFn plugins, PluginsOff pluginsOff)
    : config_(std::move(c)), changeAccount_(std::move(changeAccount)), updater_(updater),
      openUpdate_(std::move(openUpdate)), pendingUpdate_(std::move(pendingUpdate)), plugins_(std::move(plugins)),
      pluginsOff_(std::move(pluginsOff)) {}

// The band between the rules above and below (no buttons in it: the whole row opens the list).
Rect SettingsScreen::pluginsRect() const {
    const Rows& r = rows(homeRow_);
    return {kMargin, r.plugins - r.rule, W() - 2 * kMargin, (homeRow_ ? r.home : r.about) - r.plugins};
}

Rect SettingsScreen::homeRect() const {
    if (!homeRow_) return {};
    const Rows& r = rows(homeRow_);
    return {kMargin, r.home - r.rule, W() - 2 * kMargin, r.about - r.home};
}

std::string SettingsScreen::pluginsLine() const { return pluginsSummary(plugins_ ? plugins_() : nullptr); }

Rect SettingsScreen::groupsRect() const {
    return {kMargin, rows(homeRow_).webDav - rows(homeRow_).rule, W() - 2 * kMargin, rows(homeRow_).readTag - rows(homeRow_).webDav};
}

std::string SettingsScreen::accountLine() const {
    const std::string user = "User ID " + std::to_string(config_.userId);
    if (!groupsRow_) return user;  // WebDAV has its own row
    return user + " · " + (config_.webdavUrl.empty() ? "files from Zotero storage" : "WebDAV: " + hostOf(config_.webdavUrl));
}

Rect SettingsScreen::accountRect() const { return {W() - kMargin - 240, rows(homeRow_).account + 20, 240, 80}; }
Rect SettingsScreen::readTagRect() const { return {W() - kMargin - 240, rows(homeRow_).readTag + 20, 240, 80}; }
Rect SettingsScreen::intervalRect(int i) const {
    const int w = std::min(259, (W() - 2 * kMargin - 3 * 20) / 4);  // four in a row on a narrow window
    return {kMargin + i * (w + 20), rows(homeRow_).interval + 60, w, 80};
}
Rect SettingsScreen::aboutTextRect() const { return {kMargin, rows(homeRow_).about + 54, W() - 2 * kMargin - 460, 96}; }
Rect SettingsScreen::toggleRect() const { return {W() - kMargin - 140, rows(homeRow_).highlights, 140, 80}; }
Rect SettingsScreen::flashMinusRect() const { return {W() - kMargin - 460, rows(homeRow_).flash + 10, 120, 80}; }
Rect SettingsScreen::flashPlusRect() const { return {W() - kMargin - 120, rows(homeRow_).flash + 10, 120, 80}; }
Rect SettingsScreen::updateRect() const { return {W() - kMargin - 420, rows(homeRow_).about + 20, 420, 80}; }

std::string SettingsScreen::updateLine() const {
    if (!updateResult_.empty()) return updateResult_;
    if (pendingUpdate_)
        if (const auto v = pendingUpdate_()) return "Update available: " + *v;
    return "Version " + (updater_ ? updater_->current() : appVersion()).str() + " · open source (AGPL-3.0)";
}

std::string SettingsScreen::storageLine() const {
    if (confirmRemove_ && keptCount_ > 0)
        return "Removes " + mb(downloadedBytes_) + " and stops keeping " + std::to_string(keptCount_) +
               (keptCount_ == 1 ? " collection" : " collections") + " downloaded";
    return "Downloaded files: " + mb(downloadedBytes_);
}

TextStyle SettingsScreen::storageStyle(bool confirming) {
    return confirming ? TextStyle{kSans, 7.5, 500} : TextStyle{kSans, 8.5, 500};
}

Rect SettingsScreen::storageTextRect() const {
    const int w = W() - 2 * kMargin - 460;
    return confirmRemove_ && keptCount_ > 0 ? Rect{kMargin, rows(homeRow_).storage + 2, w, 100} : Rect{kMargin, rows(homeRow_).storage + 24, w, 50};
}

Rect SettingsScreen::removeRect() const { return {W() - kMargin - 420, rows(homeRow_).storage + 10, 420, 80}; }

int SettingsScreen::keptCountFor(Library& lib) {
    int n = 0;
    for (const auto& key : lib.keptCollections()) n += lib.libraryShown(lib.libraryOf(key).value_or(kMyLibrary));
    return n;
}

void SettingsScreen::load(Nav& nav) {
    Library& lib = nav.library();
    const AppSettings s = loadSettings(lib, nav.device());
    interval_ = s.syncIntervalMinutes;
    flashEvery_ = s.flashEvery;
    allOffline_ = lib.allAnnotationsOffline();
    readTag_ = lib.readTag();
    keptCount_ = keptCountFor(lib);
    const auto libs = lib.libraries(true);
    const GroupAccess access = lib.keyGroupAccess();
    groupsRow_ = libs.size() > 1 || access == GroupAccess::None;  // None is only ever read when groups are listed
    groupsLine_ = groupLibrariesSummary(libs, access);
    downloadedBytes_ = 0;
    for (const auto& a : lib.downloadedAttachments()) {
        std::error_code ec;
        const auto n = std::filesystem::file_size(a.localPath, ec);
        if (!ec) downloadedBytes_ += static_cast<int64_t>(n);
    }
}

void SettingsScreen::loadHome(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    homeVersion_ = e ? std::optional<uint64_t>(e->version()) : std::nullopt;
    const HomeChoices c = homeChoices(nav, plugins_ ? plugins_() : nullptr);
    homeRow_ = e && (c.options.size() > 1 || !c.stored.empty());
    homeLine_ = c.line();
}

void SettingsScreen::onShow(Nav& nav) {
    load(nav);
    loadHome(nav);
    pluginsDot_ = nav.catalog() && nav.catalog()->updatesWaiting() > 0;
    confirmRemove_ = false;
    // A check already running (the Home banner's): its answer opens the update screen as ours would.
    if (updater_ && updater_->status().phase == Updater::Status::Phase::Checking) checking_ = true;
}

void SettingsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Settings", true);
    const TextStyle label{kSans, 8.5, 500}, value{kSans, 7.5, 400, false, kSecondary};
    const int w = W() - 2 * kMargin;

    drawText(cr, "Accounts", label, {kMargin, rows(homeRow_).account, w, 50}, 1);
    drawText(cr, accountLine(), value, {kMargin, rows(homeRow_).account + 54, w - 260, 50}, 1);
    drawButton(cr, accountRect(), "Manage…", false);
    hRule(cr, rows(homeRow_).webDav - rows(homeRow_).rule, kMargin, W() - kMargin);

    if (groupsRow_) {  // in WebDAV's place (the account row names it): the whole row opens the list
        drawText(cr, "Group libraries", label, {kMargin, rows(homeRow_).webDav, w - 50, 50}, 1);
        drawText(cr, groupsLine_, value, {kMargin, rows(homeRow_).webDav + 54, w - 50, 50}, 1);
        const Rect band = groupsRect();
        centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {band.x + band.w - 30, band.y, 30, band.h}, Align::Right);
    } else {
        drawText(cr, "WebDAV", label, {kMargin, rows(homeRow_).webDav, w, 50}, 1);
        drawText(cr, config_.webdavUrl.empty() ? "Not used: files come from Zotero storage" : hostOf(config_.webdavUrl), value,
                 {kMargin, rows(homeRow_).webDav + 54, w, 50}, 1);
    }
    hRule(cr, rows(homeRow_).readTag - rows(homeRow_).rule, kMargin, W() - kMargin);

    drawText(cr, "Read tag", label, {kMargin, rows(homeRow_).readTag, w - 260, 50}, 1);
    drawText(cr, "Items tagged “" + readTag_ + "” in Zotero show as read", value,
             {kMargin, rows(homeRow_).readTag + 54, w - 260, 50}, 1);
    drawButton(cr, readTagRect(), "Change…", false);
    hRule(cr, rows(homeRow_).interval - rows(homeRow_).rule, kMargin, W() - kMargin);

    drawText(cr, "Sync every", label, {kMargin, rows(homeRow_).interval, w, 50}, 1);
    int active = 2;
    for (int i = 0; i < 4; ++i)
        if (kIntervals[i] == interval_) active = i;
    for (int i = 0; i < 4; ++i) {
        const Rect r = intervalRect(i);
        drawButton(cr, r, kIntervalLabels[i], i == active);
    }
    hRule(cr, rows(homeRow_).highlights - rows(homeRow_).rule, kMargin, W() - kMargin);

    drawText(cr, "Keep all highlights offline", label, {kMargin, rows(homeRow_).highlights + 4, w - 180, 50}, 1);
    drawText(cr,
             allOffline_ ? "Every highlight is downloaded and searchable offline."
                         : "Off: a book's highlights download when you open it. On: all of them (the first "
                           "time can take 15 minutes).",
             value, {kMargin, rows(homeRow_).highlights + 60, w - 180, 120}, 3);
    drawToggle(cr, toggleRect(), allOffline_);
    hRule(cr, rows(homeRow_).storage - rows(homeRow_).rule, kMargin, W() - kMargin);

    const bool confirmingKept = confirmRemove_ && keptCount_ > 0;
    drawText(cr, storageLine(), storageStyle(confirmingKept), storageTextRect(), confirmingKept ? 2 : 1);
    drawButton(cr, removeRect(), confirmRemove_ ? "Tap again to remove" : "Remove all", false);
    hRule(cr, rows(homeRow_).flash - rows(homeRow_).rule, kMargin, W() - kMargin);

    drawText(cr, "Full refresh every", label, {kMargin, rows(homeRow_).flash + 24, w - 420, 50}, 1);
    drawButton(cr, flashMinusRect(), "−", false);
    drawButton(cr, flashPlusRect(), "+", false);
    centredText(cr, std::to_string(flashEvery_) + (flashEvery_ == 1 ? " page" : " pages"), {kSans, 8, 400},
                {flashMinusRect().x + 120, rows(homeRow_).flash + 10, flashPlusRect().x - flashMinusRect().x - 120, 80});
    hRule(cr, rows(homeRow_).plugins - rows(homeRow_).rule, kMargin, W() - kMargin);

    const Rect band = pluginsRect();
    const bool canOpen = plugins_ && plugins_();
    centredText(cr, "Plugins", label, {band.x, band.y, 300, band.h}, Align::Left);
    if (pluginsDot_) {  // plugin updates are waiting (as on Home's puzzle icon)
        const int labelW = measureText("Plugins", label, 300, 1).width;
        gray(cr, kInk);
        cairo_arc(cr, band.x + labelW + 26, band.y + band.h / 2.0 - 14, 10, 0, 2 * 3.14159265);
        cairo_fill(cr);
    }
    centredText(cr, pluginsLine(), value, {band.x + 300, band.y, band.w - 300 - (canOpen ? 50 : 0), band.h}, Align::Right);
    if (canOpen) centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {band.x + band.w - 30, band.y, 30, band.h}, Align::Right);
    if (homeRow_) {  // the whole row opens the choice
        hRule(cr, rows(homeRow_).home - rows(homeRow_).rule, kMargin, W() - kMargin);
        const Rect home = homeRect();
        centredText(cr, "Home", label, {home.x, home.y, 300, home.h}, Align::Left);
        centredText(cr, homeLine_, value, {home.x + 300, home.y, home.w - 350, home.h}, Align::Right);
        centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {home.x + home.w - 30, home.y, 30, home.h}, Align::Right);
    }
    hRule(cr, rows(homeRow_).about - rows(homeRow_).rule, kMargin, W() - kMargin);

    drawText(cr, "Zotero for Kindle", label, {kMargin, rows(homeRow_).about, w - 460, 50}, 1);
    drawText(cr, updateLine(), value, aboutTextRect(), 2);
    drawButton(cr, updateRect(), checking_ ? "Checking…" : "Check for updates", false);
}

void SettingsScreen::onTick(Nav& nav) {
    if (PluginEntries* e = nav.pluginEntries(); e && homeVersion_ != e->version()) {  // plugins loaded or went
        const bool row = homeRow_;
        const std::string line = homeLine_;
        loadHome(nav);
        if (row != homeRow_) nav.redrawAll(Waveform::Quality);  // the rows move
        else if (line != homeLine_) nav.damage(homeRect(), Waveform::Ui);
    }
    if (const bool dot = nav.catalog() && nav.catalog()->updatesWaiting() > 0; dot != pluginsDot_) {
        pluginsDot_ = dot;
        nav.damage(pluginsRect(), Waveform::Ui);
    }
    if (!checking_ || !updater_) return;
    using Phase = Updater::Status::Phase;
    const Updater::Status st = updater_->status();
    if (st.phase == Phase::Checking) return;
    checking_ = false;
    if (st.phase == Phase::UpToDate) {
        updateResult_ = "You have the latest version.";
    } else if (st.phase == Phase::Failed && !st.mustRestart) {
        updateResult_ = st.error.message;
    } else if (st.phase != Phase::Idle) {  // a newer release, or an update already under way
        if (openUpdate_) openUpdate_(nav);
    }
    nav.damage({0, rows(homeRow_).about - 10, W(), 200}, Waveform::Ui);
}

void SettingsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (updateRect().contains(g.pos)) {
        if (!updater_ || checking_) return;
        updateResult_.clear();
        updater_->reset();
        updater_->check();
        checking_ = true;
        nav.damage({0, rows(homeRow_).about - 10, W(), 200}, Waveform::Ui);
        return;
    }
    if (accountRect().contains(g.pos)) {
        if (changeAccount_) changeAccount_(nav);
        return;
    }
    if (readTagRect().contains(g.pos)) {
        nav.push(std::make_unique<ReadTagScreen>());
        return;
    }
    if (groupsRow_ && groupsRect().contains(g.pos)) {
        nav.push(std::make_unique<GroupLibrariesScreen>());
        return;
    }
    if (homeRow_ && homeRect().contains(g.pos)) {
        nav.push(std::make_unique<HomeSettingScreen>(plugins_));
        return;
    }
    if (pluginsRect().contains(g.pos)) {
        if (plugins_ && plugins_()) nav.push(std::make_unique<PluginsScreen>(plugins_, pluginsOff_));
        return;
    }
    for (int i = 0; i < 4; ++i)
        if (intervalRect(i).contains(g.pos)) {
            AppSettings s = loadSettings(nav.library(), nav.device());
            s.syncIntervalMinutes = kIntervals[i];
            saveSettings(nav.library(), nav.device(), s);
            interval_ = kIntervals[i];
            nav.sync().setIntervalMinutes(interval_);
            nav.damage({0, rows(homeRow_).interval, W(), 160}, Waveform::Ui);
            return;
        }
    if (flashMinusRect().contains(g.pos) || flashPlusRect().contains(g.pos)) {
        AppSettings st = loadSettings(nav.library(), nav.device());
        st.flashEvery = std::clamp(st.flashEvery + (flashPlusRect().contains(g.pos) ? 1 : -1), 1, 12);
        saveSettings(nav.library(), nav.device(), st);
        flashEvery_ = st.flashEvery;
        nav.damage({0, rows(homeRow_).flash, W(), 120}, Waveform::Ui);
        return;
    }
    if (toggleRect().contains(g.pos)) {
        allOffline_ = !allOffline_;
        nav.sync().setAllHighlightsOffline(allOffline_);
        nav.damage({0, rows(homeRow_).highlights, W(), 200}, Waveform::Ui);
        return;
    }
    if (removeRect().contains(g.pos)) {
        if (!confirmRemove_) {
            confirmRemove_ = true;
        } else {
            confirmRemove_ = false;
            // Kept collections would download it all again: they stop being kept (as the line said).
            for (const auto& key : nav.library().keptCollections()) nav.library().setCollectionKept(key, false);
            keptCount_ = 0;
            nav.sync().removeDownloads();
        }
        nav.damage({0, rows(homeRow_).storage, W(), 200}, Waveform::Ui);
        return;
    }
    if (confirmRemove_) {
        confirmRemove_ = false;
        nav.damage({0, rows(homeRow_).storage, W(), 200}, Waveform::Ui);
    }
}

void SettingsScreen::onSyncEvent(const SyncEvent& e, Nav& nav) {
    if (e.kind != SyncEvent::Kind::SyncFinished && e.kind != SyncEvent::Kind::DownloadFinished) return;
    const bool groups = groupsRow_;
    const std::string line = groupsLine_;
    load(nav);
    nav.damage({0, rows(homeRow_).storage, W(), 200}, Waveform::Quality);
    if (groups != groupsRow_) nav.damage({0, rows(homeRow_).account, W(), rows(homeRow_).readTag - rows(homeRow_).account}, Waveform::Quality);
    else if (line != groupsLine_) nav.damage(groupsRect(), Waveform::Ui);
}

}  // namespace zk
