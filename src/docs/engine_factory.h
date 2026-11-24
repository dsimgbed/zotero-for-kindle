#pragma once
// Loads an engine module at runtime: libzk-epub.so / libzk-pdf.so from $ZK_LIB_DIR, next to the
// executable, or in ../lib (the Kindle install keeps them in /mnt/us/zotero/lib).

#include <memory>
#include <string>
#include <vector>

#include "docs/document.h"
#include "docs/pdf_geometry.h"

namespace zk {

std::unique_ptr<DocumentEngine> loadEngine(DocType, const EngineEnv&, UserError*);

// Inspect per-page PDF geometry in points (width/height after /Rotate, normalized rotation,
// page label, user-space CropBox, and user-to-display affine matrix). Uses libzk-pdf.so's
// ABI-safe zk_pdf_inspect_pages export when available, falling back to DocumentEngine probing
// if forceFallback is true or an older libzk-pdf.so lacks the symbol.
bool inspectPdfPages(const std::string& path, const EngineEnv& env, std::vector<PdfPageGeometry>& out, UserError* err,
                     bool forceFallback = false);

// crengine's document cache closed (AccountSession::stop, once no engine of the account is left):
// nothing is written to the account's cache/cre after it, until an engine names a cache again.
// Nothing when the EPUB module was never loaded.
void closeEngineDocCache();

// The factory each module exports (extern "C"; libzk-epub.so also exports zk_epub_doc_cache_close).
using EngineCreateFn = DocumentEngine* (*)(const EngineEnv*);

}  // namespace zk
