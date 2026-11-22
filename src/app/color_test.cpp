#include "app/color_test.h"

#include <chrono>
#include <cstdio>
#include <vector>

#include "core/log.h"
#include "platform/display.h"
#include "screens/annotation_bar.h"
#include "ui/canvas.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace zk {

namespace {

constexpr int kPad = 60;

void rgb(cairo_t* cr, uint32_t c) {
    cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

void fill(cairo_t* cr, Rect r, uint32_t c) {
    rgb(cr, c);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
}

uint32_t parseHex(const std::string& s) { return static_cast<uint32_t>(std::stoul(s.substr(1), nullptr, 16)); }

// Over white at 45%: the tint the reader gives highlights on color panels (spec, Highlights).
uint32_t tint(uint32_t c) {
    uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        const double v = 0.45 * ((c >> shift) & 0xff) + 0.55 * 255;
        out |= static_cast<uint32_t>(v + 0.5) << shift;
    }
    return out;
}

Rect band(const Canvas& c, int top, int h) { return {kPad, top, c.width() - 2 * kPad, h}; }

}  // namespace

Rect colorTestBarsRect(const Canvas& c) { return band(c, 220, 220); }
Rect colorTestRampRect(const Canvas& c) { return band(c, 500, 140); }

void drawColorTestCard(Canvas& canvas, const std::string& caption) {
    cairo_t* cr = canvas.cr();
    fill(cr, {0, 0, canvas.width(), canvas.height()}, 0xffffff);
    const TextStyle title{theme::kSans, 11, 700}, label{theme::kSans, 7, 500};
    drawText(cr, caption, title, band(canvas, 60, 120), 1);

    const Rect bars = colorTestBarsRect(canvas);
    const uint32_t barColors[] = {0xff0000, 0x00ff00, 0x0000ff, 0x00ffff, 0xff00ff, 0xffff00, 0x000000, 0xffffff};
    for (int i = 0; i < 8; ++i) {
        const int x0 = bars.x + bars.w * i / 8, x1 = bars.x + bars.w * (i + 1) / 8;
        fill(cr, {x0, bars.y, x1 - x0, bars.h}, barColors[i]);
    }
    gray(cr, 0);
    cairo_set_line_width(cr, 2);
    cairo_rectangle(cr, bars.x - 1, bars.y - 1, bars.w + 2, bars.h + 2);
    cairo_stroke(cr);

    const Rect ramp = colorTestRampRect(canvas);
    for (int i = 0; i < 16; ++i) {
        const int x0 = ramp.x + ramp.w * i / 16, x1 = ramp.x + ramp.w * (i + 1) / 16;
        const uint32_t g = static_cast<uint32_t>(i * 17);
        fill(cr, {x0, ramp.y, x1 - x0, ramp.h}, (g << 16) | (g << 8) | g);
    }
    gray(cr, 0);
    cairo_rectangle(cr, ramp.x - 1, ramp.y - 1, ramp.w + 2, ramp.h + 2);
    cairo_stroke(cr);

    // A hue sweep: how smoothly the panel's filter and the driver's dithering render a gradient.
    const Rect hue = band(canvas, 680, 100);
    cairo_pattern_t* sweep = cairo_pattern_create_linear(hue.x, 0, hue.x + hue.w, 0);
    const uint32_t stops[] = {0xff0000, 0xffff00, 0x00ff00, 0x00ffff, 0x0000ff, 0xff00ff, 0xff0000};
    for (int i = 0; i < 7; ++i)
        cairo_pattern_add_color_stop_rgb(sweep, i / 6.0, ((stops[i] >> 16) & 0xff) / 255.0, ((stops[i] >> 8) & 0xff) / 255.0,
                                         (stops[i] & 0xff) / 255.0);
    cairo_set_source(cr, sweep);
    cairo_rectangle(cr, hue.x, hue.y, hue.w, hue.h);
    cairo_fill(cr);
    cairo_pattern_destroy(sweep);

    // Zotero's highlight colors: the color itself, then black text on its highlight tint.
    const auto& colours = zoteroColours();
    const int top = 840, rowH = std::max(60, (canvas.height() - top - kPad) / 8);
    for (size_t i = 0; i < colours.size(); ++i) {
        const int y = top + static_cast<int>(i) * rowH;
        const uint32_t c = parseHex(colours[i].hex);
        fill(cr, {kPad, y + 6, 160, rowH - 12}, c);
        fill(cr, {kPad + 190, y + 6, canvas.width() - 2 * kPad - 190, rowH - 12}, tint(c));
        centredText(cr, std::string(colours[i].name) + "  " + colours[i].hex + "  highlighted text", label,
                    {kPad + 210, y, canvas.width() - 2 * kPad - 230, rowH}, Align::Left);
    }
    cairo_surface_flush(canvas.surface());
}

int runColorTest(Display& d, const std::function<void(int ms)>& wait) {
    struct Step {
        ColorPath path;
        ColorWfm wfm;
    };
    std::vector<Step> steps{{ColorPath::Gray, ColorWfm::Gc16}};
    for (ColorPath p : d.colorPaths())
        for (ColorWfm w : {ColorWfm::Gl16, ColorWfm::Glrc16, ColorWfm::Gc16, ColorWfm::Gcc16, ColorWfm::Auto}) steps.push_back({p, w});
    const ColorCaps caps = d.colorCaps();
    log(LogLevel::Info, "color test: %zu steps, color panel %s, %dx%d", steps.size(), caps.panel ? "yes" : "no", d.width(),
        d.height());
    Canvas canvas(d.width(), d.height());
    const Rect all{0, 0, d.width(), d.height()};
    for (size_t i = 0; i < steps.size(); ++i) {
        const Step& s = steps[i];
        fill(canvas.cr(), all, 0xffffff);
        d.present(canvas, all, Waveform::Full);  // a flash to white: this step starts clean
        char caption[96];
        std::snprintf(caption, sizeof caption, "%zu/%zu  path %s  waveform %s", i + 1, steps.size(), colorPathName(s.path),
                      colorWfmName(s.wfm));
        drawColorTestCard(canvas, caption);
        const auto t0 = std::chrono::steady_clock::now();
        const bool shown = d.presentWith(canvas, all, s.path, s.wfm);
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        log(shown ? LogLevel::Info : LogLevel::Warn, "color test %s: %s (%lld ms)", caption, shown ? "shown" : "NOT shown", ms);
        wait(3000);
    }
    log(LogLevel::Info, "color test: done");
    return 0;
}

}  // namespace zk
