#pragma once
// Loads an engine module at runtime: libzk-epub.so / libzk-pdf.so from $ZK_LIB_DIR, next to the
// executable, or in ../lib (the Kindle install keeps them in /mnt/us/zotero/lib).

#include <memory>

#include "docs/document.h"

namespace zk {

std::unique_ptr<DocumentEngine> loadEngine(DocType, const EngineEnv&, UserError*);

// crengine's document cache closed (AccountSession::stop, once no engine of the account is left):
// nothing is written to the account's cache/cre after it, until an engine names a cache again.
// Nothing when the EPUB module was never loaded.
void closeEngineDocCache();

// The factory each module exports (extern "C"; libzk-epub.so also exports zk_epub_doc_cache_close).
using EngineCreateFn = DocumentEngine* (*)(const EngineEnv*);

}  // namespace zk
