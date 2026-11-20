#include "update/update_notice.h"

#include "core/log.h"

namespace zk {

namespace {

constexpr int64_t kCheckEveryMs = 3600 * 1000LL;  // an hour: one small GET (GitHub allows 60 an hour)

int64_t metaMs(DeviceSettings& dev, const char* key) {
    const std::string v = dev.get(key).value_or("");
    try {
        return v.empty() ? 0 : std::stoll(v);
    } catch (...) {
        return 0;
    }
}

}  // namespace

UpdateNotice::UpdateNotice(Updater& updater, std::function<int64_t()> wallMs)
    : updater_(updater), wallMs_(std::move(wallMs)) {
    serial_ = updater_.status().serial;
}

void UpdateNotice::onSyncFinished(DeviceSettings& dev, bool ok) {
    if (!ok) return;
    using Phase = Updater::Status::Phase;
    const Updater::Status s = updater_.status();
    if (s.phase != Phase::Idle && s.phase != Phase::UpToDate && s.phase != Phase::Failed &&
        s.phase != Phase::Available && s.phase != Phase::NotInstallable)
        return;  // checking, or an update under way
    if (s.phase == Phase::Failed && s.mustRestart) return;
    const int64_t now = wallMs_();
    const int64_t checked = metaMs(dev, "update:checkedMs"), failed = metaMs(dev, "update:failedMs");
    if (now < checked || now < failed) {  // the clock went back: count from now
        dev.set("update:checkedMs", std::to_string(now));
        dev.set("update:failedMs", "");
        return;
    }
    if (checked && now - checked < kCheckEveryMs) return;
    if (failed && failed > checked && now - failed < kCheckEveryMs) return;
    log(LogLevel::Info, "update: the hourly check");
    updater_.check();
    if (alsoCheck_) alsoCheck_();  // the plugin catalog, with the app's own check
    serial_ = -1;  // note its answer even if the status looks the same
}

bool UpdateNotice::poll(DeviceSettings& dev) {
    using Phase = Updater::Status::Phase;
    const Updater::Status s = updater_.status();
    if (s.serial == serial_ || s.phase == Phase::Checking) return false;
    serial_ = s.serial;
    const auto before = available(dev);
    cached_.reset();  // written below
    const std::string now = std::to_string(wallMs_());
    switch (s.phase) {
        case Phase::UpToDate:
            dev.set("update:checkedMs", now);
            dev.set("update:available", "");
            break;
        case Phase::Available:
        case Phase::NotInstallable:
            dev.set("update:checkedMs", now);
            dev.set("update:available", s.release.version.str());
            break;
        case Phase::Failed:
            if (s.release.version == Version{}) dev.set("update:failedMs", now);  // the check itself
            break;
        default: return false;
    }
    return available(dev) != before;
}

std::optional<Version> UpdateNotice::pending(DeviceSettings& dev) const {
    const auto v = parseVersion(dev.get("update:available").value_or(""));
    if (!v || !(*v > updater_.current())) return std::nullopt;
    return v;
}

std::optional<Version> UpdateNotice::available(DeviceSettings& dev) const {
    if (cached_ && cachedFor_ == &dev) return *cached_;
    auto v = pending(dev);
    if (v && dev.get("update:dismissed").value_or("") == v->str()) v.reset();
    cached_ = v;
    cachedFor_ = &dev;
    return v;
}

void UpdateNotice::dismiss(DeviceSettings& dev) {
    cached_.reset();
    if (const auto v = pending(dev)) dev.set("update:dismissed", v->str());
}

}  // namespace zk
