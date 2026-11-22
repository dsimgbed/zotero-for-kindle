// Kindle display: an X11 window under the awesome WM + explicit e-ink refresh via FBInk.
// See research/spikes/s2-s5-window-refresh.md.
// Color (Colorsoft, platform/color.h): the X path when X has a 24-bit TrueColor visual (a 32-bit
// image), else the framebuffer path (X keeps its 8-bit gray; the region's RGB goes to the
// framebuffer through FBInk right before the refresh). Color off is the gray path as before.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fbink.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <bit>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/wake.h"
#include "platform/color.h"
#include "platform/display.h"
#include "platform/keysym.h"
#include "platform/kindle/evdev_pen.h"
#include "platform/kindle/expose_policy.h"
#include "platform/kindle/wet_ink.h"
#include "platform/kindle/x_touch.h"

namespace zk {

namespace {

constexpr const char* kWindowTitle = "L:A_N:application_PC:T_ID:org.zotero.kindle_O:U";

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The self-pipe workers wake the loop through (core/wake.h): its read end is in pollEvents' poll.
int gWakePipe[2] = {-1, -1};

void writeWake() {
    if (gWakePipe[1] < 0) return;
    const char b = 1;
    [[maybe_unused]] const ssize_t n = ::write(gWakePipe[1], &b, 1);  // full (EAGAIN): a wake is waiting anyway
}

// SIGWINCH, from the launcher when Zotero is started again while it runs (assets/launcher/Zotero.sh):
// come to the front. The handler only notes it and wakes the loop (both async-signal-safe).
volatile sig_atomic_t gRaiseRequested = 0;

void onRaiseSignal(int) {
    gRaiseRequested = 1;
    writeWake();
}

class X11Display final : public Display {
public:
    X11Display() {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            log(LogLevel::Error, "cannot open X display");
            return;
        }
        // FBInk first: the device it names decides the window (wet ink needs it full screen).
        fb_ = fbink_open();
        std::memset(&cfg_, 0, sizeof cfg_);
        cfg_.is_quiet = true;
        if (fb_ < 0 || fbink_init(fb_, &cfg_) < 0) {
            log(LogLevel::Warn, "FBInk init failed: X refreshes only");
        } else {
            FBInkState state{};
            fbink_get_state(&cfg_, &state);
            deviceName_ = state.device_name;
            ppi_ = state.screen_dpi;
            panelFound_ = state.has_color_panel;
            log(LogLevel::Info,
                "FBInk: %s (%s, id %#x), view %ux%u, %u bpp, pixel format %u, rotation %u, color panel %s, MTK %s",
                state.device_name, state.device_codename, static_cast<unsigned>(state.device_id), state.view_width,
                state.view_height, state.bpp, static_cast<unsigned>(state.pixel_format), static_cast<unsigned>(state.current_rota),
                state.has_color_panel ? "yes" : "no", state.is_mtk ? "yes" : "no");
        }
        rawCfg_ = cfg_;  // writing pixels (the framebuffer path): no refresh of its own, RGB without alpha
        rawCfg_.no_refresh = true;
        rawCfg_.ignore_alpha = true;
        wfmAuto_ = envIs("ZK_COLOR_WFM", "auto");
        int scr = DefaultScreen(dpy_);
        readVisual(scr);
        w_ = DisplayWidth(dpy_, scr);
        h_ = DisplayHeight(dpy_, scr);
        win_ = XCreateSimpleWindow(dpy_, RootWindow(dpy_, scr), 0, 0, w_, h_, 0, 0, WhitePixel(dpy_, scr));
        // Wet ink draws only over the top window: the status bar would be it, so go full screen.
        // Only where its protocol was checked (the 1st-gen Scribe); ZK_WET_INK=1 / ZK_NO_WET_INK=1.
        const char* forced = std::getenv("ZK_WET_INK");
        const bool wetInk = std::getenv("ZK_NO_WET_INK") == nullptr &&
                            (deviceName_ == "Scribe" || (forced && std::strcmp(forced, "1") == 0));
        if (wetInk) wet_ = WetInk::open();
        if (wet_) title_.replace(title_.find("PC:T"), 4, "PC:N");
        XStoreName(dpy_, win_, title_.c_str());
        XSelectInput(dpy_, win_, ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                                     Button1MotionMask | FocusChangeMask | VisibilityChangeMask | KeyPressMask);
        XMapWindow(dpy_, win_);
        gc_ = XCreateGC(dpy_, win_, 0, nullptr);
        waitForFinalGeometry();
        decideColor();
        makeImage();

