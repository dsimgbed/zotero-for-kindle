#!/bin/sh
# Automated POSIX-sh desktop E2E test harness for GrimoireZotero.sh
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TRAMP="$SCRIPT_DIR/GrimoireZotero.sh"
PASS=0
FAIL=0

ok() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

run_test() { echo "=== $1 ==="; }

# helper to make temp bridge
make_tmp() {
    _t=$(mktemp -d /tmp/opencode/trampXXXXXX)
    echo "$_t"
}


run_tramp() {
  (
    export GZ_BRIDGE_DIR="$BRIDGE"
    export GZ_GRIMOIRE_CMD="$MOCKBIN/grimoire"
    export GZ_ZOTERO_CMD="$MOCKBIN/zotero"
    export GZ_IS_KINDLE=0
    export GZ_SWITCH_SETTLE_SEC=0
    export MOCK_STATE="$STATE"
    export BRIDGE="$BRIDGE"
    "$TRAMP" "$@"
  )
}

# --- Test 1: full round trip ---
run_test "Test1 round trip"
T=$(make_tmp)
BRIDGE="$T/bridge"
mkdir -p "$BRIDGE/inbox"
STATE="$T/state"
mkdir -p "$STATE"
MOCKBIN="$T/bin"
mkdir -p "$MOCKBIN"

cat > "$MOCKBIN/grimoire" <<'EOF'
#!/bin/sh
STATE="$MOCK_STATE"
c=$(cat "$STATE/gc" 2>/dev/null || echo 0)
c=$((c+1))
echo $c > "$STATE/gc"
if [ "$c" = 1 ]; then
  cat > "$BRIDGE/handoff.json" <<'JSON'
{"schema_version":1,"nonce":"g1","created_at":1,"source":"grimoire","target":"zotero","action":"open_item","zotero":{"item_key":"ABCD1234"},"grimoire":{"notebook_id":"nb1","page_id":7,"page_index":3},"return_to":{"target":"grimoire","notebook_id":"nb1","page_id":7,"page_index":3}}
JSON
  exit 42
