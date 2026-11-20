#include "ui/metrics.h"

#include <cctype>
#include <mutex>

namespace zk {

namespace {
Metrics g_metrics;  // set at start and on a rotation, on the UI thread
std::mutex g_mu;    // for metricsSnapshot from other threads; the UI thread's reads need none
}  // namespace

const Metrics& metrics() { return g_metrics; }
void setMetrics(const Metrics& m) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_metrics = m;
}
Metrics metricsSnapshot() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_metrics;
}

std::string deviceModel(const std::string& name) {
    if (name == kPw5.device) return "pw5";
    if (name == kKt4.device) return "kt4";
    if (name == kScribe.device) return "scribe";
    std::string out;
    for (unsigned char c : name)
        if (!std::isspace(c)) out += char(std::tolower(c));
    return out;
}

}  // namespace zk