        {
            // The panel is portrait in its native orientation; the root window shows how it's turned now.
            const int rw = DisplayWidth(dpy_, scr), rh = DisplayHeight(dpy_, scr);
            pen_ = std::make_unique<EvdevPen>(std::min(rw, rh), std::max(rw, rh), wet_ != nullptr);
            updatePenView();
            if (wet_ && !wet_->connect(win_, std::min(rw, rh), std::max(rw, rh))) {
                // No wet ink after all: the status bar back, and the pen as the system re-sends it.
                wet_.reset();
                title_.replace(title_.find("PC:N"), 4, "PC:T");
                XStoreName(dpy_, win_, title_.c_str());
                XFlush(dpy_);
                pen_ = std::make_unique<EvdevPen>(std::min(rw, rh), std::max(rw, rh));
                updatePenView();
            }
        }
        if (gWakePipe[0] < 0 && ::pipe(gWakePipe) == 0) {
            for (int fd : gWakePipe) {
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            setUiWaker(writeWake);
        }
        struct sigaction sa {};
        sa.sa_handler = onRaiseSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &sa, nullptr);
        log(LogLevel::Info, "window %dx%d at (%d,%d)", w_, h_, originX_, originY_);
    }

    // How the screen is turned from the digitizer. The raw digitizer (wet ink) stays with the
    // panel: the framebuffer's rotation says how far the picture is turned, upside down included
    // (the Scribe turns 180 degrees for the left hand without the window changing size).
    // fb0 rotate 3 is upright on the Scribe (2026-11-07: raw corners matched the screen).
    // Otherwise landscape reading is a quarter turn (clockwise assumed; ZK_PEN_TURNS overrides).
    void updatePenView() {
        if (!pen_) return;
        int turns = 0;
        if (wet_) {
            if (FILE* f = std::fopen("/sys/class/graphics/fb0/rotate", "r")) {
                int r = 3;
                if (std::fscanf(f, "%d", &r) == 1) turns = ((3 - r) % 4 + 4) % 4;
                std::fclose(f);
            }
        } else {
            const int scr = DefaultScreen(dpy_);
            turns = DisplayWidth(dpy_, scr) > DisplayHeight(dpy_, scr) ? 1 : 0;
        }
        if (turns != penTurns_) log(LogLevel::Info, "pen: screen turned %d quarter(s) from the digitizer", turns);
        penTurns_ = turns;
        pen_->setView(turns, {originX_, originY_});
    }

    ~X11Display() override {
        setUiWaker(nullptr);
        wet_.reset();  // leaves the handler off before our window goes
        if (image_) {
            image_->data = nullptr;
            XDestroyImage(image_);
        }
        if (fb_ >= 0) fbink_close(fb_);
        if (dpy_) XCloseDisplay(dpy_);
    }

