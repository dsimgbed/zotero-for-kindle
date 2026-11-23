#pragma once
// Reading fonts: the families found in the font directories (spec §3: v1 offers the fonts on
// the device; more fonts later = dropping files into /mnt/us/zotero/fonts).

#include <cstdlib>
#include <string>
#include <vector>

namespace zk {

// Families with an upright regular face, de-duplicated, Bookerly first, then alphabetical.
std::vector<std::string> readingFonts(const std::vector<std::string>& dirs);

// Where reading fonts are (the app, its EPUB engine and zk-sync).
inline std::vector<std::string> readingFontDirs() {
#ifdef ZK_KINDLE
    // The Kindle's own, the app's (Fast Font; replaced by updates), the user's own.
    const char* envHome = std::getenv("ZK_HOME");
    std::string home = (envHome && *envHome) ? std::string(envHome) : "/mnt/us/zotero";
    while (home.size() > 1 && home.back() == '/') home.pop_back();
    return {"/usr/java/lib/fonts", home + "/share/fonts", home + "/fonts"};
#else
    return {"assets/fonts-device", "assets/fonts-fast", "local/fonts"};
#endif
}

// Of the Kindle's system fonts (symbols, scripts, handwriting, CJK…) only its reading fonts are
// offered, as on the Kindle's own Display menu; fonts added to the app's folder all are.
bool offeredFamily(const std::string& family, bool systemFont);

// Fast Font (bundled): bold word beginnings; listed last, under "Fast reading", in this order.
inline const char* const kFastFonts[] = {"Fast Serif", "Fast Sans", "Fast Mono"};
inline bool isFastFont(const std::string& family) {
    for (const char* f : kFastFonts)
        if (family == f) return true;
    return false;
}

}  // namespace zk
