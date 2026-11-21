#include "screens/account_error.h"

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

AccountErrorScreen::AccountErrorScreen(std::string title, std::string reason, std::vector<Choice> choices)
    : title_(std::move(title)), reason_(std::move(reason)), choices_(std::move(choices)), restartNote_(choices_.empty()) {
    if (choices_.empty()) choices_.push_back({"Close Zotero", [](Nav& nav) { nav.quit(); }});
}

Rect AccountErrorScreen::choiceRect(size_t i) {
    return {kMargin, kHeaderH + 520 + static_cast<int>(i) * 130, metrics().width - 2 * kMargin, 100};
}

void AccountErrorScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    const int w = metrics().width;
    fillRect(cr, {0, 0, w, metrics().height}, 255);
    drawHeader(cr, title_, false);
    drawText(cr, reason_, {kSans, 8, 400, false, kInk}, {kMargin, kHeaderH + 40, w - 2 * kMargin, 300}, 6);
    drawText(cr, restartNote_ ? "Nothing was changed. Zotero tries again the next time it starts." : "Nothing was changed.",
             {kSans, 7.5, 400, false, kSecondary}, {kMargin, kHeaderH + 360, w - 2 * kMargin, 120}, 3);
    for (size_t i = 0; i < choices_.size(); ++i) drawButton(cr, choiceRect(i), choices_[i].label, i == 0);
}

void AccountErrorScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    for (size_t i = 0; i < choices_.size(); ++i)
        if (choiceRect(i).contains(g.pos)) {
            const auto act = choices_[i].act;  // it may replace this screen
            if (act) act(nav);
            return;
        }
}

}  // namespace zk