    int width() const override { return w_; }
    int height() const override { return h_; }
    Point origin() const override { return {originX_, originY_}; }
    std::string deviceName() const override { return deviceName_; }
    int ppi() const override { return ppi_; }
    ColorCaps colorCaps() const override { return caps_; }
    void setColorScreen(bool on) override {
        if (on == colorSetting_) return;
        colorSetting_ = on;
        decideColor();
        if (path_ == ColorPath::Fb && rgb_.size() != (size_t)w_ * h_) rgb_.assign((size_t)w_ * h_, 0xffffff);
    }
    std::vector<ColorPath> colorPaths() const override {
        std::vector<ColorPath> out;
        if (x32_) out.push_back(ColorPath::X);
        if (fbWritable()) out.push_back(ColorPath::Fb);
        return out;
    }
    // The device test: the gray path as color off shows it, or a color path, with this waveform.
    bool presentWith(const Canvas& canvas, Rect r, ColorPath path, ColorWfm wfm) override {
        r = r.intersected({0, 0, w_, h_});
        if (r.empty() || !dpy_) return false;
        if ((path == ColorPath::X && !x32_) || (path == ColorPath::Fb && !fbWritable())) return false;
        const ColorPath saved = path_;
        path_ = path;
        if (path == ColorPath::Fb && rgb_.size() != (size_t)w_ * h_) rgb_.assign((size_t)w_ * h_, 0xffffff);
        waitIfColoredBefore();
        const bool colored = convert(canvas, r);
        log(LogLevel::Info, "color test: path %s, %s, region %scolored", colorPathName(path), colorWfmName(wfm),
            colored ? "" : "not ");
        put(r);
        // Without a flash (the test flashed the screen white before), as the app's own updates.
        const bool shown = fb_ >= 0 && (path != ColorPath::Fb || writeFb(r)) && refresh(r, Waveform::Quality, wfm, colored);
        if (fb_ >= 0) {
            const auto t0 = std::chrono::steady_clock::now();
            const int rc = fbink_wait_for_complete(fb_, LAST_MARKER);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
            log(LogLevel::Info, "color test: refresh done in %lld ms (wait %d)", static_cast<long long>(ms.count()), rc);
        }
        path_ = saved;
        return shown;
    }
    // The window manager turns the screen for our window when the title's orientation field
    // changes (O:U upright, O:L landscape; spike 2026-11-07); ConfigureNotify then brings the size.
    void setOrientation(Orientation o) override {
        if (!dpy_) return;
        std::string title = title_;
        title.back() = o == Orientation::Landscape ? 'L' : 'U';
        XStoreName(dpy_, win_, title.c_str());
        XFlush(dpy_);
    }

    void present(const Canvas& canvas, Rect r, Waveform w) override {
        r = r.intersected({0, 0, w_, h_});
        if (r.empty() || !dpy_) return;
        if (path_ == ColorPath::Gray && !x32_) {  // color off on an 8-bit X: as it always was
            canvas.toGray(backing_.data(), w_, r);
            putAndRefresh(r, w);
            return;
        }
        waitIfColoredBefore();
        putAndRefresh(r, w, convert(canvas, r));
    }

    bool repaintsOnForeground() const override { return true; }  // Expose: from the backing store
    bool systemInk() const override { return wet_ && wet_->connected(); }
    // The handler's 3.5 (its notebook's default pen) draws about the app's 7 px line.
    void setSystemInkWidth(int px) override {
        if (wet_) wet_->setThickness(px / 2.0f);
    }
    bool setSystemInk(bool on) override {
        return !wet_ || wet_->setMode(on ? WetInk::Mode::Pen : WetInk::Mode::Paused);
    }

