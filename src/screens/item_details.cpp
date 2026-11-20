#include "screens/item_details.h"

#include "screens/item_sheet.h"
#include "plugins/identifiers.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cctype>
#include <functional>

namespace zk {

using namespace theme;

namespace {

int W() { return metrics().width; }
int H() { return metrics().height; }

std::string roleLabel(const std::string& type) {
    if (type.empty() || type == "author") return "";
    std::string t = itemTypeLabel(type);  // "seriesEditor" -> "Series editor"
    t[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
    return " (" + t + ")";
}

}  // namespace

ItemDetailsScreen::ItemDetailsScreen(ItemRow row, std::function<void(const ItemRow&)> open)
    : row_(std::move(row)), open_(std::move(open)) {}

Rect ItemDetailsScreen::openRect() { return {kMargin, H() - kMargin - 100, 360, 100}; }

std::vector<Rect> ItemDetailsScreen::removeRects() const {
    std::vector<Rect> out;
    for (const auto& [key, r] : removeTargets_) out.push_back(r);
    return out;
}

std::vector<Rect> ItemDetailsScreen::fileOpenRects() const {
    std::vector<Rect> out;
    for (const auto& [key, r] : openTargets_) out.push_back(r);
    return out;
}

std::vector<Rect> ItemDetailsScreen::actionRects() const { return actionRects_; }

void ItemDetailsScreen::onShow(Nav& nav) {
    if (auto d = nav.library().details(row_.key)) details_ = std::move(*d);
    library_ = details_.item.lib != kMyLibrary ? nav.library().libraryName(details_.item.lib) : "";
    if (auto r = nav.library().rowFor(row_.key)) row_ = std::move(*r);
    PluginEntries* e = nav.pluginEntries();
    entriesVersion_ = e ? e->version() : 0;
    if (!result_) actions_ = e ? e->actions("itemAction") : std::vector<PluginAction>{};
}

void ItemDetailsScreen::onTick(Nav& nav) {
    PluginEntries* e = nav.pluginEntries();
    if (e && !result_ && e->version() != entriesVersion_) {  // plugins loaded, turned on or off meanwhile
        entriesVersion_ = e->version();
        auto now = e->actions("itemAction");
        bool same = now.size() == actions_.size();
        for (size_t i = 0; same && i < now.size(); ++i) same = now[i].pluginId == actions_[i].pluginId && now[i].id == actions_[i].id;
        actions_ = std::move(now);
        if (!same) nav.damage({0, 0, W(), H()}, Waveform::Ui);
    }
    if (!result_) return;
    const bool working = result_->running() && !result_->shown();
    if (result_->tick(nav) == PluginResult::Status::Closed) result_.reset();
    const bool now = result_ && result_->running() && !result_->shown();
    if (working != now && running_ >= 0 && running_ < static_cast<int>(actionRects_.size()) &&
        !actionRects_[static_cast<size_t>(running_)].empty())
        nav.damage(actionRects_[static_cast<size_t>(running_)], Waveform::Ui);
    if (!result_) running_ = -1;
}

void ItemDetailsScreen::onClose(Nav&) {
    if (result_) result_->stop();
}

Rect ItemDetailsScreen::pageIndicatorRect() {
    const Rect o = openRect();
    return {o.x + o.w + 20, o.y, W() - kMargin - (o.x + o.w + 20), o.h};
}

void ItemDetailsScreen::draw(Canvas& canvas) {
    cairo_t* cr = canvas.cr();
    fillRect(cr, {0, 0, W(), H()}, 255);
    drawHeader(cr, "About this item", true);
    const TextStyle title{kSerif, 14, 700}, label{kSans, 7, 500, false, kSecondary}, value{kSans, 8, 400},
        body{kSerif, 8, 400};
    const int x = kMargin, w = W() - 2 * kMargin;
    // The page is laid out in pieces, each drawn at a y and telling its height. They go on pages
    // (a swipe turns) so that none runs into the Open button: a small Kindle (KT4) can't show an
    // item with many files or plugin actions on one. The abstract takes the rest of its page.
    using Piece = std::function<int(cairo_t*, int y)>;
    std::vector<Piece> pieces;
    const auto& item = details_.item;
    pieces.push_back([&](cairo_t* c, int y) {
        return drawText(c, item.title.empty() ? "Untitled" : item.title, title, {x, y, w, 220}, 3).height + 24;
    });
    auto section = [&](const std::string& name, const std::string& text, int maxLines) {
        if (text.empty()) return;
        pieces.push_back([&, name, text, maxLines](cairo_t* c, int y) {
            drawText(c, name, label, {x, y, w, 40}, 1);
            return 44 + drawText(c, text, value, {x, y + 44, w, maxLines * 52}, maxLines).height + 22;
        });
    };
    std::string people;
    for (const auto& c : item.creators) {
        const std::string name = !c.name.empty() ? c.name : c.first.empty() ? c.last : c.first + " " + c.last;
        people += (people.empty() ? "" : "\n") + name + roleLabel(c.type);
    }
    section(item.creators.size() == 1 ? "Author" : "People", people, 4);
    std::string kind = itemTypeLabel(item.itemType);
    if (!item.date.empty()) kind += " · " + item.date;
    section("Type", kind, 1);
    section("Published in", item.publication, 2);
    std::string collections;
    for (const auto& c : details_.collectionPaths) collections += (collections.empty() ? "" : "\n") + c;
    section("Collections", collections, 3);
    if (item.lib != kMyLibrary) section("Library", library_, 1);  // a group's item
    std::string tags;
    for (const auto& t : item.tags) tags += (tags.empty() ? "" : ", ") + t;
    section("Tags", tags, 2);

    // Files, each with its own Open and Remove from Kindle; the heading goes with the first.
    auto readable = [](const AttachmentRecord& a) {
        return isReadableType(a.contentType) && a.linkMode != "linked_file" && a.linkMode != "linked_url";
    };
    const auto readables = std::count_if(details_.files.begin(), details_.files.end(),
                                         [&](const ItemDetails::File& f) { return readable(f.attachment); });
    for (size_t i = 0; i < details_.files.size(); ++i)
        pieces.push_back([&, i](cairo_t* c, int y) {
            const int y0 = y;
            if (i == 0) {
                drawText(c, details_.files.size() == 1 ? "File" : "Files", label, {x, y, w, 40}, 1);
                y += 50;
            }
            const auto& f = details_.files[i];
            y += drawText(c, fileLine(f, f.progress), value, {x, y, w, 110}, 2).height + 14;
            const auto& a = f.attachment;
            int bx = x;
            if (readables > 1 && readable(a) && open_) {  // one file: the page's Open is the file's
                const Rect r{bx, y, 200, 84};
                drawButton(c, r, "Open", false);
                openTargets_.push_back({a.key, r});
                bx += 200 + 24;
            }
            if (f.onKindle) {
                const Rect r{bx, y, 440, 84};
                drawButton(c, r, confirming_ == f.attachment.key ? "Tap again to remove" : "Remove from Kindle", false);
                removeTargets_.push_back({f.attachment.key, r});
            }
            if (bx > x || f.onKindle) y += 84 + 14;
            y += 16;
            if (i + 1 == details_.files.size()) y += 12;
            return y - y0;
        });
    // The plugins' item actions: title, and the plugin's name (or "Working…") on the right.
    const TextStyle quiet{kSans, 7, 400, false, kSecondary};
    for (size_t i = 0; i < actions_.size(); ++i)
        pieces.push_back([&, i](cairo_t* c, int y) {
            const int y0 = y;
            if (i == 0) {
                drawText(c, "Plugins", label, {x, y, w, 40}, 1);
                y += 50;
            }
            const Rect r{x, y, w, 84};
            const bool working = static_cast<int>(i) == running_ && result_ && result_->running() && !result_->shown();
            const std::string side = working ? "Working…" : actions_[i].pluginName;
            const int sideW = std::min(measureText(side, quiet, w / 2, 1).width + 10, w / 2);
            centredText(c, actions_[i].title, value, {r.x, r.y, r.w - sideW - 30, r.h}, Align::Left);
            centredText(c, side, quiet, {r.x + r.w - sideW, r.y, sideW, r.h}, Align::Right);
            hRule(c, r.y + r.h - 2, r.x, r.x + r.w);
            if (i < actionRects_.size()) actionRects_[i] = r;
            return y + 84 + (i + 1 == actions_.size() ? 28 : 0) - y0;
        });

    // Measured on a scratch surface (the same fonts and layout), then laid out on pages.
    const int top = kHeaderH + 20, bottom = openRect().y - 40;
    std::vector<int> heights;
    {
        cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
        cairo_t* sc = cairo_create(scratch);
        for (const auto& p : pieces) heights.push_back(p(sc, 0));
        cairo_destroy(sc);
        cairo_surface_destroy(scratch);
    }
    std::vector<size_t> starts = {0};  // each page's first piece
    int y = top;
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (y > top && y + heights[i] > bottom) {
            starts.push_back(i);
            y = top;
        }
        y += heights[i];
    }
    // The abstract: its heading and some lines, else it starts a page of its own.
    const bool abstract = !details_.abstract.empty();
    const bool abstractAlone = abstract && y > top && bottom - (y + 44) <= 50;
    pages_ = static_cast<int>(starts.size()) + (abstractAlone ? 1 : 0);
    page_ = std::clamp(page_, 0, pages_ - 1);

