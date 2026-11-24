#pragma once
// ABI-safe PDF page geometry inspection types.
// ZkPdfPageMetrics is a plain C POD struct safe to cross the libzk-pdf.so boundary
// (Kindle links static libstdc++ per shared module, so no C++ STL or exceptions cross it).

#include <array>
#include <cstddef>
#include <string>

extern "C" {

struct ZkPdfPageMetrics {
    int pageIndex;
    double widthPt;
    double heightPt;
    int rotation;            // normalized 0, 90, 180, 270
    char pageLabel[128];     // NUL-terminated logical label (or 1-based page number)
    double cropBox[4];       // [x0, y0, x1, y1] in PDF user space
    double userToPage[6];    // fz_matrix [a, b, c, d, e, f]: user space -> display page space
};

using ZkPdfInspectCb = void (*)(const ZkPdfPageMetrics*, void*);
using ZkPdfInspectFn = bool (*)(const char* path, ZkPdfInspectCb cb, void* ctx, char* errBuf, size_t errCap);

}  // extern "C"

namespace zk {

struct PdfPageGeometry {
    int pageIndex = 0;
    double widthPt = 0.0;
    double heightPt = 0.0;
    int rotation = 0;  // normalized 0, 90, 180, 270
    std::string pageLabel;
    std::array<double, 4> cropBox{0.0, 0.0, 0.0, 0.0};            // [x0, y0, x1, y1] in PDF user space
    std::array<double, 6> userToPage{1.0, 0.0, 0.0, -1.0, 0.0, 0.0};  // fz_matrix [a, b, c, d, e, f]
};

}  // namespace zk
