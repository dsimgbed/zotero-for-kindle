#include "docs/engine_factory.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "core/log.h"
#include "core/open_guard.h"

namespace zk {

namespace fs = std::filesystem;

namespace {

fs::path exeDir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::path(buf).parent_path();
    return ".";
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path(".") : p.parent_path();
#endif
}

// The module's folders, in the order they are looked in.
std::vector<fs::path> moduleDirs() {
    std::vector<fs::path> dirs;
    if (const char* d = std::getenv("ZK_LIB_DIR")) dirs.emplace_back(d);
    const fs::path exe = exeDir();
    dirs.push_back(exe);
    dirs.push_back(exe / ".." / "lib");
    dirs.push_back(exe / "..");
    return dirs;
}

double cleanRound2(double v) {
    const double r = std::round(v * 100.0) / 100.0;
    return r == 0.0 ? 0.0 : r;
}

void collectPdfPage(const ZkPdfPageMetrics* m, void* ctx) {
    if (!m || !ctx) return;
    auto* pages = static_cast<std::vector<PdfPageGeometry>*>(ctx);
    PdfPageGeometry g;
    g.pageIndex = m->pageIndex;
    g.widthPt = m->widthPt;
    g.heightPt = m->heightPt;
    g.rotation = m->rotation;
    g.pageLabel = m->pageLabel;
    for (size_t i = 0; i < 4; ++i) g.cropBox[i] = m->cropBox[i];
    for (size_t i = 0; i < 6; ++i) g.userToPage[i] = m->userToPage[i];
    pages->push_back(std::move(g));
}

bool inspectPdfPagesFallback(const std::string& path, const EngineEnv& env, std::vector<PdfPageGeometry>& out,
                             UserError* err) {
    auto engine = loadEngine(DocType::Pdf, env, err);
    if (!engine) return false;
    if (!engine->open(path, err)) return false;

    // High-resolution virtual layout (never rendered) so pageBox integer rounding stays < 0.001 pt.
    constexpr int kProbeDim = 1000000;
    Layout l;
    l.width = kProbeDim;
    l.height = kProbeDim;
    l.trimMargins = false;
    l.columnView = false;
    engine->setLayout(l);

    const int n = engine->pageCount();
    if (n <= 0) {
        if (err) *err = UserError{ErrorKind::Other, "This PDF is damaged and can't be opened.", "no pages"};
        return false;
    }

    std::vector<PdfPageGeometry> pages;
    pages.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Rect box = engine->pageBox(i);
        if (box.w <= 0 || box.h <= 0) {
            if (err) *err = UserError{ErrorKind::Other, "Can't inspect PDF page bounds.", "empty pageBox"};
            return false;
        }
        const std::vector<std::vector<Point>> probe = {
            {{box.x, box.y}, {box.x + box.w, box.y}, {box.x, box.y + box.h}}};
        const auto placed = engine->inkPosition(i, probe, kProbeDim);
        if (!placed) {
            if (err) *err = UserError{ErrorKind::Other, "Can't inspect PDF page transform.", "inkPosition failed"};
            return false;
        }
        const auto pos = nlohmann::json::parse(placed->position, nullptr, false);
        if (!pos.is_object() || !pos.contains("paths") || !pos["paths"].is_array() || pos["paths"].empty() ||
            !pos["paths"][0].is_array() || pos["paths"][0].size() < 6 || !pos.contains("width") ||
            !pos["width"].is_number()) {
            if (err) *err = UserError{ErrorKind::Other, "Can't inspect PDF page transform.", "malformed probe position"};
            return false;
        }
        const auto& pts = pos["paths"][0];
        const double u0x = pts[0].get<double>(), u0y = pts[1].get<double>();
        const double u1x = pts[2].get<double>(), u1y = pts[3].get<double>();
        const double u2x = pts[4].get<double>(), u2y = pts[5].get<double>();
        const double widthRes = pos["width"].get<double>();
        if (!std::isfinite(widthRes) || widthRes <= 0.0) {
            if (err) *err = UserError{ErrorKind::Other, "Can't inspect PDF page scale.", "invalid probe width"};
            return false;
        }

        const double scale = static_cast<double>(kProbeDim) / widthRes;
        const double W = static_cast<double>(box.w) / scale;
        const double H = static_cast<double>(box.h) / scale;
        if (!(W > 0.0) || !(H > 0.0)) {
            if (err) *err = UserError{ErrorKind::Other, "Can't inspect PDF page size.", "non-positive dimensions"};
            return false;
        }

        const double dxX = u1x - u0x, dxY = u1y - u0y;
        const double dyX = u2x - u0x, dyY = u2y - u0y;

        // pageToUser maps display (q_x, q_y) -> PDF user (u_x, u_y):
        //   u_x = A * q_x + C * q_y + E
        //   u_y = B * q_x + D * q_y + F
        const double A = dxX / W;
        const double B = dxY / W;
        const double C = dyX / H;
        const double D = dyY / H;
        const double E = u0x;
        const double F = u0y;
        const double det = A * D - B * C;
        if (!std::isfinite(det) || std::abs(det) < 1e-12) {
            if (err) *err = UserError{ErrorKind::Other, "Can't inspect PDF page transform.", "singular transform"};
            return false;
        }

        const double a = D / det;
        const double b = -B / det;
        const double c = -C / det;
        const double d = A / det;
        const double e = -(a * E + c * F);
        const double f = -(b * E + d * F);

        int rot = 0;
        if (std::abs(dxX) >= std::abs(dxY)) {
            rot = dxX >= 0.0 ? 0 : 180;
        } else {
            rot = dxY >= 0.0 ? 90 : 270;
        }

        const double u3x = u1x + u2x - u0x;
        const double u3y = u1y + u2y - u0y;
        const double minX = std::min({u0x, u1x, u2x, u3x});
        const double minY = std::min({u0y, u1y, u2y, u3y});
        const double maxX = std::max({u0x, u1x, u2x, u3x});
        const double maxY = std::max({u0y, u1y, u2y, u3y});

        PdfPageGeometry g;
        g.pageIndex = i;
        g.widthPt = cleanRound2(W);
        g.heightPt = cleanRound2(H);
        g.rotation = rot;
        g.pageLabel = placed->pageLabel.empty() ? std::to_string(i + 1) : placed->pageLabel;
        g.cropBox = {cleanRound2(minX), cleanRound2(minY), cleanRound2(maxX), cleanRound2(maxY)};
        g.userToPage = {cleanRound2(a), cleanRound2(b), cleanRound2(c),
                        cleanRound2(d), cleanRound2(e), cleanRound2(f)};
        pages.push_back(std::move(g));
    }

