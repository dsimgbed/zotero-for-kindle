#pragma once
// Color on Kindle Colorsoft (spec 2026-11-22-colorsoft-design.md): what the screen can do, how a
// region reaches it, and which waveform shows it. Pure functions; x11_display uses them on the
// device, the tests here.

#include <cstdint>
#include <string>

#include "core/geometry.h"

namespace zk {

class Canvas;
enum class Waveform;

// `panel`: the screen is a color one (FBInk's has_color_panel; ZK_COLOR=1 imitates one).
// `on`: color is shown (a color panel, Display → Color on, and a way to show it).
struct ColorCaps {
    bool panel = false;
    bool on = false;
};

// From what FBInk found, the Display → Color setting and ZK_COLOR (nullptr when unset):
// "0" is a gray Kindle whatever the panel; "1" a color panel even where none was found.
ColorCaps decideColorCaps(bool panelFound, bool settingOn, const char* zkColor);

// How color reaches the screen. Gray: today's 8-bit gray (color off). X: the window's image is
// 32-bit TrueColor and X writes the color. Fb: X keeps its 8-bit gray, and the region's RGB is
// written to the framebuffer (FBInk) just before the refresh.
enum class ColorPath { Gray, X, Fb };
const char* colorPathName(ColorPath);

// The path for `caps`: the X one when X can show 24-bit color, else the framebuffer's, else gray.
// `forced` is ZK_COLOR_PATH ("x" or "fb"; nullptr: unset), taken when that path is possible.
ColorPath chooseColorPath(const ColorCaps& caps, bool xTrueColor24, bool fbWritable, const char* forced);

// More than 0.5% of the pixels of `r` have chroma (max - min of R, G, B) above 24. `rgb24`: the
// pixel (0, 0) of 0x00RRGGBB pixels, `stride` bytes per row.
bool isColored(const uint32_t* rgb24, int stride, Rect r);

// FBInk waveforms on a color panel. Keep: the waveform the gray path uses (Fast, Ink).
// Auto: the driver's choice (ZK_COLOR_WFM=auto, and the device test).
enum class ColorWfm { Gl16, Glrc16, Gc16, Gcc16, Keep, Auto };
const char* colorWfmName(ColorWfm);
// Ui: GL16, or GLRC16 when colored; Quality and Full: GC16, or GCC16 when colored; else Keep.
ColorWfm colorWaveform(Waveform, bool colored);

// Writes `r` of the canvas as 32-bit pixels for the given channel masks (an X TrueColor visual's)
// into `dst`, whose pixel (0, 0) is the canvas's. Returns isColored for `r`, from the same pass.
bool toXrgb(const Canvas&, uint8_t* dst, int dstStride, Rect r, uint32_t rmask, uint32_t gmask, uint32_t bmask);
// The same pixels, each R = G = B = the luma toGray writes (color off on a 32-bit X visual).
void toXgray(const Canvas&, uint8_t* dst, int dstStride, Rect r, uint32_t rmask, uint32_t gmask, uint32_t bmask);
// `r` of 0x00RRGGBB pixels (pixel (0, 0) at `rgb24`, `stride` bytes per row) as packed R, G, B
// bytes, row after row (r.w * r.h * 3): what fbink_print_raw_data takes.
void packRgb(const uint32_t* rgb24, int stride, Rect r, uint8_t* out);

// The framebuffer path writes past X's clipping: only where nothing can be over the window. In
// front, not covered at all (VisibilityUnobscured), and without the system keyboard up.
struct FbWriteState {
    bool foreground = true;
    bool unobscured = false;
    bool keyboardUp = false;
};
bool fbColorAllowed(const FbWriteState&);

// The state as it changes; closed until X says the window is unobscured. Each setter returns true
// when that change opened the gate again: the window is then shown once more in color (what was
// presented meanwhile went out gray).
class FbColorGate {
public:
    bool setForeground(bool on) { return change(s_.foreground, on); }
    bool setUnobscured(bool on) { return change(s_.unobscured, on); }
    bool setKeyboardUp(bool on) { return change(s_.keyboardUp, on); }
    bool allowed() const { return fbColorAllowed(s_); }

private:
    bool change(bool& field, bool on) {
        const bool before = allowed();
        field = on;
        return !before && allowed();
    }
    FbWriteState s_;
};

// The masks fit toXrgb: each channel's bits contiguous, 1 to 8 of them, not overlapping.
bool usableMasks(uint32_t rmask, uint32_t gmask, uint32_t bmask);

}  // namespace zk
