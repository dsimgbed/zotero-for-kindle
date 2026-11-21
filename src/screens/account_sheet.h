#pragma once
// The account sheet (spec: multiple accounts, Screens): Home's account chip opens it over Home; the
// PIN pad's Use another account opens it over the pad. One row per account — the active one marked
// with a dot, its summary on the right ("412 items · synced 10 min ago", "3 changes not sent") —
// a locked account's row shows its name and a lock, nothing else. Then + Add account (hidden at
// three accounts) and Manage accounts…; a tap outside the sheet closes it.

#include <cstdint>
#include <string>
#include <vector>

#include <cairo.h>

#include "core/geometry.h"
#include "ui/gesture.h"

namespace zk {

class AccountSheet {
public:
    struct Row {
        int64_t userId = 0;
        std::string name;
        bool active = false;
        bool locked = false;  // has a PIN and isn't open: its name and a lock only
        std::string line;     // the summary (unlocked accounts only)
    };
    enum class Action { None, Close, Switch, Add, Manage };

    AccountSheet(std::vector<Row> rows, bool canAdd, bool canManage, std::string title = "Accounts");
    void draw(cairo_t*, Rect area) const;
    Action onGesture(const Gesture&, Rect area);
    int64_t chosen() const { return chosen_; }  // after Switch: whose row was tapped

    const std::vector<Row>& rows() const { return rows_; }
    Rect sheetRect(Rect area) const;
    Rect rowRect(Rect area, size_t i) const;
    Rect addRect(Rect area) const;     // empty when hidden
    Rect manageRect(Rect area) const;  // empty when hidden

private:
    std::vector<Row> rows_;
    bool canAdd_, canManage_;
    std::string title_;
    int64_t chosen_ = 0;
};

}  // namespace zk