    out = std::move(pages);
    return true;
}

}  // namespace

void closeEngineDocCache() {
    for (const auto& d : moduleDirs()) {
        const fs::path p = d / "libzk-epub.so";
        // Only a module already loaded: with none, no engine ever opened the cache.
        void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        if (!h) continue;
        if (auto close = reinterpret_cast<void (*)()>(dlsym(h, "zk_epub_doc_cache_close"))) close();
        dlclose(h);  // (RTLD_NOLOAD took a reference)
    }
}

std::unique_ptr<DocumentEngine> loadEngine(DocType type, const EngineEnv& env, UserError* err) {
    if (!env.cacheDir.empty()) ZK_NOTE_OPEN(env.cacheDir);  // crengine's document cache
    const char* name = type == DocType::Epub ? "libzk-epub.so" : "libzk-pdf.so";
    const char* symbol = type == DocType::Epub ? "zk_epub_engine_create" : "zk_pdf_engine_create";
    const std::vector<fs::path> dirs = moduleDirs();
    std::string why = std::string(name) + " not found";
    for (const auto& d : dirs) {
        const fs::path p = d / name;
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        // Loaded once and kept for the process lifetime (engines may be created again).
        void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            why = dlerror();
            continue;
        }
        auto create = reinterpret_cast<EngineCreateFn>(dlsym(h, symbol));
        if (!create) {
            why = std::string(symbol) + " missing in " + p.string();
            continue;
        }
        if (DocumentEngine* e = create(&env)) return std::unique_ptr<DocumentEngine>(e);
        why = std::string(symbol) + " returned nothing";
    }
    log(LogLevel::Error, "engine: %s", why.c_str());
    if (err)
        *err = UserError{ErrorKind::Other,
                         type == DocType::Epub ? "The EPUB reader is missing from this install."
                                               : "The PDF reader is missing from this install.",
                         why};
    return nullptr;
}

bool inspectPdfPages(const std::string& path, const EngineEnv& env, std::vector<PdfPageGeometry>& out, UserError* err,
                     bool forceFallback) {
    out.clear();
    if (!forceFallback) {
        for (const auto& d : moduleDirs()) {
            const fs::path p = d / "libzk-pdf.so";
            std::error_code ec;
            if (!fs::exists(p, ec)) continue;
            void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) continue;
            auto inspect = reinterpret_cast<ZkPdfInspectFn>(dlsym(h, "zk_pdf_inspect_pages"));
            if (!inspect) continue;  // older libzk-pdf.so without the symbol -> fallback below
            std::vector<PdfPageGeometry> pages;
            char errBuf[512] = "";
            if (!inspect(path.c_str(), &collectPdfPage, &pages, errBuf, sizeof errBuf)) {
                if (err)
                    *err = UserError{ErrorKind::Other, "Can't inspect PDF pages.",
                                     errBuf[0] ? std::string(errBuf) : std::string("PDF inspection failed")};
                return false;
            }
            out = std::move(pages);
            return true;
        }
    }
    return inspectPdfPagesFallback(path, env, out, err);
}

}  // namespace zk
