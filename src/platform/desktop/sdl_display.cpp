// Desktop display for development: an SDL window showing the 8-bit gray framebuffer at
// half size. Mouse = finger. Waveforms are logged, not simulated. ZK_COLOR=1 imitates a color
// panel (Colorsoft): with Display → Color on, the window shows the canvas's colors.
#include <cmath>
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "core/log.h"
#include "core/wake.h"
#include "platform/display.h"
#include "ui/metrics.h"

namespace zk {

namespace {


int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Workers wake the loop with an SDL user event (core/wake.h); SDL_PushEvent is thread-safe.
Uint32 gWakeEvent = static_cast<Uint32>(-1);

void pushWake() {
    if (gWakeEvent == static_cast<Uint32>(-1)) return;
    SDL_Event e{};
    e.type = gWakeEvent;
    SDL_PushEvent(&e);
}

class SdlDisplay final : public Display {
public:
    // The screen of the Kindle being imitated (metrics(): --device pw5|scribe|kt4), in its own
    // pixels: a KT4's 167-ppi screen is smaller than the window the app lays out in.
    SdlDisplay()
        : kW(static_cast<int>(std::lround(metrics().width * metrics().scale))),
          kH(static_cast<int>(std::lround(metrics().height * metrics().scale))),
          kScale(metrics().scale < 1 ? 1 : 2) {
        SDL_Init(SDL_INIT_VIDEO);
        win_ = SDL_CreateWindow("Zotero for Kindle (desktop)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                kW / kScale, kH / kScale, 0);
        ren_ = SDL_CreateRenderer(win_, -1, 0);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
        gray_.assign((size_t)kW * kH, 0xff);
        rgb_.assign((size_t)kW * kH, 0xffffff);
        gWakeEvent = SDL_RegisterEvents(1);
        setUiWaker(pushWake);
    }

    int kW, kH;
    const int kScale;  // desktop window: half size, but a small screen at full size
    std::string deviceName() const override { return metrics().device; }  // the Kindle imitated
    int ppi() const override { return metrics().ppi; }
    // Turning swaps the window's sides, as the Kindle's window manager does.
    void setOrientation(Orientation o) override {
        if (o == orientation_) return;
        orientation_ = o;
        std::swap(kW, kH);
        SDL_SetWindowSize(win_, kW / kScale, kH / kScale);
        SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
        gray_.assign((size_t)kW * kH, 0xff);
        rgb_.assign((size_t)kW * kH, 0xffffff);
        resized_ = true;
    }
    Orientation orientation_ = Orientation::Portrait;
    bool resized_ = false;

    ~SdlDisplay() override {
        setUiWaker(nullptr);
        SDL_DestroyTexture(tex_);
        SDL_DestroyRenderer(ren_);
        SDL_DestroyWindow(win_);
        SDL_Quit();
    }

    int width() const override { return kW; }
    int height() const override { return kH; }

    void present(const Canvas& canvas, Rect r, Waveform w) override {
        r = r.intersected({0, 0, kW, kH});
        if (r.empty()) return;
        const char* name =
            w == Waveform::Fast || w == Waveform::Ink ? "fast" : w == Waveform::Ui ? "ui" : w == Waveform::Quality ? "quality" : "full";
        if (colorCaps().on) {
            const bool colored = toXrgb(canvas, reinterpret_cast<uint8_t*>(rgb_.data()), kW * 4, r, 0xff0000, 0x00ff00, 0x0000ff);
            log(LogLevel::Debug, "present %d,%d %dx%d %s (color: %s)", r.x, r.y, r.w, r.h, name,
                colorWfmName(colorWaveform(w, colored)));
        } else {
            canvas.toGray(gray_.data(), kW, r);
            log(LogLevel::Debug, "present %d,%d %dx%d %s", r.x, r.y, r.w, r.h, name);
        }
        upload();
    }

    ColorCaps colorCaps() const override { return decideColorCaps(false, colorSetting_, std::getenv("ZK_COLOR")); }
    void setColorScreen(bool on) override { colorSetting_ = on; }
    // The device test on the desktop: the window's RGB stands for the X path.
    std::vector<ColorPath> colorPaths() const override {
        return colorCaps().panel ? std::vector<ColorPath>{ColorPath::X} : std::vector<ColorPath>{};
    }
    bool presentWith(const Canvas& canvas, Rect r, ColorPath path, ColorWfm wfm) override {
        r = r.intersected({0, 0, kW, kH});
        if (path == ColorPath::Fb || (path == ColorPath::X && !colorCaps().panel)) return false;
        const bool wasOn = colorSetting_;
        colorSetting_ = path == ColorPath::X;
        const ColorCaps caps = colorCaps();
        if (caps.on) toXrgb(canvas, reinterpret_cast<uint8_t*>(rgb_.data()), kW * 4, r, 0xff0000, 0x00ff00, 0x0000ff);
        else canvas.toGray(gray_.data(), kW, r);
        log(LogLevel::Debug, "present %d,%d %dx%d with %s", r.x, r.y, r.w, r.h, colorWfmName(wfm));
        upload();
        colorSetting_ = wasOn;
        return true;
    }

    bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) override {
        if (resized_) {
            resized_ = false;
            InputEvent e;
            e.kind = InputEvent::Kind::Resize;
            out.push_back(e);
        }
        // -1: no limit (nothing is due); a worker's wake comes as an SDL event.
        SDL_Event ev;
        if (uiWakePending()) timeout_ms = 0;
        const bool got = SDL_WaitEventTimeout(&ev, timeout_ms);
        uiWakeTaken();  // before the loop reads the workers' queues
        if (!got) return true;
        do {
            if (ev.type == gWakeEvent) continue;
            switch (ev.type) {
                case SDL_QUIT: return false;
                case SDL_MOUSEBUTTONDOWN:
                    down_ = true;
                    touch(out, {TouchEvent::Type::Down, 0, {ev.button.x * kScale, ev.button.y * kScale}, nowMs()});
                    break;
                case SDL_MOUSEMOTION:
                    if (down_)
                        touch(out, {TouchEvent::Type::Move, 0, {ev.motion.x * kScale, ev.motion.y * kScale}, nowMs()});
                    break;
                case SDL_MOUSEBUTTONUP:
                    down_ = false;
                    touch(out, {TouchEvent::Type::Up, 0, {ev.button.x * kScale, ev.button.y * kScale}, nowMs()});
                    break;
                case SDL_TEXTINPUT: key(out, KeyEvent::Code::Text, ev.text.text); break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_BACKSPACE) key(out, KeyEvent::Code::Backspace, "");
                    if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) key(out, KeyEvent::Code::Return, "");
                    if (ev.key.keysym.sym == SDLK_ESCAPE) key(out, KeyEvent::Code::Escape, "");
                    break;
                case SDL_WINDOWEVENT: upload(); break;
                default: break;
            }
        } while (SDL_PollEvent(&ev));
        return true;
    }

private:
    static void touch(std::vector<InputEvent>& out, TouchEvent t) {
        InputEvent e;
        e.touch = t;
        out.push_back(e);
    }
    static void key(std::vector<InputEvent>& out, KeyEvent::Code code, const char* text) {
        InputEvent e;
        e.kind = InputEvent::Kind::Key;
        e.key = {code, text};
        out.push_back(e);
    }

    void upload() {
        const bool color = colorCaps().on;
        void* pixels;
        int pitch;
        SDL_LockTexture(tex_, nullptr, &pixels, &pitch);
        for (int y = 0; y < kH; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
            if (color) {
                std::copy_n(rgb_.data() + (size_t)y * kW, kW, row);
                continue;
            }
            for (int x = 0; x < kW; ++x) {
                uint32_t g = gray_[(size_t)y * kW + x];
                row[x] = (g << 16) | (g << 8) | g;
            }
        }
        SDL_UnlockTexture(tex_);
        SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
        SDL_RenderPresent(ren_);
    }

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    std::vector<uint8_t> gray_;
    std::vector<uint32_t> rgb_;  // with color on: what the window shows
    bool colorSetting_ = true;   // Display → Color
    bool down_ = false;
};

}  // namespace

std::unique_ptr<Display> makeDisplay() { return std::make_unique<SdlDisplay>(); }

void displayEmergencyExit() {}
void displayResetSystemInk() {}

const char* fontDir() { return "assets/fonts-device"; }

}  // namespace zk
