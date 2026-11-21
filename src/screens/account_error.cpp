#include "screens/account_error.h"

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

AccountErrorScreen::AccountErrorScreen(std::string title, std::string reason)
    : title_(std::move(title)), reason_(std::move(reason)) {}

Rect AccountErrorScreen::closeRect() { return {kMargin, kHeaderH + 520, metrics().width - 2 * kMargin, 100}; }

void AccountErrorScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    const int w = metrics().width;
    fillRect(cr, {0, 0, w, metrics().height}, 255);
    drawHeader(cr, title_, false);
    drawText(cr, reason_, {kSans, 8, 400, false, kInk}, {kMargin, kHeaderH + 40, w - 2 * kMargin, 300}, 6);
    drawText(cr, "Nothing was changed. Zotero tries again the next time it starts.", {kSans, 7.5, 400, false, kSecondary},
             {kMargin, kHeaderH + 360, w - 2 * kMargin, 120}, 3);
    drawButton(cr, closeRect(), "Close Zotero", true);
}

void AccountErrorScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type == GestureType::Tap && closeRect().contains(g.pos)) nav.quit();
}

}  // namespace zk
