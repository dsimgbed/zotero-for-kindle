#include "app/previews.h"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "app/app.h"
#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "platform/keyboard.h"
#include "plugins/host.h"
#include "screens/plugin_screen.h"
#include "screens/preview_screen.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/null_sync.h"
#include "ui/canvas.h"
#include "ui/metrics.h"

namespace zk {

std::unique_ptr<ReaderScreen> previewReader(const PreviewEntry& e, const EngineEnv& env, int flashEvery, Lipc* lipc) {
    ItemRow row;
    row.title = e.title;
    row.attachmentKey = "preview:" + e.key;
    row.badge = e.type == PreviewType::Html ? "WEB" : e.type == PreviewType::Pdf ? "PDF" : "EPUB";
    return std::make_unique<ReaderScreen>(
        row, e.file, e.type == PreviewType::Pdf ? DocType::Pdf : DocType::Epub,
        [env] {
            return std::make_unique<RenderWorker>([env](DocType t) {
                UserError err;
                return loadEngine(t, env, &err);
            });
        },
        flashEvery, lipc);
}

PluginEvents::PreviewOpener previewOpener(PreviewCache& cache, std::function<PluginHost*()> host,
                                          std::function<std::unique_ptr<ReaderScreen>(const PreviewEntry&)> makeReader) {
    return [&cache, host = std::move(host), makeReader = std::move(makeReader)](
               App& app, const PluginHostEvent& ev, std::function<void(const std::string&)> answer, std::function<void()> closed) {
        PreviewDoc doc{ev.value.value("url", ""), ev.value.value("html", ""), ev.value.value("title", ""), ev.value.value("baseUrl", "")};
        PreviewScreen::Deps d;
        d.cache = &cache;
        PluginHost* h = host();
        d.download = [h, id = ev.pluginId](HttpRequest req) {
            if (!h) throw std::runtime_error("Plugins aren't running");
            return h->previewDownload(id, std::move(req), PreviewCache::kMaxDownloadBytes, 5 * 60 * 1000);
        };
        d.makeReader = makeReader;
        d.answer = std::move(answer);
        d.closed = std::move(closed);
        PreviewScreen::show(std::move(doc), std::move(d), app);
    };
}

namespace {

class NoKeyboard final : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

}  // namespace

int pluginScreenshot(const std::string& pluginsDir, const std::string& dbPath, const EngineEnv& env, Version version,
                     const std::string& id, const std::string& screenId, int page, const std::string& out) {
    Db db = Db::open(dbPath);
    Library lib(db);
    PluginHostDeps d;
    d.pluginsDir = pluginsDir;
    d.dbPath = dbPath;
    d.engineEnv = env;
    d.appVersion = version;
    d.loadEagerly = true;
    d.renderBudgetMs = 5000;
    PluginHost host(d);
    host.setEnabled(id, true);
    host.start();
    std::string name = id, title = screenId;
    for (int i = 0; i < 300 && title == screenId; ++i) {
        for (const auto& st : host.status())
            if (st.folder.dir == id) {
                if (st.folder.manifest) name = st.folder.manifest->name;
                for (const auto& e : st.entries)
                    if (e.kind == "screen" && e.id == screenId) title = e.title;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    struct Link final : ScreenLink {
        PluginHost& host;
        explicit Link(PluginHost& h) : host(h) {}
        void send(uint64_t instance, nlohmann::json e) override { host.screenEvent(instance, std::move(e)); }
        void left(uint64_t instance) override { host.closeScreen(instance); }
        bool take(uint64_t instance, PluginHostEvent& got) override {
            PluginHostEvent ev;
            while (host.poll(ev))
                if (ev.instance == instance) {
                    got = std::move(ev);
                    return true;
                }
            return false;
        }
    } link(host);
    struct ShotNav final : Nav {
        Library& lib;
        NullSync syncNull;
        NoKeyboard kb;
        explicit ShotNav(Library& l) : lib(l) {}
        void push(std::unique_ptr<Screen>) override {}
        void pop() override {}
        void switchTab(int) override {}
        void damage(Rect, Waveform) override {}
        void redrawAll(Waveform) override {}
        SyncControl& sync() override { return syncNull; }
        Library& library() override { return lib; }
        Keyboard& keyboard() override { return kb; }
        int64_t nowMs() const override { return 0; }
        void quit() override {}
        void toast(const std::string&) override {}
        const Screen* top() const override { return nullptr; }
    } nav(lib);
    const uint64_t instance = host.openScreen(id, screenId);
    PluginScreen ps(link, instance, name, title);
    ps.onShow(nav);
    for (int i = 0; i < 500 && ps.state() == PluginScreen::State::Loading; ++i) {
        ps.onTick(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const Point at{metrics().width / 2, metrics().height / 3};
    for (int i = 0; i < page; ++i) ps.onGesture(Gesture{GestureType::Swipe, at, at, Direction::Up}, nav);
    Canvas c(metrics().width, metrics().height);
    ps.draw(c);
    const bool ok = ps.state() == PluginScreen::State::Shown && cairo_surface_write_to_png(c.surface(), out.c_str()) == CAIRO_STATUS_SUCCESS;
    host.stop();
    return ok ? 0 : 1;
}

}  // namespace zk