    // Waits for input, a worker's wake or `timeout_ms` (-1: no limit; nothing is due).
    bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) override {
        if (!dpy_) return false;
        if (uiWakePending()) timeout_ms = 0;  // e.g. one from before the pipe existed
        if (!XPending(dpy_)) {  // (XPending also sends what is queued for the server)
            // A negative fd is skipped by poll(): the pen may be gone. In the background the app
            // ignores the pen, so a pen hovering over the screensaver or a system dialog mustn't
            // wake it at the digitizer's rate; what it sent is read (and dropped) at the next wake,
            // and an overflow there is a SYN_DROPPED the decoder already handles.
            const int penFd = pen_ && !background_ ? pen_->fd() : -1;
            pollfd p[3] = {{ConnectionNumber(dpy_), POLLIN, 0}, {penFd, POLLIN, 0}, {gWakePipe[0], POLLIN, 0}};
            poll(p, 3, timeout_ms);
            if (p[1].fd >= 0 && (p[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                log(LogLevel::Warn, "pen: the digitizer went away");
                pen_.reset();
                InputEvent e;  // whatever it was doing has ended
                e.kind = InputEvent::Kind::Pen;
                e.pen.type = PenEvent::Type::Leave;
                e.pen.t_ms = nowMs();
                out.push_back(e);
            }
        }
        // Taken before the loop reads the workers' queues: a wake after this one wakes the next wait.
        uiWakeTaken();
        if (gWakePipe[0] >= 0) {
            char buf[64];
            while (::read(gWakePipe[0], buf, sizeof buf) > 0) {
            }
        }
        if (gRaiseRequested) {
            gRaiseRequested = 0;
            raise();
        }
        // The pen first: its strokes are what must not wait (palm touches queued behind it are
        // then dropped by the app while the pen is near).
        if (pen_ && wet_ && nowMs() - lastTurnCheckMs_ > 1000) {  // a 180-degree turn sends no event
            lastTurnCheckMs_ = nowMs();
            updatePenView();
        }
        if (pen_) pen_->read(out, nowMs());
        while (XPending(dpy_)) {
            XEvent ev;
            XNextEvent(dpy_, &ev);
            switch (ev.type) {
                case ButtonPress:
                case ButtonRelease:
                case MotionNotify: {
                    XPointerEvent kind = ev.type == ButtonPress     ? XPointerEvent::Press
                                         : ev.type == ButtonRelease ? XPointerEvent::Release
                                                                    : XPointerEvent::Motion;
                    unsigned button = ev.type == MotionNotify ? 0 : ev.xbutton.button;
                    Point pos = ev.type == MotionNotify ? Point{ev.xmotion.x, ev.xmotion.y}
                                                        : Point{ev.xbutton.x, ev.xbutton.y};
                    if (auto t = touchFromX(kind, button, fingerDown_, pos, nowMs())) {
                        if (t->type == TouchEvent::Type::Down) fingerDown_ = true;
                        if (t->type == TouchEvent::Type::Up) fingerDown_ = false;
                        InputEvent e;
                        e.touch = *t;
                        out.push_back(e);
                    }
                    break;
                }
                case KeyPress:
                    if (auto k = keyFromKeysym(keysymOf(ev.xkey))) {
                        InputEvent e;
                        e.kind = InputEvent::Kind::Key;
                        e.key = *k;
                        out.push_back(e);
                    }
                    break;
                case VisibilityNotify:
                    expose_.visibilityChanged(ev.xvisibility.state == VisibilityFullyObscured);
                    if (ev.xvisibility.state == VisibilityFullyObscured) setBackground(true, out);
                    break;
                case Expose:
                    // Repaint the exposed area from the backing store: flash only after wake.
                    exposed_ = exposed_.united({ev.xexpose.x, ev.xexpose.y, ev.xexpose.width, ev.xexpose.height});
                    if (ev.xexpose.count == 0) {
                        const Rect shown = exposed_.intersected({0, 0, w_, h_});
                        const Waveform w = expose_.onExpose();
                        if (path_ == ColorPath::Gray) {
                            putAndRefresh(shown, w);
                        } else if (!shown.empty()) {
                            waitIfColoredBefore();
                            putAndRefresh(shown, w, coloredNow(shown));
                        }
                        exposed_ = {};
                    }
                    setBackground(false, out);
                    break;
                case FocusOut:
                    log(LogLevel::Debug, "focus out (background)");
                    setBackground(true, out);
                    break;
                case FocusIn:
                    log(LogLevel::Debug, "focus in");
                    setBackground(false, out);
                    break;
                case ConfigureNotify:
                    if (ev.xconfigure.width != w_ || ev.xconfigure.height != h_) resize(ev.xconfigure.width, ev.xconfigure.height, out);
                    break;
                case DestroyNotify: return false;
                default: break;
            }
        }
        return true;
    }

private:
    // The text a key produces: XLookupString applies the keyboard's shift state; the Kindle's
    // keyboard sends ready-made keysyms (S3), so the unshifted keysym is the fallback.
    static unsigned long keysymOf(XKeyEvent& key) {
        KeySym sym = 0;
        char buf[8];
        XLookupString(&key, buf, sizeof buf, &sym, nullptr);
        return sym ? sym : XLookupKeysym(&key, 0);
    }

    // To the front: the window manager (lab126 awesome) makes a newly mapped application window the
    // active one, so the window goes and comes back. The Expose that follows repaints it from the
    // backing store and turns the app to the foreground (seen on a PW5, 5.18.4).
    void raise() {
        log(LogLevel::Info, "asked to come to the front");
        XUnmapWindow(dpy_, win_);
        XSync(dpy_, False);
        XMapRaised(dpy_, win_);
        XSync(dpy_, False);
    }

    void setBackground(bool background, std::vector<InputEvent>& out) {
        if (background == background_) return;
        background_ = background;
        InputEvent e;
        e.kind = background ? InputEvent::Kind::Background : InputEvent::Kind::Foreground;
        out.push_back(e);
    }

    void waitForFinalGeometry() {
        // The WM maps us full-screen, then resizes below the status bar (~350 ms).
        int64_t deadline = nowMs() + 2000;
        while (nowMs() < deadline) {
            pollfd p{ConnectionNumber(dpy_), POLLIN, 0};
            poll(&p, 1, 50);
            while (XPending(dpy_)) {
                XEvent ev;
                XNextEvent(dpy_, &ev);
                if (ev.type == ConfigureNotify) {
                    w_ = ev.xconfigure.width;
                    h_ = ev.xconfigure.height;
                }
            }
            if (h_ < DisplayHeight(dpy_, DefaultScreen(dpy_))) break;
        }
        Window child;
        XTranslateCoordinates(dpy_, win_, DefaultRootWindow(dpy_), 0, 0, &originX_, &originY_, &child);
    }

    // The window turned (or the window manager settled its size): new backing store, FBInk state
    // for the rotated framebuffer, and a Resize for the app.
    void resize(int w, int h, std::vector<InputEvent>& out) {
        w_ = w;
        h_ = h;
        Window child;
        XTranslateCoordinates(dpy_, win_, DefaultRootWindow(dpy_), 0, 0, &originX_, &originY_, &child);
        if (fb_ >= 0 && fbink_reinit(fb_, &cfg_) < 0) log(LogLevel::Warn, "FBInk reinit after a turn failed");
        decideColor();  // the framebuffer may not turn with the window
        makeImage();
        updatePenView();
        // The canvas as the screen is turned now, from the window (Xlib's DisplayWidth keeps the
        // size from when the display was opened and doesn't follow a turn).
        if (wet_) wet_->setCanvas(originX_ + w_, originY_ + h_);
        log(LogLevel::Info, "window %dx%d at (%d,%d)", w_, h_, originX_, originY_);
        InputEvent e;
        e.kind = InputEvent::Kind::Resize;
        out.push_back(e);
    }

    // The gray path (color off): the image to X, then the refresh with the waveform's mode.
    void putAndRefresh(Rect r, Waveform w) {
        XPutImage(dpy_, win_, gc_, image_, r.x, r.y, r.x, r.y, r.w, r.h);
        XSync(dpy_, False);
        if (fb_ < 0) return;
        cfg_.wfm_mode = w == Waveform::Ink    ? WFM_A2
                   : w == Waveform::Fast ? WFM_DU
                   : w == Waveform::Ui   ? WFM_GL16
                                         : WFM_GC16;
        cfg_.is_flashing = w == Waveform::Full;
        fbink_refresh(fb_, r.y + originY_, r.x + originX_, r.w, r.h, &cfg_);
    }

    // A color path (or gray on a 32-bit X): the image to X, the RGB to the framebuffer on that
    // path, then the refresh with the color panel's waveform for `w`.
    void putAndRefresh(Rect r, Waveform w, bool colored) {
        put(r);
        if (fb_ < 0) return;
        if (path_ == ColorPath::Fb && !writeFb(r)) colored = false;  // the framebuffer has X's gray
        ColorWfm cw = path_ == ColorPath::Gray ? ColorWfm::Keep : colorWaveform(w, colored);
        if (cw != ColorWfm::Keep && wfmAuto_) cw = ColorWfm::Auto;
        refresh(r, w, cw, colored);
    }

    void put(Rect r) {
        XPutImage(dpy_, win_, gc_, image_, r.x, r.y, r.x, r.y, r.w, r.h);
        XSync(dpy_, False);  // X has drawn it (into the framebuffer) before we write or refresh
    }

    bool refresh(Rect r, Waveform w, ColorWfm cw, bool colored) {
        switch (cw) {
            case ColorWfm::Gl16: cfg_.wfm_mode = WFM_GL16; break;
            case ColorWfm::Glrc16: cfg_.wfm_mode = WFM_GLRC16; break;
            case ColorWfm::Gc16: cfg_.wfm_mode = WFM_GC16; break;
            case ColorWfm::Gcc16: cfg_.wfm_mode = WFM_GCC16; break;
            case ColorWfm::Auto: cfg_.wfm_mode = WFM_AUTO; break;
            case ColorWfm::Keep:
                cfg_.wfm_mode = w == Waveform::Ink ? WFM_A2 : w == Waveform::Fast ? WFM_DU : w == Waveform::Ui ? WFM_GL16 : WFM_GC16;
                break;
        }
        cfg_.is_flashing = w == Waveform::Full;
        const int rc = fbink_refresh(fb_, r.y + originY_, r.x + originX_, r.w, r.h, &cfg_);
        log(rc < 0 ? LogLevel::Warn : LogLevel::Debug, "refresh %d,%d %dx%d path %s %s%s%s: %d", r.x, r.y, r.w, r.h,
            colorPathName(path_), colorWfmName(cw), cfg_.is_flashing ? " flashing" : "", colored ? " colored" : "", rc);
        // The driver works asynchronously: the next update waits until a colored one has finished
        // (KOReader does the same on these panels), so nothing lands over it half-way.
        if (colored && cw != ColorWfm::Keep) waitBeforeNext_ = true;
        return rc >= 0;
    }

    void waitIfColoredBefore() {
        if (!waitBeforeNext_ || fb_ < 0) return;
        waitBeforeNext_ = false;
        const int rc = fbink_wait_for_complete(fb_, LAST_MARKER);
        if (rc < 0) log(LogLevel::Debug, "waiting for the colored refresh: %d", rc);
    }

    // The region of the canvas into this path's buffers; whether it is colored.
    bool convert(const Canvas& canvas, Rect r) {
        auto* image32 = reinterpret_cast<uint8_t*>(backing32_.data());
        switch (path_) {
            case ColorPath::X: return toXrgb(canvas, image32, w_ * 4, r, rmask_, gmask_, bmask_);
            case ColorPath::Fb:
                if (x32_) toXgray(canvas, image32, w_ * 4, r, rmask_, gmask_, bmask_);
                else canvas.toGray(backing_.data(), w_, r);
                return toXrgb(canvas, reinterpret_cast<uint8_t*>(rgb_.data()), w_ * 4, r, 0xff0000, 0x00ff00, 0x0000ff);
            case ColorPath::Gray: break;
        }
        if (x32_) toXgray(canvas, image32, w_ * 4, r, rmask_, gmask_, bmask_);
        else canvas.toGray(backing_.data(), w_, r);
        return false;
    }

    // Whether what is already in this path's buffers is colored (an Expose repaints from them).
    // The X image's channels are whole bytes (readVisual), and chroma doesn't care which is which.
    bool coloredNow(Rect r) const {
        if (path_ == ColorPath::X) return isColored(backing32_.data(), w_ * 4, r);
        if (path_ == ColorPath::Fb) return isColored(rgb_.data(), w_ * 4, r);
        return false;
    }

    // The framebuffer path: the region's RGB over the gray X just drew, without a refresh.
    bool writeFb(Rect r) {
        packed_.resize((size_t)r.w * r.h * 3);
        packRgb(rgb_.data(), w_ * 4, r, packed_.data());
        // FBInk adds its view's origin (row 0: minus its row offset) to the position: taken off here.
        const int x = r.x + originX_ - fbView_.horiOrigin, y = r.y + originY_ - (fbView_.vertOrigin - fbView_.vertOffset);
        const int rc = fbink_print_raw_data(fb_, packed_.data(), r.w, r.h, packed_.size(), static_cast<short>(x),
                                            static_cast<short>(y), &rawCfg_);
        if (rc == -ENOSYS) {
            log(LogLevel::Warn, "color: FBInk was built without image support: no framebuffer path");
            fbRawOk_ = false;
            decideColor();
        } else if (rc < 0) {
            log(LogLevel::Warn, "color: writing %dx%d at (%d,%d) to the framebuffer failed: %d", r.w, r.h, x, y, rc);
        }
        return rc >= 0;
    }

    // X's default visual: a 32-bit image of byte-wide channels on a 24-bit TrueColor one (the X
    // path); else the 8-bit image as always.
    void readVisual(int scr) {
        Visual* v = DefaultVisual(dpy_, scr);
        const int depth = DefaultDepth(dpy_, scr);
        rmask_ = v->red_mask;
        gmask_ = v->green_mask;
        bmask_ = v->blue_mask;
        const bool trueColor = v->c_class == TrueColor;
        // Each channel a whole byte of the low three (RGB or BGR): what coloredNow reads.
        const auto r = static_cast<uint32_t>(rmask_), g = static_cast<uint32_t>(gmask_), b = static_cast<uint32_t>(bmask_);
        const bool byteChannels = (r | g | b) == 0xffffff && usableMasks(r, g, b) && std::popcount(r) == 8 &&
                                  std::popcount(g) == 8 && std::popcount(b) == 8;
        int bpp = 0, count = 0;
        if (XPixmapFormatValues* f = XListPixmapFormats(dpy_, &count)) {
            for (int i = 0; i < count; ++i)
                if (f[i].depth == depth) bpp = f[i].bits_per_pixel;
            XFree(f);
        }
        x32_ = depth >= 24 && trueColor && byteChannels && bpp == 32;
        std::string depths;
        int n = 0;
        if (int* ds = XListDepths(dpy_, scr, &n)) {
            for (int i = 0; i < n; ++i) depths += (i ? "," : "") + std::to_string(ds[i]);
            XFree(ds);
        }
        static const char* classes[] = {"StaticGray", "GrayScale", "StaticColor", "PseudoColor", "TrueColor", "DirectColor"};
        log(LogLevel::Info, "X: default visual depth %d (%d bpp), %s, masks %06lx/%06lx/%06lx; depths %s: %s image", depth, bpp,
            v->c_class >= 0 && v->c_class < 6 ? classes[v->c_class] : "?", static_cast<unsigned long>(rmask_),
            static_cast<unsigned long>(gmask_), static_cast<unsigned long>(bmask_), depths.c_str(), x32_ ? "32-bit color" : "8-bit gray");
        if (depth != 8 && !x32_) log(LogLevel::Warn, "X: a depth-%d visual the app can't draw in color: the 8-bit image as always", depth);
    }

    void makeImage() {
        if (image_) {
            image_->data = nullptr;
            XDestroyImage(image_);
        }
        const int scr = DefaultScreen(dpy_);
        if (x32_) {
            backing32_.assign((size_t)w_ * h_, rmask_ | gmask_ | bmask_);  // white
            image_ = XCreateImage(dpy_, DefaultVisual(dpy_, scr), DefaultDepth(dpy_, scr), ZPixmap, 0,
                                  reinterpret_cast<char*>(backing32_.data()), w_, h_, 32, w_ * 4);
            if (image_) image_->byte_order = std::endian::native == std::endian::little ? LSBFirst : MSBFirst;  // our uint32s
        } else {
            backing_.assign((size_t)w_ * h_, 0xff);
            image_ = XCreateImage(dpy_, DefaultVisual(dpy_, scr), 8, ZPixmap, 0, reinterpret_cast<char*>(backing_.data()), w_, h_,
                                  8, w_);
        }
        if (path_ == ColorPath::Fb) rgb_.assign((size_t)w_ * h_, 0xffffff);
        else rgb_ = {};
    }

    // The framebuffer path may write the window's pixels: FBInk is up, can write images, and its
    // view is turned as the window is (in landscape, if the framebuffer stays portrait, the window
    // isn't where FBInk would write: no framebuffer path then).
    bool fbWritable() const {
        if (fb_ < 0 || !fbRawOk_ || !fbView_.known) return false;
        const bool windowLandscape = originX_ + w_ > originY_ + h_;
        const bool viewLandscape = fbView_.width > fbView_.height;
        return windowLandscape == viewLandscape && originX_ + w_ <= fbView_.width && originY_ + h_ <= fbView_.height;
    }

    static bool envIs(const char* name, const char* value) {
        const char* v = std::getenv(name);
        return v && std::strcmp(v, value) == 0;
    }

    // Color caps and path from the panel, the setting, X, FBInk's view and the overrides; logged.
    void decideColor() {
        if (fb_ >= 0) {
            FBInkState state{};
            fbink_get_state(&cfg_, &state);
            fbView_ = {true, static_cast<int>(state.view_width), static_cast<int>(state.view_height), state.view_hori_origin,
                       state.view_vert_origin, state.view_vert_offset};
        }
        const char* zkColor = std::getenv("ZK_COLOR");
        const char* forced = std::getenv("ZK_COLOR_PATH");
        caps_ = decideColorCaps(panelFound_, colorSetting_, zkColor);
        path_ = chooseColorPath(caps_, x32_, fbWritable(), forced);
        if (caps_.on && path_ == ColorPath::Gray) caps_.on = false;  // no way to show it
        log(LogLevel::Info,
            "color: panel %s, setting %s, ZK_COLOR=%s ZK_COLOR_PATH=%s ZK_COLOR_WFM=%s; X %s, framebuffer %s "
            "(view %dx%d, origin %d,%d offset %d; window %dx%d at %d,%d) -> path %s, color %s",
            caps_.panel ? "yes" : "no", colorSetting_ ? "on" : "off", zkColor ? zkColor : "-", forced ? forced : "-",
            wfmAuto_ ? "auto" : "-", x32_ ? "32-bit" : "8-bit", fbWritable() ? "writable" : "not writable", fbView_.width,
            fbView_.height, fbView_.horiOrigin, fbView_.vertOrigin, fbView_.vertOffset, w_, h_, originX_, originY_,
            colorPathName(path_), caps_.on ? "on" : "off");
        if (caps_.panel && colorSetting_ && !x32_ && fb_ >= 0 && fbRawOk_ && !fbWritable())
            log(LogLevel::Warn, "color: the framebuffer's view (%dx%d) isn't turned as the window (%dx%d at %d,%d): gray here",
                fbView_.width, fbView_.height, w_, h_, originX_, originY_);
    }

    ::Display* dpy_ = nullptr;
    Window win_ = 0;
    GC gc_ = nullptr;
    XImage* image_ = nullptr;
    std::vector<uint8_t> backing_;     // the 8-bit image's pixels (an 8-bit X)
    std::vector<uint32_t> backing32_;  // the 32-bit image's (a 24-bit TrueColor X)
    std::vector<uint32_t> rgb_;        // the framebuffer path: the window's colors, 0x00RRGGBB
    std::vector<uint8_t> packed_;      // a region of rgb_ as FBInk takes it
    bool x32_ = false;
    unsigned long rmask_ = 0, gmask_ = 0, bmask_ = 0;
    bool panelFound_ = false;   // FBInk's has_color_panel
    bool colorSetting_ = true;  // Display → Color
    ColorCaps caps_;
    ColorPath path_ = ColorPath::Gray;
    bool fbRawOk_ = true;          // FBInk can write pixels (built with image support)
    bool wfmAuto_ = false;         // ZK_COLOR_WFM=auto
    bool waitBeforeNext_ = false;  // a colored refresh is on its way
    struct FbView {
        bool known = false;
        int width = 0, height = 0, horiOrigin = 0, vertOrigin = 0, vertOffset = 0;
    } fbView_;
    FBInkConfig rawCfg_;
    int w_ = 0, h_ = 0, originX_ = 0, originY_ = 0;
    int fb_ = -1;
    bool fingerDown_ = false;
    ExposePolicy expose_;
    bool background_ = false;
    Rect exposed_;
    FBInkConfig cfg_;
    std::string deviceName_ = "Kindle";
    int ppi_ = 0;
    std::unique_ptr<EvdevPen> pen_;
    std::string title_ = kWindowTitle;
    std::unique_ptr<WetInk> wet_;
    int penTurns_ = -1;
    int64_t lastTurnCheckMs_ = 0;
};

}  // namespace

std::unique_ptr<Display> makeDisplay() { return std::make_unique<X11Display>(); }

void displayEmergencyExit() { WetInk::emergencyOff(); }
void displayResetSystemInk() { WetInk::forceOff(); }

const char* fontDir() { return "/usr/java/lib/fonts"; }

}  // namespace zk
