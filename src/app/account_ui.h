#pragma once
// The account screens' side in the app (spec: multiple accounts, PIN and Screens): what Settings →
// Accounts, Home's chip and sheet, the PIN pad and Remove from this Kindle show and do, over the
// account list (accounts.json), the credentials and the switcher.
//
// The lock: the switcher asks unlock() before an account with a PIN opens (at startup nothing of it
// is opened before that); lockNow() — the Kindle going to sleep — covers whatever shows with the
// PIN pad (App::lock) when the open account has a PIN and Lock on sleep: the session goes on under
// it. On both, Use another account is a normal switch.
//
// Removal: an account that isn't open goes at once; the open one goes once the switch to another
// has opened that one (nothing of it runs then); the last one closes its session, goes, and
// leaves Setup (noAccount). Only accounts/<userId>/, its credentials and its entry are deleted.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "app/account_switch.h"
#include "app/pin.h"
#include "screens/account_sheet.h"
#include "screens/accounts.h"
#include "screens/pin_pad.h"

namespace zk {

class App;

struct AccountsUiDeps {
    Paths paths;
    std::string resetFile;                  // <USB>/zotero/reset-pin.txt
    std::function<int64_t()> wallMs;        // default: the system's (the backoff, "synced 10 min ago")
    std::function<bool()> online;           // default: the device's Wi-Fi
    // checkAccount over the network; `name`: the key's Zotero username.
    std::function<UserError(const Config&, std::string* name)> checkKey;
    // Whose a key is, asked of Zotero (checkKeyOwner): a typed reset key that may be the account's
    // new one. Default: checkKey.
    std::function<UserError(const Config&)> keyOwner;
    std::function<std::optional<Config>(int64_t userId)> credentials;  // default: loadCredentials
    // The open account's key changed (Update key, or a new key proving a forgotten PIN): its sync
    // starts again with it.
    std::function<void(const Config&)> keyChanged;
    // The last account was removed: Home's place goes to `setup` (Setup for a first account).
    std::function<void(std::unique_ptr<Screen> setup)> noAccount;
};

class AccountsUi final : public AccountsHost {
public:
    AccountsUi(App&, AccountSwitcher&, AccountsUiDeps);

    // AccountsHost
    std::vector<AccountSummary> accounts(int64_t measure = 0) override;
    int64_t wallMs() override { return deps_.wallMs(); }
    void switchTo(int64_t userId) override;
    void addAccount(Nav&) override;
    void updateKey(Nav&, int64_t userId) override;
    std::unique_ptr<Screen> pinPad(int64_t userId, PinPadScreen::Purpose, std::function<void(Nav&)> done) override;
    void setLockOnSleep(int64_t userId, bool on) override;
    bool online() override { return deps_.online(); }
    uint64_t sendNow() override;
    bool sent(uint64_t request) override;
    void remove(Nav&, int64_t userId) override;

    // The switcher's unlock: the PIN pad over everything until the PIN is right (true), or another
    // account is chosen (false, then the switch to that one).
    void unlock(int64_t userId, std::function<void(bool unlocked)> done);
    // Going to sleep: the PIN pad over what shows if the open account has a PIN and Lock on sleep.
    void lockNow();
    // main's onOpened, after the services started: a removal waiting for the switch away from the
    // account ends; an account just added syncs.
    void opened(AccountSession&);
    // A sync of the open account finished well: its summary (and when) kept in accounts.json.
    void onSyncEvent(const SyncEvent&);

    // Home: the chip (the open account's name with two accounts or more, else ""), its sheet, and
    // what the sheet's choices do.
    std::string chip();
    AccountSheet sheet();
    void sheetChoice(AccountSheet::Action, int64_t userId);
    // Setup for a first account (no account left; Add account's flow without a way back).
    std::unique_ptr<Screen> firstAccountSetup();

    AccountPins& pins() { return pins_; }

private:
    // `useAnother` (the lock's): Use another account, offering the other accounts.
    std::unique_ptr<PinPadScreen> pad(int64_t userId, PinPadScreen::Purpose, std::function<void(Nav&)> done,
                                      std::function<void(Nav&)> back, std::function<void(int64_t, Nav&)> useAnother = nullptr);
    std::unique_ptr<Screen> addSetup(bool canGoBack);
    void added(const Config&, const std::string& name);
    void keepKey(const Config&);
    std::vector<AccountSheet::Row> rowsExcept(int64_t userId);
    std::string nameOf(int64_t userId);
    int64_t openUserId() const;
    void finishRemoval();
    void showOpenLock();  // the open account's lock pad
    void backToOpen();    // a nested pad left: the open account's lock pad if locked, else what was there

    App& app_;
    AccountSwitcher& switcher_;
    AccountsUiDeps deps_;
    AccountPins pins_;
    int64_t removing_ = 0;  // the open account to remove once the switch away from it has opened another
    int64_t added_ = 0;     // an account just added: it syncs once open
    bool openLocked_ = false;  // the open account is locked (on sleep) until its right PIN
};

}  // namespace zk