    removeTargets_.clear();
    openTargets_.clear();
    actionRects_.assign(actions_.size(), Rect{});  // by action; empty: not on this page
    y = top;
    const size_t from = static_cast<size_t>(page_) < starts.size() ? starts[static_cast<size_t>(page_)] : pieces.size();
    const size_t to = static_cast<size_t>(page_) + 1 < starts.size() ? starts[static_cast<size_t>(page_) + 1] : pieces.size();
    for (size_t i = from; i < to; ++i) y += pieces[i](cr, y);
    if (abstract && page_ == pages_ - 1) {
        drawText(cr, "Abstract", label, {x, y, w, 40}, 1);
        y += 44;
        if (bottom - y > 50) drawText(cr, details_.abstract, body, {x, y, w, bottom - y}, 1000);
    }
    if (!details_.files.empty() && open_) drawButton(cr, openRect(), "Open", true);
    drawPageIndicator(cr, pageIndicatorRect(), page_, pages_);
    if (result_) result_->draw(cr);
}

void ItemDetailsScreen::onGesture(const Gesture& g, Nav& nav) {
    if (result_ && result_->shown()) {  // the result pop-up takes every gesture until it is closed
        if (result_->onGesture(g, nav) == PluginResult::Status::Closed) {
            result_.reset();
            running_ = -1;
        }
        return;
    }
    if (g.type == GestureType::Swipe && (g.dir == Direction::Up || g.dir == Direction::Down)) {
        const int to = page_ + (g.dir == Direction::Up ? 1 : -1);
        if (to >= 0 && to < pages_) {
            page_ = to;
            confirming_.clear();
            nav.damage({0, 0, W(), H()}, Waveform::Quality);
        }
        return;
    }
    if (g.type != GestureType::Tap) return;
    if (headerBackRect().contains(g.pos)) {
        nav.pop();
        return;
    }
    if (!details_.files.empty() && open_ && openRect().contains(g.pos)) {
        open_(row_);
        return;
    }
    for (size_t i = 0; i < actionRects_.size() && i < actions_.size(); ++i)
        if (actionRects_[i].contains(g.pos)) {
            if (result_) {  // one run at a time; its "Working…" row tapped again stops it
                if (static_cast<int>(i) == running_) {
                    result_->stop();
                    result_.reset();
                    running_ = -1;
                    nav.damage(actionRects_[i], Waveform::Ui);
                }
                return;
            }
            PluginEntries* e = nav.pluginEntries();
            auto item = nav.library().item(row_.key);
            if (!e || !item) return;
            result_ = std::make_unique<PluginResult>(*e, actions_[i], "itemAction",
                                                     plugins::itemJson(*item, nav.library().isRead(row_.key), nav.library().libraryName(item->lib), nav.library().readingInfo(row_.key)), Rect{0, 0, W(), H()});
            running_ = static_cast<int>(i);
            nav.damage(actionRects_[i], Waveform::Ui);
            return;
        }
    for (const auto& [key, r] : openTargets_)
        if (r.contains(g.pos)) {
            if (auto row = nav.library().rowFor(row_.key, key)) open_(*row);
            return;
        }
    for (const auto& [key, r] : removeTargets_)
        if (r.contains(g.pos)) {
            if (confirming_ != key) {
                confirming_ = key;
                nav.damage(r, Waveform::Ui);
                return;
            }
            confirming_.clear();
            nav.library().removeDownload(key);
            onShow(nav);
            nav.damage({0, 0, W(), H()}, Waveform::Ui);
            return;
        }
    if (!confirming_.empty()) {
        confirming_.clear();
        nav.damage({0, 0, W(), H()}, Waveform::Ui);
    }
}

}  // namespace zk
