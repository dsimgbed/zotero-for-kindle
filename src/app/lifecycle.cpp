#include "app/lifecycle.h"

#include "core/log.h"

namespace zk {

Lifecycle::Lifecycle(SyncControl& sync, SyncStatus& status, std::function<int()> interval, std::function<bool()> storage)
    : sync_(sync), status_(status), interval_(std::move(interval)), storage_(std::move(storage)) {}

void Lifecycle::attach(App& app) {
    app.setLifecycleHook([this, &app](bool foreground) {
        if (foreground) onForeground(app.nowMs());
        else onBackground();
    });
    app.setReadingHook([this](bool reading) { onReading(reading); });
    app.setWatchdog([this] { return watch(); }, kStorageCheckMs);
}

bool Lifecycle::watch() {
    if (!storage_()) {
        log(LogLevel::Warn, "app storage is gone (USB storage mode?): exiting");
        return false;
    }
    return true;
}

void Lifecycle::applyInterval() { sync_.setIntervalMinutes(background_ || reading_ ? 0 : interval_()); }

void Lifecycle::onBackground() {
    if (lock_) lock_();
    background_ = true;
    applyInterval();
    if (onBackgroundExtra_) onBackgroundExtra_();
}

void Lifecycle::onForeground(int64_t nowMs) {
    if (onForegroundExtra_) onForegroundExtra_();
    background_ = false;
    applyInterval();
    // Not periodic: the one sync that brings a woken Kindle up to date, reader in front or not.
    if (interval_() == 0) return;  // Off: no sync the reader didn't ask for
    if (status_.syncing() || (!status_.hasError() && status_.msSinceSync(nowMs) <= kResyncAfterMs)) return;
    sync_.syncWhenOnline();
}

void Lifecycle::onReading(bool reading) {
    reading_ = reading;
    applyInterval();
}

}  // namespace zk
