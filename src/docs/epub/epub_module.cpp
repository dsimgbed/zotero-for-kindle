// libzk-epub.so's exported symbols: the engine's factory, and closing crengine's document cache.
#include "docs/engine_factory.h"
#include "docs/guarded_engine.h"
#include "docs/epub/epub_engine.h"

extern "C" __attribute__((visibility("default"))) zk::DocumentEngine* zk_epub_engine_create(const zk::EngineEnv* env) {
    try {
        return new zk::GuardedEngine(std::make_unique<zk::EpubEngine>(env ? *env : zk::EngineEnv{}));
    } catch (...) {
        return nullptr;  // nothing crosses the module boundary as an exception
    }
}

extern "C" __attribute__((visibility("default"))) void zk_epub_doc_cache_close() {
    try {
        zk::closeDocCache();
    } catch (...) {
    }
}
