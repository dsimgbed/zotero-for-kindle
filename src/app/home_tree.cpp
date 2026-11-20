#include "app/home_tree.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app/plugin_events.h"
#include "core/log.h"
#include "plugins/ui_tree.h"

namespace zk {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::optional<json> HomeTreeStore::load(const std::string& pluginId, const std::string& screenId) const {
    std::ifstream in(file_, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream text;
    text << in.rdbuf();
    const json j = json::parse(text.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("pluginId", "") != pluginId || j.value("screenId", "") != screenId ||
        !j.contains("tree"))
        return std::nullopt;
    return j["tree"];
}

void HomeTreeStore::save(const std::string& pluginId, const std::string& screenId, const json& tree) const {
    const std::string tmp = file_ + ".tmp";
    std::error_code ec;
    if (const fs::path dir = fs::path(file_).parent_path(); !dir.empty()) fs::create_directories(dir, ec);
    const std::string text =
        json{{"pluginId", pluginId}, {"screenId", screenId}, {"tree", tree}}.dump(-1, ' ', false, json::error_handler_t::replace);
    // Synced before the rename: a power cut right after must not leave an empty file in its place.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const bool ok = fd >= 0 && ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    if (fd >= 0) ::close(fd);
    if (!ok) {
        log(LogLevel::Warn, "home: can't write %s: %s", tmp.c_str(), std::strerror(errno));
        fs::remove(tmp, ec);
        return;
    }
    fs::rename(tmp, file_, ec);
    if (ec) {
        log(LogLevel::Warn, "home: can't keep the tree in %s: %s", file_.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
    }
}

void HomeTreeStore::forget() const {
    std::error_code ec;
    fs::remove(file_, ec);
}

PluginHome::PluginHome(PluginEvents& events, HomeTreeStore& store, std::function<std::string()> choice, ItemSource* items,
                       ItemTap tap, std::function<int64_t()> clock)
    : events_(events), store_(store), choice_(std::move(choice)), items_(items), tap_(std::move(tap)), clock_(std::move(clock)) {
    if (!clock_)
        clock_ = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };
}

PluginHome::~PluginHome() { keep(true); }

void PluginHome::keep(bool now) {
    if (waiting_.is_null()) return;
    const int64_t t = clock_();
    if (!now && writtenMs_ && t - *writtenMs_ < kKeepEveryMs) return;
    store_.save(pluginId_, screenId_, waiting_);
    waiting_ = nullptr;
    writtenMs_ = t;
}

void PluginHome::shown(Nav& nav, std::function<Rect(Nav&)> area, std::function<Rect()> window) {
    area_ = std::move(area);
    window_ = std::move(window);
    const std::string choice = choice_ ? choice_() : "";
    const size_t slash = choice.find('/');
    const std::string pluginId = slash == std::string::npos ? "" : choice.substr(0, slash);
    const std::string screenId = slash == std::string::npos ? "" : choice.substr(slash + 1);
    if (body_ && (pluginId != pluginId_ || screenId != screenId_)) drop(nav);  // Settings → Home changed
    if (body_) {
        events_.rerender(body_->instance());
        return;
    }
    if (pluginId.empty() || screenId.empty() || !events_.homeAvailable(pluginId, screenId)) return;
    const PluginEvents::Opened o = events_.openHome(pluginId, screenId);
    if (!o.instance) return;
    pluginId_ = pluginId;
    screenId_ = screenId;
    saved_ = nullptr;
    waiting_ = nullptr;
    kept_.reset();
    if (auto j = store_.load(pluginId, screenId)) {
        std::string why;
        if (auto tree = parseTree(*j, &why, TreeUse::HomeBody)) {
            PluginHostEvent ev{PluginHostEvent::Kind::ScreenTree, pluginId, o.pluginName, "", {}, 0, ""};
            ev.instance = o.instance;
            ev.screenId = screenId;
            ev.tree = std::make_shared<const Block>(std::move(*tree));
            kept_ = std::move(ev);
            saved_ = std::move(*j);
        } else {
            log(LogLevel::Warn, "home: the kept tree of %s/%s can't be shown: %s", pluginId.c_str(), screenId.c_str(), why.c_str());
        }
    }
    body_ = std::make_unique<PluginBody>(
        *this, o.instance, o.pluginName, o.title, [this](Nav& n) { return area_(n); }, TreeUse::HomeBody, items_, tap_);
    body_->setWindow([this] { return window_(); });
    body_->setPackage(o.package);
}

void PluginHome::synced() {
    if (body_) events_.rerender(body_->instance());
}

void PluginHome::drop(Nav& nav) {
    if (!body_) return;
    keep(true);  // still this body's ids
    body_->onClose(nav);  // the host hears `left` (not when the plugin closed it itself)
    body_.reset();
    kept_.reset();
    pluginId_.clear();
    screenId_.clear();
}

void PluginHome::send(uint64_t instance, json event) { events_.send(instance, std::move(event)); }

void PluginHome::left(uint64_t instance) { events_.left(instance); }

bool PluginHome::take(uint64_t instance, PluginHostEvent& out) {
    if (kept_ && body_ && instance == body_->instance()) {
        out = std::move(*kept_);
        kept_.reset();
        return true;
    }
    if (!events_.take(instance, out)) return false;
    const bool mine = body_ && instance == body_->instance();
    if (out.kind == PluginHostEvent::Kind::ScreenTree && !out.value.is_null() && mine) {
        if (out.value != saved_) {  // a new tree: kept for the next start
            saved_ = out.value;
            waiting_ = out.value;
            keep(false);
        }
        out.value = nullptr;
    } else if (out.kind == PluginHostEvent::Kind::ScreenError && mine && out.value.is_object() && out.value.value("missing", false)) {
        log(LogLevel::Info, "home: %s/%s is gone from its plugin: its kept tree goes", pluginId_.c_str(), screenId_.c_str());
        waiting_ = nullptr;
        store_.forget();  // no stale tree drawn in the next start's first frame
    }
    return true;
}

bool PluginHome::takeItemOpened(uint64_t instance) { return events_.takeItemOpened(instance); }

void PluginHome::rerender(uint64_t instance) { events_.rerender(instance); }

}  // namespace zk
