#include "screens/switching.h"

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

SwitchingScreen::SwitchingScreen(std::function<View()> view, std::function<void()> skip, std::function<void(Nav&)> tick)
    : view_(std::move(view)), skip_(std::move(skip)), tick_(std::move(tick)) {}

Rect SwitchingScreen::skipRect() { return {kMargin, kHeaderH + 360, metrics().width - 2 * kMargin, 100}; }

void SwitchingScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    const int w = metrics().width;
    fillRect(cr, {0, 0, w, metrics().height}, 255);
    drawHeader(cr, "Accounts", false);
    drawText(cr, shown_.title, {kSans, 9, 600, false, kInk}, {kMargin, kHeaderH + 80, w - 2 * kMargin, 100}, 2);
    if (!shown_.progress.empty())
        drawText(cr, shown_.progress, {kSans, 7.5, 400, false, kSecondary}, {kMargin, kHeaderH + 200, w - 2 * kMargin, 120}, 3);
    if (shown_.canSkip) drawButton(cr, skipRect(), "Skip", false);
}

void SwitchingScreen::onGesture(const Gesture& g, Nav&) {
    if (g.type == GestureType::Tap && shown_.canSkip && skipRect().contains(g.pos) && skip_) skip_();
}

void SwitchingScreen::onShow(Nav&) {
    if (view_) shown_ = view_();
}

void SwitchingScreen::onTick(Nav& nav) {
    if (tick_) tick_(nav);
    if (!view_ || nav.top() != this) return;  // the switch is over: this screen is gone
    View now = view_();
    if (now == shown_) return;
    shown_ = std::move(now);
    nav.damage({0, 0, metrics().width, metrics().height}, Waveform::Ui);
}

}  // namespace zk
