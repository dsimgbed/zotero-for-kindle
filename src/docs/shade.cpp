#include <algorithm>
#include <cctype>
#include <string>

#include "docs/document.h"

namespace zk {

uint8_t shadeOf(const std::string& color) {
    std::string c = color;
    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
    // Zotero's annotation palette, as shades that keep black text readable on e-ink.
    if (c == "#ffd400") return 220;  // yellow
    if (c == "#ff6666") return 170;  // red
    if (c == "#5fb236") return 190;  // green
    if (c == "#2ea8e5") return 180;  // blue
    if (c == "#a28ae5") return 160;  // purple
    if (c == "#e56eee") return 200;  // magenta
    if (c == "#f19837") return 210;  // orange
    return 200;                      // gray #aaaaaa and anything else
}

uint32_t zoteroRgb(const std::string& color) {
    if (color.size() != 7 || color[0] != '#' ||
        !std::all_of(color.begin() + 1, color.end(), [](unsigned char ch) { return std::isxdigit(ch); }))
        return 0xaaaaaa;  // Zotero's gray
    return static_cast<uint32_t>(std::stoul(color.substr(1), nullptr, 16));
}

uint32_t highlightTint(const std::string& color) {
    const uint32_t c = zoteroRgb(color);
    uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        const double v = 0.45 * ((c >> shift) & 0xff) + 0.55 * 255;
        out |= static_cast<uint32_t>(v + 0.5) << shift;
    }
    return out;
}

}  // namespace zk