else
  found=0
  for f in "$BRIDGE"/inbox/*.json; do
    [ -f "$f" ] || continue
    if grep -q '"item_key":"ABCD1234"' "$f" || grep -q '"item_key": "ABCD1234"' "$f"; then found=1; fi
  done
  [ "$found" = 1 ] || exit 10
  if [ -f "$BRIDGE/handoff.json" ]; then
    if grep -Eq '"target"[[:space:]]*:[[:space:]]*"grimoire"' "$BRIDGE/handoff.json"; then
      rm -f "$BRIDGE/handoff.json"
    else
      exit 11
    fi
  else
    exit 12
  fi
  exit 0
fi
EOF
chmod +x "$MOCKBIN/grimoire"

cat > "$MOCKBIN/zotero" <<'EOF'
#!/bin/sh
STATE="$MOCK_STATE"
c=$(cat "$STATE/zc" 2>/dev/null || echo 0)
c=$((c+1))
echo $c > "$STATE/zc"
if [ "$1" != "--handoff" ]; then exit 20; fi
[ -f "$BRIDGE/handoff.json" ] || exit 21
rm -f "$BRIDGE/handoff.json"
mkdir -p "$BRIDGE/inbox"
cat > "$BRIDGE/inbox/123-abc.json" <<'JSON'
{"schema_version":1,"id":"123-abc","created_at":1,"item_key":"ABCD1234","attachment_key":"EFGH5678","short_cite":"Kuhn 1962, p. 42","title":"T","authors":"K","year":"1962","page":42,"locator":"","quote":"","note":""}
JSON
cat > "$BRIDGE/handoff.json" <<'JSON'
{"schema_version":1,"nonce":"z1","created_at":2,"source":"zotero","target":"grimoire","action":"return_essay","grimoire":{"notebook_id":"nb1","page_id":7,"page_index":3}}
JSON
exit 43
EOF
chmod +x "$MOCKBIN/zotero"

run_tramp >/tmp/opencode/t1.log 2>&1
rc=$?
if [ "$rc" = 0 ] && [ ! -f "$BRIDGE/handoff.json" ]; then
    ok "Test1 round trip (rc=$rc, handoff consumed)"
else
    fail "Test1 round trip (rc=$rc, handoff exists=$([ -f "$BRIDGE/handoff.json" ] && echo yes || echo no))"
    cat /tmp/opencode/t1.log
fi

# --- Test 2: self-update 75 ---
run_test "Test2 self-update 75"
T=$(make_tmp)
BRIDGE="$T/bridge"; mkdir -p "$BRIDGE"
STATE="$T/state"; mkdir -p "$STATE"
MOCKBIN="$T/bin"; mkdir -p "$MOCKBIN"
cat > "$MOCKBIN/zotero" <<'EOF'
#!/bin/sh
STATE="$MOCK_STATE"
c=$(cat "$STATE/zc" 2>/dev/null || echo 0)
c=$((c+1))
echo $c > "$STATE/zc"
if [ "$c" = 1 ]; then exit 75; fi
exit 0
EOF
chmod +x "$MOCKBIN/zotero"
cat > "$MOCKBIN/grimoire" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$MOCKBIN/grimoire"

run_tramp --start zotero >/tmp/opencode/t2.log 2>&1
rc=$?
if [ "$rc" = 0 ]; then ok "Test2 self-update (rc=$rc)"; else fail "Test2 self-update (rc=$rc)"; cat /tmp/opencode/t2.log; fi

# --- Test 3: crash loop circuit breaker ---
run_test "Test3 crash loop"
T=$(make_tmp)
BRIDGE="$T/bridge"; mkdir -p "$BRIDGE"
STATE="$T/state"; mkdir -p "$STATE"
MOCKBIN="$T/bin"; mkdir -p "$MOCKBIN"
cat > "$MOCKBIN/grimoire" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$MOCKBIN/grimoire"
cat > "$MOCKBIN/zotero" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$MOCKBIN/zotero"
# place a handoff.json
echo '{"schema_version":1,"target":"grimoire"}' > "$BRIDGE/handoff.json"

run_tramp >/tmp/opencode/t3.log 2>&1
rc=$?
if [ "$rc" != 0 ] && [ ! -f "$BRIDGE/handoff.json" ] && grep -qi 'circuit' "$BRIDGE/bridge.log"; then
    ok "Test3 crash loop (rc=$rc, handoff cleared, log ok)"
else
    fail "Test3 crash loop (rc=$rc, handoff=$([ -f "$BRIDGE/handoff.json" ] && echo exists || echo gone), log=$(cat "$BRIDGE/bridge.log" 2>/dev/null))"
    cat /tmp/opencode/t3.log
fi

# --- Test 4: bounce storm ---
run_test "Test4 bounce storm"
T=$(make_tmp)
BRIDGE="$T/bridge"; mkdir -p "$BRIDGE"
STATE="$T/state"; mkdir -p "$STATE"
MOCKBIN="$T/bin"; mkdir -p "$MOCKBIN"
cat > "$MOCKBIN/grimoire" <<'EOF'
#!/bin/sh
exit 42
EOF
chmod +x "$MOCKBIN/grimoire"
cat > "$MOCKBIN/zotero" <<'EOF'
#!/bin/sh
exit 43
EOF
chmod +x "$MOCKBIN/zotero"

timeout 10 env \
    GZ_BRIDGE_DIR="$BRIDGE" \
    GZ_GRIMOIRE_CMD="$MOCKBIN/grimoire" \
    GZ_ZOTERO_CMD="$MOCKBIN/zotero" \
    GZ_IS_KINDLE=0 \
    GZ_SWITCH_SETTLE_SEC=0 \
    MOCK_STATE="$STATE" \
    BRIDGE="$BRIDGE" \
    "$TRAMP" >/tmp/opencode/t4.log 2>&1
rc=$?
if [ "$rc" != 0 ] && [ ! -f "$BRIDGE/handoff.json" ] && grep -qi 'circuit' "$BRIDGE/bridge.log"; then
    ok "Test4 bounce storm (rc=$rc)"
else
    fail "Test4 bounce storm (rc=$rc)"
    cat /tmp/opencode/t4.log
    echo "--- log ---"; cat "$BRIDGE/bridge.log" 2>/dev/null
fi

# --- Test 5: oversized / symlink handoff ---
run_test "Test5 oversized/symlink handoff"
T=$(make_tmp)
BRIDGE="$T/bridge"; mkdir -p "$BRIDGE"
STATE="$T/state"; mkdir -p "$STATE"
MOCKBIN="$T/bin"; mkdir -p "$MOCKBIN"
cat > "$MOCKBIN/grimoire" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$MOCKBIN/grimoire"
cat > "$MOCKBIN/zotero" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$MOCKBIN/zotero"

# oversized with metacharacters
python3 - "$BRIDGE/handoff.json" <<'PY'
import sys
p=sys.argv[1]
payload='{"schema_version":1,"target":"zotero","x":"$(touch /tmp/opencode/pwned)"}' + ('A'*300000)
open(p,'w').write(payload)
PY
rm -f /tmp/opencode/pwned
run_tramp >/tmp/opencode/t5a.log 2>&1
rc1=$?
# symlink
ln -s /etc/hostname "$BRIDGE/handoff.json" 2>/dev/null || true
# remove if previous oversized already gone; create symlink fresh
rm -f "$BRIDGE/handoff.json"
ln -s /etc/hostname "$BRIDGE/handoff.json"
run_tramp >/tmp/opencode/t5b.log 2>&1
rc2=$?
if [ "$rc1" = 0 ] && [ "$rc2" = 0 ] && [ ! -e /tmp/opencode/pwned ] && [ ! -L "$BRIDGE/handoff.json" ]; then
    ok "Test5 oversized/symlink (rc1=$rc1 rc2=$rc2, no pwned)"
else
    fail "Test5 oversized/symlink (rc1=$rc1 rc2=$rc2, pwned=$([ -e /tmp/opencode/pwned ] && echo yes || echo no))"
fi

# --- Test 6: single-instance lock ---
run_test "Test6 single-instance lock"
T=$(make_tmp)
BRIDGE="$T/bridge"; mkdir -p "$BRIDGE"
STATE="$T/state"; mkdir -p "$STATE"
MOCKBIN="$T/bin"; mkdir -p "$MOCKBIN"
cat > "$MOCKBIN/grimoire" <<'EOF'
#!/bin/sh
sleep 2
exit 0
EOF
chmod +x "$MOCKBIN/grimoire"
cat > "$MOCKBIN/zotero" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$MOCKBIN/zotero"

run_tramp >/tmp/opencode/t6a.log 2>&1 &
first=$!
sleep 0.5
run_tramp >/tmp/opencode/t6b.log 2>&1
rc2=$?
wait $first
rc1=$?
if [ "$rc2" != 0 ]; then
    ok "Test6 lock (second rc=$rc2, first rc=$rc1)"
else
    fail "Test6 lock (second did not fail: rc2=$rc2)"
fi

# --- Test 7: GZ_UNDER_TRAMPOLINE propagation + --handoff only for target=zotero ---
run_test "Test7 under-trampoline & handoff gating"
T=$(make_tmp)
BRIDGE="$T/bridge"; mkdir -p "$BRIDGE"
STATE="$T/state"; mkdir -p "$STATE"
MOCKBIN="$T/bin"; mkdir -p "$MOCKBIN"
cat > "$MOCKBIN/grimoire" <<'EOF'
#!/bin/sh
echo "g:$GZ_UNDER_TRAMPOLINE" >> "$MOCK_STATE/env.log"
exit 0
EOF
chmod +x "$MOCKBIN/grimoire"
cat > "$MOCKBIN/zotero" <<'EOF'
#!/bin/sh
echo "z:$GZ_UNDER_TRAMPOLINE:$*" >> "$MOCK_STATE/env.log"
exit 43
EOF
chmod +x "$MOCKBIN/zotero"

# Stale handoff targeting grimoire must NOT pass --handoff to zotero when --start zotero is used.
printf '{"schema_version":1,"target":"grimoire","action":"return_essay"}\n' > "$BRIDGE/handoff.json"
run_tramp --start zotero >/tmp/opencode/t7.log 2>&1
rc=$?
if [ "$rc" = 0 ] \
    && grep -q '^z:1:$' "$STATE/env.log" \
    && grep -q '^g:1$' "$STATE/env.log" \
    && grep -q 'handoff zotero -> grimoire' "$BRIDGE/bridge.log"; then
    ok "Test7 under-trampoline & handoff gating (rc=$rc)"
else
    fail "Test7 under-trampoline & handoff gating (rc=$rc)"
    cat "$STATE/env.log" 2>/dev/null
    cat "$BRIDGE/bridge.log" 2>/dev/null
fi

echo "=============================="
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" = 0 ]