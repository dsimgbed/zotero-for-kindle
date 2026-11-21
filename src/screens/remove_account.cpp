#include "screens/remove_account.h"

#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
constexpr int kTextY = kHeaderH + 20, kUnsentY = kHeaderH + 330, kSendY = kHeaderH + 470, kRemoveY = kHeaderH + 640;

}  // namespace

RemoveAccountScreen::RemoveAccountScreen(AccountsHost& host, int64_t userId) : host_(host), userId_(userId) {
    account_.userId = userId;
}

void RemoveAccountScreen::load() {
    for (auto& a : host_.accounts(userId_))
        if (a.userId == userId_) account_ = a;
    online_ = host_.online();
}

void RemoveAccountScreen::onShow(Nav&) { load(); }

std::string RemoveAccountScreen::removeLabel() const {
    return "Remove " + accountName(account_) + " and delete " + sizeText(std::max<int64_t>(account_.bytes, 0));
}

std::string RemoveAccountScreen::unsentLine() const {
    if (account_.locked || account_.unsent <= 0) return {};
    const std::string n = std::to_string(account_.unsent) + (account_.unsent == 1 ? " change" : " changes");
    std::string line = n + " made on this Kindle " + (account_.unsent == 1 ? "hasn't" : "haven't") +
                       " been sent to Zotero; removing loses " + (account_.unsent == 1 ? "it." : "them.");
    if (sendFailed_) line += " Sending didn't get them all through.";
    else if (!account_.active) line += " Switch to " + accountName(account_) + " to send them first.";
    else if (!online_) line += " Connect to Wi-Fi to send them first.";
    return line;
}

Rect RemoveAccountScreen::sendRect() const {
    if (account_.locked || account_.unsent <= 0 || !account_.active || !online_) return {};
    return {kMargin, kSendY, W() - 2 * kMargin, 100};
}

Rect RemoveAccountScreen::removeRect() const { return {kMargin, kRemoveY, W() - 2 * kMargin, 100}; }

void RemoveAccountScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), metrics().height}, 255);
    const std::string name = accountName(account_);
    drawHeader(cr, "Remove " + name, true);
    const int w = W() - 2 * kMargin;
    const TextStyle body{kSans, 8, 400}, quiet{kSans, 7.5, 400, false, kSecondary}, strong{kSans, 8, 700};
    drawText(cr,
             "Removing " + name + " deletes its library, downloaded files and settings on this Kindle. " + name +
                 "'s Zotero account and library at zotero.org stay as they are.",
             body, {kMargin, kTextY, w, 180}, 4);
    if (account_.locked) {
        drawText(cr, name + " is locked. Its PIN isn't asked: everything of " + name + " on this Kindle is deleted, unsent changes too.",
                 strong, {kMargin, kTextY + 200, w, 260}, 5);
    } else {
        std::string line = accountLine(account_, host_.wallMs());
        if (account_.bytes >= 0) line += " · " + sizeText(account_.bytes) + " on this Kindle";
        drawText(cr, line, quiet, {kMargin, kTextY + 200, w, 60}, 1);
        if (const std::string u = unsentLine(); !u.empty()) drawText(cr, u, strong, {kMargin, kUnsentY, w, 130}, 3);
    }
    if (const Rect s = sendRect(); !s.empty()) drawButton(cr, s, sending() ? "Sending…" : "Send now", false);
    drawButton(cr, removeRect(), removeLabel(), true);
}

void RemoveAccountScreen::onTick(Nav& nav) {
    if (!request_) return;
    if (!host_.sent(request_)) {
        nav.wakeAt(nav.nowMs() + 500);
        return;
    }
    request_ = 0;
    load();
    sendFailed_ = account_.unsent > 0;
    nav.redrawAll(Waveform::Ui);
}

void RemoveAccountScreen::onGesture(const Gesture& g, Nav& nav) {
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) return nav.pop();
    if (const Rect s = sendRect(); !s.empty() && s.contains(g.pos)) {
        if (sending()) return;
        sendFailed_ = false;
        request_ = host_.sendNow();
        nav.damage(s, Waveform::Ui);
        return;
    }
    if (!removeRect().contains(g.pos) || sending()) return;
    if (account_.hasPin && !account_.locked) {  // the PIN first (its owner is here)
        const int64_t id = userId_;
        AccountsHost& host = host_;
        nav.push(host_.pinPad(id, PinPadScreen::Purpose::Verify, [&host, id](Nav& n) {
            n.pop();  // the pad
            host.remove(n, id);
        }));
        return;
    }
    host_.remove(nav, userId_);
}

}  // namespace zk
