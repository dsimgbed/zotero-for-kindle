#pragma once
// Background/foreground and storage watch (spec §7.8): while the Kindle sleeps no periodic sync
// runs, nor while the reader is in front ("rests while you read": edits still go out, and Sync
// taps still sync); on wake the screen gets a full refresh and, with Wi-Fi up, a sync if the last
// one is older than 2 minutes (or failed) — the worker waits for Wi-Fi. With Sync every Off there
// is no sync on wake either. If the app's storage goes away (USB storage mode), the app quits.

#include <functional>

#include "app/app.h"
#include "screens/sync_status.h"

namespace zk {

class Lifecycle {
public:
    static constexpr int64_t kResyncAfterMs = 2 * 60'000;
    static constexpr int64_t kStorageCheckMs = 5'000;

    Lifecycle(SyncControl&, SyncStatus&, std::function<int()> intervalMinutes, std::function<bool()> storageOk);
    void attach(App&);  // installs the hooks; the App must outlive nothing beyond this object

    // Also called when the Kindle goes to the background (plugins' background tasks stop).
    void setOnBackground(std::function<void()> f) { onBackgroundExtra_ = std::move(f); }
    void setOnForeground(std::function<void()> f) { onForegroundExtra_ = std::move(f); }
    // Lock on sleep (spec: multiple accounts, PIN), on real sleep only — not a system dialog or the
    // keyboard: asked as the app goes to the background when `asleep` (the screensaver is up), so
    // the screen the Kindle wakes to (repainted from what was last drawn) is the PIN pad; and, as a
    // fallback, on coming back when `suspendedMs` grew meanwhile (the device suspended though the
    // screensaver wasn't seen going up).
    void setLock(std::function<void()> lock, std::function<bool()> asleep, std::function<int64_t()> suspendedMs = {}) {
        lock_ = std::move(lock);
        asleep_ = std::move(asleep);
        suspendedMs_ = std::move(suspendedMs);
    }
    void onBackground();
    void onForeground(int64_t nowMs);
    void onReading(bool reading);  // the reader (or one of its panels) shows, or stopped showing
    bool watch();  // every few seconds while in front, and on the way back: is the app's storage still there?

private:
    SyncControl& sync_;
    SyncStatus& status_;
    std::function<int()> interval_;
    std::function<bool()> storage_;
    std::function<void()> onBackgroundExtra_;
    std::function<void()> onForegroundExtra_;
    std::function<void()> lock_;
    std::function<bool()> asleep_;
    std::function<int64_t()> suspendedMs_;
    int64_t suspendedAtBackground_ = 0;
    bool background_ = false, reading_ = false;
    void applyInterval();  // the setting's, unless asleep or reading (then none)
};

}  // namespace zk
