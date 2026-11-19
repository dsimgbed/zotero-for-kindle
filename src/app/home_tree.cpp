#include "app/home_tree.h"

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
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << json{{"pluginId", pluginId}, {"screenId", screenId}, {"tree", tree}}.dump(-1, ' ', false,
                                                                                          json::error_handler_t::replace);
        out.flush();
        if (!out) {
            log(LogLevel::Warn, "home: can't write %s", tmp.c_str());
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, file_, ec);
    if (ec) {
        log(LogLevel::Warn, "home: can't keep the tree in %s: %s", file_.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
    }
}

PluginHome::PluginHome(PluginEvents& events, HomeTreeStore& store, std::function<std::string()> choice, ItemSource* items,
                       ItemTap tap)
    : events_(events), store_(store), choice_(std::move(choice)), items_(items), tap_(std::move(tap)) {}

PluginHome::~PluginHome() = default;

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
}

void PluginHome::synced() {
    if (body_) events_.rerender(body_->instance());
}

void PluginHome::drop(Nav& nav) {
    if (!body_) return;
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
    if (out.kind == PluginHostEvent::Kind::ScreenTree && !out.value.is_null() && body_ && instance == body_->instance()) {
        if (out.value != saved_) {  // a new tree: kept for the next start
            store_.save(pluginId_, screenId_, out.value);
            saved_ = out.value;
        }
        out.value = nullptr;
    }
    return true;
}

bool PluginHome::takeItemOpened(uint64_t instance) { return events_.takeItemOpened(instance); }

}  // namespace zk
