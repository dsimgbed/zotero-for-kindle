#include "screens/accounts.h"

#include <cstdio>

#include "app/accounts.h"
#include "screens/remove_account.h"
#include "screens/sync_status.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
constexpr int kListY = kHeaderH + 20, kAccountRowH = 160;
constexpr int kSectionH = 170;  // a page's row: label, a line, its buttons on the right
const TextStyle kLabel{kSans, 8.5, 500}, kValue{kSans, 7.5, 400, false, kSecondary};

std::string ago(int64_t ms) {
    const int64_t min = ms / 60'000;
    if (min < 1) return "synced just now";
    if (min < 60) return "synced " + std::to_string(min) + " min ago";
    if (min < 48 * 60) return "synced " + std::to_string(min / 60) + " h ago";
    return "synced " + std::to_string(min / (24 * 60)) + " days ago";
}

}  // namespace

std::string accountName(const AccountSummary& a) { return a.name.empty() ? "Account " + std::to_string(a.userId) : a.name; }

std::string accountLine(const AccountSummary& a, int64_t nowWallMs) {
    if (a.locked) return {};
    std::string line = thousands(a.items) + (a.items == 1 ? " item" : " items");
    if (a.unsent > 0) return line + " · " + std::to_string(a.unsent) + (a.unsent == 1 ? " change not sent" : " changes not sent");
    if (a.syncedWallMs > 0) return line + " · " + ago(std::max<int64_t>(0, nowWallMs - a.syncedWallMs));
    return line;
}

std::string sizeText(int64_t bytes) {
    constexpr int64_t kMB = 1024 * 1024, kGB = 1024 * kMB;
    if (bytes < kMB) return std::to_string((std::max<int64_t>(bytes, 0) + 1023) / 1024) + " KB";
    if (bytes < kGB) return std::to_string((bytes + kMB / 2) / kMB) + " MB";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / kGB);
    return buf;
}

// ---- the list ----------------------------------------------------------------------------------

AccountsScreen::AccountsScreen(AccountsHost& host) : host_(host) {}

void AccountsScreen::onShow(Nav&) {
    accounts_ = host_.accounts();
    nowWall_ = host_.wallMs();
}

Rect AccountsScreen::rowRect(size_t i) const {
    return {kMargin, kListY + static_cast<int>(i) * kAccountRowH, W() - 2 * kMargin, kAccountRowH};
}

Rect AccountsScreen::addRect() const {
    if (accounts_.size() >= kMaxAccounts) return {};
    return {kMargin, rowRect(accounts_.size()).y + 50, W() - 2 * kMargin, 100};
}

void AccountsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), metrics().height}, 255);
    drawHeader(cr, "Accounts", true);
    for (size_t i = 0; i < accounts_.size(); ++i) {
        const AccountSummary& a = accounts_[i];
        const Rect r = rowRect(i);
        if (i > 0) hRule(cr, r.y, r.x, r.x + r.w);
        if (a.active) {
            gray(cr, kInk);
            cairo_new_path(cr);
            cairo_arc(cr, r.x + 14, r.y + 62, 11, 0, 2 * 3.14159265);
            cairo_fill(cr);
        }
        const std::string name = accountName(a);
        const TextStyle nameStyle{kSans, 9.5, 700};
        const int nameW = std::min(measureText(name, nameStyle, r.w - 200, 1).width, r.w - 200);
        drawText(cr, name, nameStyle, {r.x + 50, r.y + 30, r.w - 200, 60}, 1);
        if (a.locked) lockIcon(cr, r.x + 50 + nameW + 44, r.y + 66, kSecondary, 0.8);
        if (!a.locked)  // a locked one: its name and the lock only
            drawText(cr, (a.active ? "Open · " : "") + accountLine(a, nowWall_), kValue, {r.x + 50, r.y + 96, r.w - 150, 50}, 1);
        centredText(cr, "›", {kSans, 12, 400, false, kSecondary}, {r.x + r.w - 30, r.y, 30, r.h}, Align::Right);
    }
    if (const Rect add = addRect(); !add.empty()) {
        hRule(cr, rowRect(accounts_.size()).y, kMargin, W() - kMargin);
        drawButton(cr, add, "+ Add account", false);
    }
}

void AccountsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) return nav.pop();
    if (const Rect add = addRect(); !add.empty() && add.contains(g.pos)) return host_.addAccount(nav);
    for (size_t i = 0; i < accounts_.size(); ++i)
        if (rowRect(i).contains(g.pos)) return nav.push(std::make_unique<AccountPageScreen>(host_, accounts_[i].userId));
}

// ---- one account's page ------------------------------------------------------------------------

AccountPageScreen::AccountPageScreen(AccountsHost& host, int64_t userId) : host_(host), userId_(userId) {
    account_.userId = userId;
}

void AccountPageScreen::onShow(Nav& nav) {
    gone_ = true;
    for (auto& a : host_.accounts(userId_))
        if (a.userId == userId_) {
            account_ = a;
            gone_ = false;
        }
    if (gone_) nav.pop();
}

void AccountPageScreen::onTick(Nav& nav) {
    if (!account_.measuring || gone_) return;
    for (auto& a : host_.accounts(userId_))
        if (a.userId == userId_) account_ = a;
    if (account_.measuring) nav.wakeAt(nav.nowMs() + 500);
    else nav.redrawAll(Waveform::Ui);
}

