#include "screens/account_sheet.h"

#include "screens/library_sheets.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

constexpr int kPad = 40, kTitleH = 90, kSheetRowH = 120, kBottomPad = 30;

}  // namespace

AccountSheet::AccountSheet(std::vector<Row> rows, bool canAdd, bool canManage, std::string title)
    : rows_(std::move(rows)), canAdd_(canAdd), canManage_(canManage), title_(std::move(title)) {}

Rect AccountSheet::sheetRect(Rect area) const {
    const int n = static_cast<int>(rows_.size()) + canAdd_ + canManage_;
    return sheetRectIn(area, kPad + kTitleH + n * kSheetRowH + kBottomPad);
}

Rect AccountSheet::rowRect(Rect area, size_t i) const {
    const Rect s = sheetRect(area);
    return {s.x + kMargin, s.y + kPad + kTitleH + static_cast<int>(i) * kSheetRowH, s.w - 2 * kMargin, kSheetRowH};
}

Rect AccountSheet::addRect(Rect area) const { return canAdd_ ? rowRect(area, rows_.size()) : Rect{}; }
Rect AccountSheet::manageRect(Rect area) const { return canManage_ ? rowRect(area, rows_.size() + canAdd_) : Rect{}; }

void AccountSheet::draw(cairo_t* cr, Rect area) const {
    const Rect s = sheetRect(area);
    drawSheet(cr, area, s, title_);
    const TextStyle name{kSans, 9.5, 700}, line{kSans, 7.5, 400, false, kSecondary};
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Rect r = rowRect(area, i);
        const Row& row = rows_[i];
        if (i > 0) hRule(cr, r.y, r.x, r.x + r.w);
        if (row.active) {  // the open account
            gray(cr, kInk);
            cairo_arc(cr, r.x + 14, r.y + r.h / 2.0, 11, 0, 2 * 3.14159265);
            cairo_fill(cr);
        }
        const int nameX = r.x + 50;
        const int nameW = std::min(measureText(row.name, name, r.w / 2, 1).width, r.w / 2);
        centredText(cr, row.name, name, {nameX, r.y, r.w / 2, r.h}, Align::Left);
        if (row.locked) lockIcon(cr, nameX + nameW + 44, r.y + r.h / 2.0 + 4, kSecondary, 0.8);
        else if (!row.line.empty())
            centredText(cr, row.line, line, {nameX + nameW + 40, r.y, r.x + r.w - nameX - nameW - 40, r.h}, Align::Right);
    }
    const TextStyle action{kSans, 9, 500};
    if (canAdd_) {
        const Rect r = addRect(area);
        hRule(cr, r.y, r.x, r.x + r.w);
        centredText(cr, "+ Add account", action, {r.x + 50, r.y, r.w - 50, r.h}, Align::Left);
    }
    if (canManage_) {
        const Rect r = manageRect(area);
        hRule(cr, r.y, r.x, r.x + r.w);
        centredText(cr, "Manage accounts…", action, {r.x + 50, r.y, r.w - 50, r.h}, Align::Left);
    }
}

AccountSheet::Action AccountSheet::onGesture(const Gesture& g, Rect area) {
    if (g.type != GestureType::Tap) return Action::None;
    if (!sheetRect(area).contains(g.pos)) return Action::Close;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rowRect(area, i).contains(g.pos)) {
            chosen_ = rows_[i].userId;
            return Action::Switch;
        }
    if (canAdd_ && addRect(area).contains(g.pos)) return Action::Add;
    if (canManage_ && manageRect(area).contains(g.pos)) return Action::Manage;
    return Action::None;
}

}  // namespace zk
