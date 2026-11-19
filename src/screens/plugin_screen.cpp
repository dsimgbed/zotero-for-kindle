#include "screens/plugin_screen.h"

#include <algorithm>

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
const TextStyle kNameText{kSans, 7.5, 500, false, kSecondary};

// Under the header, inside the margins; above the keyboard while it shows.
Rect blocksAbove(Nav& nav) {
    Rect a = blocksArea();
    const Rect kb = nav.keyboard().bounds();
    if (!kb.empty() && kb.y < a.y + a.h) a.h = std::max(0, kb.y - 12 - a.y);
    return a;
}

}  // namespace

PluginScreen::PluginScreen(ScreenLink& link, uint64_t instance, std::string pluginName, std::string title, ItemSource* items,
                           ItemTap tap)
    : pluginName_(pluginName), title_(title),
      body_(link, instance, std::move(pluginName), std::move(title), blocksAbove, TreeUse::Screen, items, std::move(tap)) {
    body_.onPluginClose([](Nav& nav) { nav.pop(); });
}

Rect PluginScreen::workingRect() { return PluginBody::workingRect(blocksArea()); }

Rect PluginScreen::backButtonRect() { return {(W() - 400) / 2, kHeaderH + 360, 400, 110}; }

int PluginScreen::nameReserve() const {
    return std::min(measureText(pluginName_, kNameText, W() / 3, 1).width + 40, W() / 3);
}

void PluginScreen::onShow(Nav& nav) { body_.onShow(nav); }

void PluginScreen::onTick(Nav& nav) { body_.onTick(nav); }

void PluginScreen::onKey(const KeyEvent& k, Nav& nav) { body_.onKey(k, nav); }

void PluginScreen::onClose(Nav& nav) { body_.onClose(nav); }

void PluginScreen::leave(Nav& nav) {
    if (!body_.focus().focused.empty()) nav.keyboard().hide();
    nav.pop();
}

void PluginScreen::onGesture(const Gesture& g, Nav& nav) {
    if (body_.sheet()) {  // the item sheet takes every gesture until it is closed
        body_.onGesture(g, nav);
        return;
    }
    if (g.type == GestureType::Tap && headerBackRect().contains(g.pos)) {
        leave(nav);
        return;
    }
    if (body_.errorPopup()) {  // so does the error pop-up, but for Back
        body_.onGesture(g, nav);
        return;
    }
    if (state() == State::Failed) {
        if (g.type == GestureType::Tap && backButtonRect().contains(g.pos)) leave(nav);
        return;
    }
    body_.onGesture(g, nav);
}

void PluginScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    const int reserve = nameReserve();
    drawHeader(cr, title_, true, reserve);
    centredText(cr, pluginName_, kNameText, {W() - kMargin - reserve + 40, 30, reserve - 40, 92}, Align::Right);
    switch (state()) {
        case State::Loading:
            centredText(cr, "Loading…", {kSans, 9, 400, false, kSecondary}, {kMargin, kHeaderH + 200, W() - 2 * kMargin, 100});
            break;
        case State::Failed:
            drawText(cr, errorText(), {kSerif, 11, 400, false, kSecondary}, {kMargin, kHeaderH + 160, W() - 2 * kMargin, 180}, 3,
                     Align::Center);
            drawButton(cr, backButtonRect(), "Back", false);
            break;
        case State::Shown: break;
    }
    body_.draw(cr);  // the blocks (Shown), and the error pop-up over all
}

}  // namespace zk