// Top to bottom: the summary; Switch to (another account's); the PIN; Lock on sleep (with a PIN);
// the key; Remove from this Kindle. A locked account: the summary, Switch to, Remove.
Rect AccountPageScreen::switchRect() const {
    if (account_.active) return {};
    return {kMargin, kHeaderH + 150, W() - 2 * kMargin, 100};
}
int AccountPageScreen::pinY() const { return kHeaderH + (account_.active ? 170 : 310); }
int AccountPageScreen::lockY() const { return pinY() + kSectionH; }
int AccountPageScreen::keyY() const { return lockY() + (account_.hasPin ? kSectionH : 0); }
int AccountPageScreen::removeY() const { return account_.locked ? kHeaderH + 310 : keyY() + kSectionH + 30; }

Rect AccountPageScreen::setPinRect() const {
    if (account_.locked || account_.hasPin) return {};
    return {W() - kMargin - 260, pinY() + 30, 260, 90};
}
Rect AccountPageScreen::changePinRect() const {
    if (account_.locked || !account_.hasPin) return {};
    return {W() - kMargin - 500, pinY() + 30, 240, 90};
}
Rect AccountPageScreen::removePinRect() const {
    if (account_.locked || !account_.hasPin) return {};
    return {W() - kMargin - 240, pinY() + 30, 240, 90};
}
Rect AccountPageScreen::lockToggleRect() const {
    if (account_.locked || !account_.hasPin) return {};
    return {W() - kMargin - 140, lockY() + 30, 140, 90};
}
Rect AccountPageScreen::updateKeyRect() const {
    if (account_.locked) return {};
    return {W() - kMargin - 300, keyY() + 30, 300, 90};
}
Rect AccountPageScreen::removeRect() const { return {kMargin, removeY(), W() - 2 * kMargin, 100}; }

void AccountPageScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), metrics().height}, 255);
    const std::string name = accountName(account_);
    drawHeader(cr, name, true);
    const int w = W() - 2 * kMargin;
    if (account_.locked) {
        lockIcon(cr, kMargin + 24, kHeaderH + 54, kSecondary, 0.8);
        drawText(cr, "Locked. Switch to " + name + " and enter its PIN to see or change it.", kValue,
                 {kMargin + 70, kHeaderH + 20, w - 70, 100}, 2);
    } else {
        std::string line = accountLine(account_, host_.wallMs());
        if (account_.measuring) line += " · … on this Kindle";
        else if (account_.bytes >= 0) line += " · " + sizeText(account_.bytes) + " on this Kindle";
        drawText(cr, (account_.active ? "Open · " : "") + line, kValue, {kMargin, kHeaderH + 30, w, 100}, 2);
    }
    if (const Rect s = switchRect(); !s.empty()) drawButton(cr, s, "Switch to " + name, true);
    if (!account_.locked) {
        const int y = pinY();
        drawText(cr, "PIN", kLabel, {kMargin, y + 20, w - 520, 50}, 1);
        drawText(cr, account_.hasPin ? "On: asked when switching to " + name + " and at start" : "Off", kValue,
                 {kMargin, y + 76, w - 520, 80}, 2);
        if (const Rect r = setPinRect(); !r.empty()) drawButton(cr, r, "Set PIN…", false);
        if (const Rect r = changePinRect(); !r.empty()) drawButton(cr, r, "Change…", false);
        if (const Rect r = removePinRect(); !r.empty()) drawButton(cr, r, "Remove", false);
        if (account_.hasPin) {
            hRule(cr, lockY(), kMargin, W() - kMargin);
            drawText(cr, "Lock on sleep", kLabel, {kMargin, lockY() + 20, w - 180, 50}, 1);
            drawText(cr, "The PIN is asked when the Kindle wakes", kValue, {kMargin, lockY() + 76, w - 180, 80}, 2);
            drawToggle(cr, lockToggleRect(), account_.lockOnSleep);
        }
        hRule(cr, keyY(), kMargin, W() - kMargin);
        drawText(cr, "Zotero key", kLabel, {kMargin, keyY() + 20, w - 320, 50}, 1);
        drawText(cr, account_.keyLine, kValue, {kMargin, keyY() + 76, w - 320, 80}, 2);
        drawButton(cr, updateKeyRect(), "Update key…", false);
        hRule(cr, removeY() - 30, kMargin, W() - kMargin);
    }
    drawButton(cr, removeRect(), "Remove from this Kindle…", false);
}

void AccountPageScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) return nav.pop();
    auto hit = [&](Rect r) { return !r.empty() && r.contains(g.pos); };
    if (hit(switchRect())) return host_.switchTo(userId_);
    if (hit(removeRect())) return nav.push(std::make_unique<RemoveAccountScreen>(host_, userId_));
    if (account_.locked) return;
    auto pinPad = [&](PinPadScreen::Purpose p) {
        nav.push(host_.pinPad(userId_, p, [](Nav& n) { n.pop(); }));  // back here, shown again
    };
    if (hit(setPinRect())) return pinPad(PinPadScreen::Purpose::Set);
    if (hit(changePinRect())) return pinPad(PinPadScreen::Purpose::Change);
    if (hit(removePinRect())) return pinPad(PinPadScreen::Purpose::Remove);
    if (hit(lockToggleRect())) {
        account_.lockOnSleep = !account_.lockOnSleep;
        host_.setLockOnSleep(userId_, account_.lockOnSleep);
        nav.damage(lockToggleRect(), Waveform::Ui);
        return;
    }
    if (hit(updateKeyRect())) return host_.updateKey(nav, userId_);
}

}  // namespace zk
