#pragma once
// "Version X is available" on Home: a check at most once an hour, after a sync that worked (Wi-Fi
// is up then; nothing turns it on for this), failed or not. Nothing downloads.
// Kept in the device's settings (device.json), so the notice outlives a restart and an account switch:
//   update:checkedMs  wall clock of the last check that answered (or of a manual one)
//   update:failedMs   wall clock of the last check that didn't
//   update:available  the newer version the last check found ("" when up to date)
//   update:dismissed  the version whose banner was closed with ×
// The UI thread only: poll() notes what the Updater's worker learned.
// Home asks for the banner on every loop step: poll() reads the settings only when the Updater has
// news, and available() is kept until poll() or dismiss() changes what it is made of.

#include "app/device_settings.h"
#include "update/updater.h"

#include <functional>
#include <optional>

namespace zk {

class UpdateNotice {
public:
    UpdateNotice(Updater&, std::function<int64_t()> wallMs);

    void onSyncFinished(DeviceSettings&, bool ok);  // starts a check when one is due
    bool poll(DeviceSettings&);                     // true when what the Updater found changed the notice
    std::optional<Version> available(DeviceSettings&) const;  // for the banner: newer and not dismissed
    std::optional<Version> pending(DeviceSettings&) const;    // newer, dismissed or not (Settings)
    void dismiss(DeviceSettings&);
    // Also run when the hourly check starts (the plugin catalog's check).
    void alsoCheck(std::function<void()> f) { alsoCheck_ = std::move(f); }

private:
    Updater& updater_;
    std::function<int64_t()> wallMs_;
    int serial_ = -1;  // the Updater status last noted
    // available()'s answer and the settings it is from; reset whenever the notice may have changed.
    mutable const DeviceSettings* cachedFor_ = nullptr;
    mutable std::optional<std::optional<Version>> cached_;
    std::function<void()> alsoCheck_;
};

}  // namespace zk
