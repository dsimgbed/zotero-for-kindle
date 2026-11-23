#include "app/paths.h"

#include <cstdlib>

namespace zk {

#ifdef ZK_KINDLE
namespace {
std::string envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    std::string s(v);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
}  // namespace
#endif

Paths defaultPaths() {
#ifdef ZK_KINDLE
    const std::string home = envOr("ZK_HOME", "/mnt/us/zotero");
    const std::string cfg = envOr("ZK_CONFIG_DIR", "/var/local/zotero");
    const std::string launcher = envOr("ZK_LAUNCHER", "/mnt/us/documents/Zotero.sh");
    const char* noUpd = std::getenv("ZK_NO_UPDATES");
    const std::string appDir = (noUpd && std::string(noUpd) == "1") ? "" : home;
    return {cfg, home + "/config.json", home + "/zotero.db", home + "/files",
            home + "/log", appDir, launcher, home + "/plugins", home + "/cache",
            home + "/state", home + "/accounts", home + "/device.json"};
#else
    return {"local", "", "local/zk.sqlite", "local/files", "local", "", "", "local/plugins", "local/cache", "local/state", "local/accounts", "local/device.json"};
#endif
}

}  // namespace zk
