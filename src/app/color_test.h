#pragma once
// The Colorsoft device test (`zotero --color-test`, docs/colorsoft-test.md): a card of color bars,
// a gray ramp and Zotero's highlight colors, shown through every way color can reach the screen
// with every candidate waveform, each step logged and left up 3 s for a photo.

#include <functional>
#include <string>

#include "core/geometry.h"

namespace zk {

class Canvas;
class Display;

// The card, white, with `caption` (which path and waveform) on top.
void drawColorTestCard(Canvas&, const std::string& caption);
Rect colorTestBarsRect(const Canvas&);  // red, green, blue, cyan, magenta, yellow, black, white
Rect colorTestRampRect(const Canvas&);  // 16 steps, black at the left

// Gray with GC16 first (today's picture), then for each of the display's color paths GL16,
// GLRC16, GC16, GCC16 and AUTO; every card after a flashing white screen. `wait` sleeps (ms).
int runColorTest(Display&, const std::function<void(int ms)>& wait);

}  // namespace zk
