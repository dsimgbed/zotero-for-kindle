#pragma once
// Test builds only (ZK_OPEN_GUARD, set when the tests are built; never in the Kindle's build): the
// places that open an account's files — Db::open, the files sweep and the downloads into files/,
// the preview, full-text and crengine caches, Home's kept tree — name the path they open here, and
// a test's recorder (tests/open_guard.h) checks that none is outside the active account's folder
// (spec: multiple accounts, Isolation checks). Without a recorder a note costs a null check.

#include <string>

#ifdef ZK_OPEN_GUARD
#include <functional>

namespace zk::openguard {

// Called with every path noted (from any thread) until reset with {}.
void setRecorder(std::function<void(const std::string& path)>);
void note(const std::string& path);

}  // namespace zk::openguard

#define ZK_NOTE_OPEN(path) ::zk::openguard::note(path)
#else
#define ZK_NOTE_OPEN(path) ((void)0)
#endif
