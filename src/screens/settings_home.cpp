#include "screens/settings_home.h"

#include <algorithm>

#include "app/settings_store.h"
#include "screens/plugin_entries.h"
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
constexpr int kOptionH = 130, kNoteH = 110;
const TextStyle kOption{kSans, 9, 500}, kNote{kSans, 7.5, 400, false, kSecondary};

}  // namespace

int HomeChoices::chosen() const {
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i].value == stored) return static_cast<int>(i);
    return -1;
}

std::string HomeChoices::line() const {
    const int i = chosen();
    return i >= 0 ? options[static_cast<size_t>(i)].label : unavailable;
}

HomeChoices homeChoices(Nav& nav, PluginHost* host) {
    HomeChoices c;
    c.options.push_back({"", "Built-in"});
    if (PluginEntries* e = nav.pluginEntries())
        for (const auto& s : e->homeScreens()) c.options.push_back({s.pluginId + "/" + s.screenId, s.title + " — " + s.pluginName});
    c.stored = loadSettings(nav.library(), nav.device()).homeScreen;
    if (c.chosen() < 0) {
        const std::string id = c.stored.substr(0, c.stored.find('/'));
        std::string name = id;
        if (host)
            for (const auto& s : host->status())
                if (s.folder.dir == id) name = pluginName(s);
        c.unavailable = name + " — not available";
    }
    return c;
}

HomeSettingScreen::HomeSettingScreen(PluginHostFn plugins) : plugins_(std::move(plugins)) {}

void HomeSettingScreen::load(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    version_ = e ? std::optional<uint64_t>(e->version()) : std::nullopt;
    choices_ = homeChoices(nav, plugins_ ? plugins_() : nullptr);
}

void HomeSettingScreen::onShow(Nav& nav) { load(nav); }

void HomeSettingScreen::onTick(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    if (!e || version_ == e->version()) return;
    load(nav);
    nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Ui);
}

std::vector<Rect> HomeSettingScreen::optionRects() const {
    const int top = kHeaderH + 20;
    const int fit = std::max(1, (H() - top - kNoteH - 40) / kOptionH);
    const int n = std::min(static_cast<int>(choices_.options.size()), fit);
    std::vector<Rect> out;
    for (int i = 0; i < n; ++i) out.push_back({kMargin, top + i * kOptionH, W() - 2 * kMargin, kOptionH});
    return out;
}

Rect HomeSettingScreen::noteRect() const {
    const auto rects = optionRects();
    return {kMargin, rects.back().y + rects.back().h + 20, W() - 2 * kMargin, kNoteH};
}

void HomeSettingScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "Home", true);
    const auto rects = optionRects();
    const int chosen = choices_.chosen();
    for (size_t i = 0; i < rects.size(); ++i) {
        const Rect& r = rects[i];
        if (i > 0) hRule(cr, r.y, r.x, r.x + r.w);
        centredText(cr, choices_.options[i].label, kOption, {r.x, r.y, r.w - 100, r.h}, Align::Left);
        if (static_cast<int>(i) == chosen) centredText(cr, "✓", {kSans, 10, 700}, {r.x + r.w - 80, r.y, 80, r.h}, Align::Right);
    }
    if (!choices_.unavailable.empty()) drawText(cr, choices_.unavailable, kNote, noteRect(), 2);
}

void HomeSettingScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    const auto rects = optionRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].contains(g.pos) || static_cast<int>(i) == choices_.chosen()) continue;
        AppSettings s = loadSettings(nav.library(), nav.device());
        s.homeScreen = choices_.options[i].value;
        saveSettings(nav.library(), nav.device(), s);
        load(nav);  // the stored choice is offered now: no note
        nav.damage({0, kHeaderH, W(), H() - kHeaderH}, Waveform::Ui);
        return;
    }
}

}  // namespace zk
