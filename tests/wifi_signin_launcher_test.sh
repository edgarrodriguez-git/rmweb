#!/usr/bin/env bash
# Host tests for the wifi-signin takeover launcher: device/wifi-signin/{lib.sh,appload-entry.sh,launch,cleanup}
# (docs/wifi-signin/design.md section 6). Nothing real is touched: systemctl, systemd-run, mount, umount, sleep
# and usleep are stubs on PATH that append what they were asked to stub.log, and /proc, /sys/power, /run, /tmp,
# /dev/dri and /usr/libexec are directories under a temp dir whose path has a space in it on purpose. The
# side-effecting stubs are never removed: lib.sh appends the standard directories to PATH, so a missing stub would
# run the host's real binary (a missing systemd-run is modelled with the WS_SYSTEMD_RUN seam instead).
#
# The model:
#   - systemctl keeps unit state in units/<unit>.state and units/<unit>.mainpid. `stop xochitl` makes it inactive
#     and releases its panel fd; `start` makes it active (units/xochitl.startfail = how many starts fail first;
#     units/<unit>.stopfail = every stop fails and leaves the unit as it was).
#     It also writes a HAZARD line for each device hazard it can see: xochitl stopped less than 30 s after its
#     start, or started while an app or WPE helper is alive (an app also before it shows in /proc: app.pids), or
#     over the EPD lock. Every case requires none.
#   - /proc/<pid>/{stat,cmdline,fd/} and /proc/{uptime,mounts} are plain files; mount and umount edit
#     proc/mounts.
#   - A full or broken /run: freeze_after names the state token after which the state file turns read-only (the
#     systemctl, mount and chmod stubs check it; chmod is the real one plus that check).
#   - /sys/power/wake_lock and wake_unlock are symlinks to stub.log, so every write lands in order among the
#     stub calls ("wifi-signin 900000000000", "wifi-signin-exit 30000000000", or "wifi-signin" for an unlock).
#     locks_held_after replays them with the kernel's rule: a timed lock is only ever extended (R27).
#   - Time: `sleep N` and `usleep N` move the fake boot clock (proc/uptime) by N seconds, add N to the `slept`
#     file and really wait N/100 (at least 10 ms), so the 30 s guard costs 0.3 s. A sleep of 1000 s or more only
#     waits (really, N/100): every case sets WS_REFRESH_S=100000, which parks the 60 s wakelock refresher so it
#     neither spins nor moves the clock (refresher_rewrites_and_stops sets it to 1 to watch it work).
#   - A process that survives TERM and KILL (stuck in the e-ink driver on a tablet) is modelled by a zombie:
#     kill -0 still finds it, and no signal removes it until its parent goes. APP_MODE=stubborn is a live app
#     that ignores TERM, so only a kill -9 (cleanup's, or launch's for an unrecorded app) ends it.
#   - The fake app (app/bin/wifi-signin-wpeqt) registers itself in the fake /proc and waits on a REAL
#     /bin/sleep, so it never moves the clock. APP_MODE picks what it does; HELPER=1 also starts a fake
#     WPEWebProcess.
#
# WS_TEST_SH=/bin/dash runs the scripts under another POSIX shell (their #! lines are rewritten in the copies).
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/device/wifi-signin"
TEST_SH="${WS_TEST_SH:-}"
for v in $(env | sed -n 's/^\(WS_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
BASEPATH="$PATH"
BASE="$(mktemp -d)"
ROOT="$BASE/wifi signin"
mkdir -p "$ROOT"
fails=0; ncase=0; CASE=; T=; dumped=; LPID=
ENVX=()

# --- harness ------------------------------------------------------------------------------------------------

teardown(){ # kill the real sleeps this case left behind (parked refresher, fake app and helper waits)
  [ -n "$T" ] && [ -f "$T/pids" ] || return 0
  local p
  while read -r p; do
    case "$(ps -o comm= -p "$p" 2>/dev/null)" in *sleep*) kill -9 "$p" 2>/dev/null ;; esac
  done < "$T/pids"
  : > "$T/pids"
}
finish(){ teardown; rm -rf "$BASE"; }
trap finish EXIT

dump(){
  [ -n "$dumped" ] && return 0; dumped=1
  echo "    --- stub.log (last 40) ---"; tail -n 40 "$STUBLOG" | sed 's/^/    /'
  echo "    --- journal (last 30) ---"; tail -n 30 "$JOURNAL" | sed 's/^/    /'
}
fail(){ echo "  FAIL [$CASE]: $*"; fails=$((fails+1)); dump; }

write_stubs(){
  cat > "$T/stubs/systemctl" <<'EOF'
#!/bin/sh
# stub systemctl: unit state in $WST/units; HAZARD lines for what would hurt a real tablet
L="$WST/stub.log"; U="$WST/units"
echo "systemctl $*" >> "$L"
"$WST/lib/maybe_freeze"
state() { cat "$U/$1.state" 2>/dev/null || echo inactive; }
age() { # centiseconds since fake process $1 started: the boot clock minus field 22 of its stat (USER_HZ ticks,
  # 100 a second), parsed after the last ')'
  awk -v up="$(awk '{ printf "%d", $1 * 100 + 0.5 }' "$WST/proc/uptime")" \
    '{ s = $0; sub(/.*\)/, "", s); split(s, f, " "); print up - f[20] }' "$WST/proc/$1/stat"
}
alive_matching() { # alive fake processes whose cmdline matches the ERE $1
  for c in "$WST"/proc/[0-9]*/cmdline; do
    [ -f "$c" ] || continue
    if tr '\000' ' ' < "$c" | grep -qE "$1"; then p=${c%/cmdline}; p=${p##*/}; kill -0 "$p" 2>/dev/null && echo "$p"; fi
  done
}
case $1 in
  is-active)
    if [ "$2" = --quiet ]; then [ "$(state "$3")" = active ]; exit; fi
    s=$(state "$2"); echo "$s"; [ "$s" = active ]; exit ;;
  show) # show -p PROPERTY --value UNIT
    case $3 in
      ActiveState) state "$5" ;;
      MainPID) if [ -x "$WST/hooks/mainpid_query" ]; then "$WST/hooks/mainpid_query"; fi
               cat "$U/$5.mainpid" 2>/dev/null || echo 0 ;;
      *) echo 0 ;;
    esac ;;
  stop)
    [ -e "$U/$2.stopfail" ] && exit 1
    if [ "$2" = xochitl ]; then
      pid=$(cat "$U/xochitl.mainpid" 2>/dev/null || echo 0)
      if [ -f "$WST/proc/$pid/stat" ]; then
        a=$(age "$pid"); echo "xochitl stopped at age $((a / 100)) (pid $pid)" >> "$L"
        [ "$a" -ge 3000 ] || echo "HAZARD: xochitl stopped $a cs after its start" >> "$L"
      fi
      rm -rf "$WST/proc/$pid/fd"; echo 0 > "$U/xochitl.mainpid"
    fi
    echo inactive > "$U/$2.state" ;;
  start)
    if [ "$2" = xochitl ]; then
      for p in $(alive_matching 'wifi-signin-wpeqt|wpe-webkit-2\.0/WPE'); do
        echo "HAZARD: xochitl started while pid $p is alive" >> "$L"
      done
      for p in $(cat "$WST/app.pids" 2>/dev/null); do   # every fake app, registered in proc/ or not yet
        if kill -0 "$p" 2>/dev/null; then echo "HAZARD: xochitl started while app pid $p is alive" >> "$L"; fi
      done
      [ -e "$WST/tmp/epframebuffer.lock" ] && echo "HAZARD: xochitl started over the EPD lock" >> "$L"
      f=$(cat "$U/xochitl.startfail" 2>/dev/null || echo 0)
      if [ "$f" -gt 0 ]; then echo $((f - 1)) > "$U/xochitl.startfail"; echo failed > "$U/xochitl.state"; exit 1; fi
    fi
    echo active > "$U/$2.state" ;;
esac
exit 0
EOF
  cat > "$T/stubs/systemd-run" <<'EOF'
#!/bin/sh
# stub systemd-run: logs each argument in brackets
{ printf 'systemd-run'; for a in "$@"; do printf ' [%s]' "$a"; done; echo; } >> "$WST/stub.log"
exit 0
EOF
  cat > "$T/stubs/mount" <<'EOF'
#!/bin/sh
# stub mount: records the overlay in proc/mounts (mount.fail: fail; mount.foreign: someone else's upperdir)
echo "mount $*" >> "$WST/stub.log"
"$WST/lib/maybe_freeze"
[ -e "$WST/mount.fail" ] && exit 32
opts=; prev=; mp=
for a in "$@"; do [ "$prev" = -o ] && opts=$a; prev=$a; mp=$a; done
[ -e "$WST/mount.foreign" ] && opts="lowerdir=$mp,upperdir=/elsewhere/upper,workdir=/elsewhere/work"
echo "overlay $mp overlay rw,$opts 0 0" >> "$WST/proc/mounts"
exit 0
EOF
  cat > "$T/stubs/umount" <<'EOF'
#!/bin/sh
# stub umount: drops the mount point's lines from proc/mounts
echo "umount $*" >> "$WST/stub.log"
echo "clock at umount: $(awk '{ print $1 }' "$WST/proc/uptime" 2>/dev/null)" >> "$WST/stub.log"
echo "slept at umount: $(cat "$WST/slept" 2>/dev/null | awk '{ s += $1 } END { printf "%.2f", s }')" >> "$WST/stub.log"
mp=; for a in "$@"; do mp=$a; done
grep -vF -- " $mp overlay " "$WST/proc/mounts" > "$WST/proc/mounts.new"; mv -f "$WST/proc/mounts.new" "$WST/proc/mounts"
if [ -x "$WST/hooks/after_umount" ]; then "$WST/hooks/after_umount"; fi
exit 0
EOF
  cat > "$T/stubs/sleep" <<'EOF'
#!/bin/sh
# stub sleep: moves the fake boot clock by $1 s and really waits $1/100 s (at least 10 ms); 1000 s or more
# only waits (the parked wakelock refresher)
echo "sleep $*" >> "$WST/stub.log"
case $1 in *.*) if [ -n "${SLEEP_NO_FRACTION:-}" ]; then echo "sleep: invalid number '$1'" >&2; exit 1; fi ;; esac
real=$(awk -v d="$1" 'BEGIN { r = d / 100; if (r < 0.01) r = 0.01; print r }')
if awk -v d="$1" 'BEGIN { exit !(d >= 1000) }'; then echo "$$" >> "$WST/pids"; exec /bin/sleep "$real"; fi
/bin/sleep "$real"
"$WST/lib/advance" "$1"; echo "$1" >> "$WST/slept"
n=$(( $(cat "$WST/sleeps" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$WST/sleeps"
if [ -x "$WST/hooks/after_sleep" ]; then "$WST/hooks/after_sleep" "$n"; fi
exit 0
EOF
  cat > "$T/stubs/usleep" <<'EOF'
#!/bin/sh
# stub usleep: moves the fake boot clock by $1 microseconds and really waits a hundredth of that (min 10 ms)
echo "usleep $*" >> "$WST/stub.log"
s=$(awk -v u="$1" 'BEGIN { print u / 1000000 }')
/bin/sleep "$(awk -v s="$s" 'BEGIN { r = s / 100; if (r < 0.01) r = 0.01; print r }')"
"$WST/lib/advance" "$s"; echo "$s" >> "$WST/slept"
n=$(( $(cat "$WST/usleeps" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$WST/usleeps"
if [ -x "$WST/hooks/after_usleep" ]; then "$WST/hooks/after_usleep" "$n"; fi
exit 0
EOF
  cat > "$T/stubs/chmod" <<'EOF'
#!/bin/sh
# stub chmod: the real chmod, then a chance to freeze the state file (launch's only command between its lock and
# its first state record)
/bin/chmod "$@" || exit
"$WST/lib/maybe_freeze"
EOF
  cat > "$T/lib/maybe_freeze" <<'EOF'
#!/bin/sh
# maybe_freeze: once the state file's last token is the one in $WST/freeze_after ("-": the file is still empty),
# the state file turns read-only, so the launcher's next record fails (a full or broken /run)
f="$WST/freeze_after"; s="$WST/run/wifi-signin/state"
[ -f "$f" ] && [ -f "$s" ] || exit 0
last=$(tail -n 1 "$s"); [ "$(cat "$f")" = "${last:--}" ] || exit 0
/bin/chmod a-w "$s"; rm -f "$f"; echo "state frozen after ${last:--}" >> "$WST/stub.log"
EOF
  cat > "$T/lib/advance" <<'EOF'
#!/bin/sh
# advance SECONDS: moves the fake boot clock forward (atomic replace: readers never see a torn file); a case
# that hides proc/uptime (an unreadable clock) has none to move
u="$WST/proc/uptime"
[ -f "$u" ] || exit 0
awk -v d="$1" '{ printf "%.2f %s\n", $1 + d, $2 }' "$u" > "$u.$$" && mv -f "$u.$$" "$u"
EOF
  cat > "$T/lib/mkstat" <<'EOF'
#!/bin/sh
# mkstat PID START_TICKS [COMM]: a fake proc/PID/stat. Field 22 (start time, USER_HZ ticks since boot) is
# START_TICKS; fields 4-21 hold their own index, so a mis-parsed field shows up as a wrong age.
pid=$1; start=$2; comm=${3:-xochitl}
mkdir -p "$WST/proc/$pid"
{ printf '%s (%s) S' "$pid" "$comm"; i=4; while [ "$i" -le 21 ]; do printf ' %s' "$i"; i=$((i + 1)); done
  printf ' %s' "$start"; i=23; while [ "$i" -le 52 ]; do printf ' 0'; i=$((i + 1)); done; echo; } > "$WST/proc/$pid/stat"
EOF
  cat > "$T/lib/respawn" <<'EOF'
#!/bin/sh
# respawn NEWPID: xochitl restarted; NEWPID started now (fake clock) and holds the panel fd
old=$(cat "$WST/units/xochitl.mainpid")
"$WST/lib/mkstat" "$1" "$(awk '{ print int($1 * 100 + 0.5) }' "$WST/proc/uptime")"
if [ -d "$WST/proc/$old/fd" ]; then mv "$WST/proc/$old/fd" "$WST/proc/$1/fd"; fi
rm -rf "$WST/proc/$old"
echo "$1" > "$WST/units/xochitl.mainpid"
EOF
  chmod +x "$T/stubs/"* "$T/lib/"*
}

write_app(){
  cat > "$APP/bin/wifi-signin-wpeqt" <<'EOF'
#!/bin/sh
# fake wifi-signin-wpeqt: records the run, registers itself in the fake /proc, acts out $APP_MODE
L="$WST/stub.log"
echo "app run" >> "$L"; echo "$$" >> "$WST/app.pids"
n=$(( $(cat "$WST/app.runs" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$WST/app.runs"
if [ -n "${APP_REGISTER_DELAY:-}" ]; then /bin/sleep "$APP_REGISTER_DELAY"; fi   # its exec not in /proc yet
mkdir -p "$WS_PROC/$$"; printf '%s\000' "$0" > "$WS_PROC/$$/cmdline"
[ -e "$WS_EPD_LOCK" ] && echo "HAZARD: app $n started over the EPD lock" >> "$L"
: > "$WS_EPD_LOCK"
if [ "$n" = 1 ]; then
  env | grep -E '^(RMWEB_KIOSK|RMWEB_PROFILE|RMWEB_COOKIES|XDG_CACHE_HOME|XDG_DATA_HOME|XDG_CONFIG_HOME|MESA_SHADER_CACHE_DISABLE|WS_READY_FILE|WS_LOG|QT_QPA_PLATFORM|LD_LIBRARY_PATH)=' \
    | sort > "$WST/app.env"
  ls "$WS_RUN/ovl/upper/wpe-webkit-2.0" > "$WST/app.upper" 2>&1
fi
if [ "${HELPER:-}" = 1 ]; then
  /bin/sleep 30 & h=$!
  echo "$h" >> "$WST/pids"; echo "$h" > "$WST/helper.pid"
  mkdir -p "$WS_PROC/$h"; printf '%s\000%s\000' /usr/libexec/wpe-webkit-2.0/WPEWebProcess 3 > "$WS_PROC/$h/cmdline"
fi
s=
park() { # wait on a REAL sleep; TERM is the app's drain, which exits 0
  trap 'echo "app TERM" >> "$L"; kill "$s" 2>/dev/null; exit 0' TERM
  /bin/sleep 30 & s=$!
  echo "$s" >> "$WST/pids"
  wait "$s"; exit 0
}
ready() { : > "$WS_READY_FILE"; }
case ${APP_MODE:-online} in
  online) ready; exit 20 ;;
  cancel) ready; exit 21 ;;
  code:*) ready; exit "${APP_MODE#code:}" ;;
  grabretry-code:*) # upstream logs this even when a later grab attempt succeeds
    echo '[touch] EVIOCGRAB failed (attempt 1/5, device held elsewhere) - retrying' >&2
    ready; exit "${APP_MODE#grabretry-code:}" ;;
  hang) ready; park ;;
  noready) park ;;
  noready-swtcon) echo 'Failed to initialize SWTCON' >&2; park ;;
  brief) ready; /bin/sleep 0.5; exit 20 ;;
  swtcon) if [ "$n" = 1 ]; then echo 'Failed to initialize SWTCON' >&2; exit 134; fi; ready; exit 20 ;;
  swtcon-twice) echo 'Failed to initialize SWTCON' >&2; exit 134 ;;
  swtcon-late) "$WST/lib/advance" 25; echo 'Failed to initialize SWTCON' >&2; exit 134 ;;
  evgrab) if [ "$n" = 1 ]; then echo '[touch] EVIOCGRAB failed after 5 attempts - cannot run ungrabbed, exiting' >&2; exit 1; fi
          ready; exit 20 ;;
  watchdog63) echo 'Failed to initialize SWTCON' >&2; exit 63 ;;
  stubborn) # alive but deaf to TERM (from before its ready file): only a last-resort kill -9 ends it
    trap 'echo "app TERM ignored" >> "$L"' TERM; ready
    /bin/sleep 30 & s=$!; echo "$s" >> "$WST/pids"
    while kill -0 "$s" 2>/dev/null; do wait "$s"; done; exit 0 ;;
esac
exit 99
EOF
  chmod +x "$APP/bin/wifi-signin-wpeqt"
}

setup(){ # a fresh fake tablet: xochitl active for 990 s and holding the panel, tripletap active, no session
  teardown
  ncase=$((ncase+1)); CASE="$1"; dumped=; LPID=; ENVX=()
  echo "case $ncase: $CASE"
  T="$ROOT/$ncase"
  RUN="$T/run/wifi-signin"; APP="$T/app"; LIBEXEC="$T/usr/libexec"; PROC="$T/proc"
  CARD="$T/dev/dri/card0"; EPD="$T/tmp/epframebuffer.lock"; STUBLOG="$T/stub.log"; JOURNAL="$T/journal"
  mkdir -p "$T/stubs" "$T/lib" "$T/hooks" "$T/units" "$APP/bin" "$T/runtime/libexec/wpe-webkit-2.0" \
    "$T/runtime/lib" "$T/user" "$T/sys/power" "$PROC" "$T/run" "$T/tmp" "$T/dev/dri" "$LIBEXEC"
  : > "$STUBLOG"; : > "$JOURNAL"; : > "$T/pids"
  local f
  for f in lib.sh appload-entry.sh launch cleanup; do   # copied: launch runs "$WS_APP/cleanup", as on the device
    [ -e "$SRC/$f" ] || continue
    if [ -n "$TEST_SH" ] && [ "$f" != lib.sh ]; then { echo "#!$TEST_SH"; tail -n +2 "$SRC/$f"; } > "$APP/$f"
    else cp "$SRC/$f" "$APP/$f"; fi
    chmod +x "$APP/$f"
  done
  cp "$REPO/device/rmweb-env.sh" "$T/runtime/rmweb-env.sh"
  : > "$T/runtime/libexec/wpe-webkit-2.0/WPEWebProcess"
  write_stubs; write_app
  ln -s "$STUBLOG" "$T/sys/power/wake_lock"; ln -s "$STUBLOG" "$T/sys/power/wake_unlock"
  printf '1000.00 900.00\n' > "$PROC/uptime"; : > "$PROC/mounts"
  echo active > "$T/units/xochitl.state"; echo 4242 > "$T/units/xochitl.mainpid"
  WST="$T" "$T/lib/mkstat" 4242 1000
  mkdir -p "$PROC/4242/fd"; ln -s "$CARD" "$PROC/4242/fd/7"
  echo active > "$T/units/xovi-tripletap.state"; echo inactive > "$T/units/wifi-signin.state"
  : > "$EPD"   # left behind by xochitl: launch must remove it before the app starts
  ARGS=(WST="$T" PATH="$T/stubs:$BASEPATH" WS_RUN="$RUN" WS_APP="$APP" WS_RUNTIME="$T/runtime"
    WS_USERDIR="$T/user" WS_SYSPOWER="$T/sys/power" WS_PROC="$PROC" WS_EPD_LOCK="$EPD" WS_CARD="$CARD"
    WS_LIBEXEC="$LIBEXEC" WS_REFRESH_S=100000)
}

run(){ env "${ARGS[@]}" ${ENVX[@]+"${ENVX[@]}"} "$@" >> "$JOURNAL" 2>&1; RC=$?; }
start_bg(){ env "${ARGS[@]}" ${ENVX[@]+"${ENVX[@]}"} "$@" >> "$JOURNAL" 2>&1 & LPID=$!; }
wait_bg(){ # wait_bg SECONDS: wait for the background launch; sets RC (stderr off: bash's "Killed" job notices)
  local i=0
  { while kill -0 "$LPID"; do
      i=$((i+1))
      if [ "$i" -gt $(( $1 * 20 )) ]; then fail "launch still running after $1 s"; kill -9 "$LPID"; break; fi
      sleep 0.05
    done
    wait "$LPID"; RC=$?; } 2>/dev/null
}
wait_ready(){ # until the fake app has written the ready file (the background launch must stay up)
  local i=0
  while [ ! -e "$RUN/ready" ]; do
    kill -0 "$LPID" 2>/dev/null || { fail "launch exited before the app was ready"; return 1; }
    i=$((i+1)); [ "$i" -le 200 ] || { fail "no ready file after 10 s"; return 1; }
    sleep 0.05
  done
}

want(){   grep -qxF -- "$1" "$STUBLOG" || fail "stub.log lacks '$1'"; }
nowant(){ ! grep -qxF -- "$1" "$STUBLOG" || fail "stub.log has unexpected '$1'"; }
nowant_re(){ ! grep -qE -- "$1" "$STUBLOG" || fail "stub.log has unexpected /$1/"; }
jwant(){  grep -qF -- "$1" "$JOURNAL" || fail "journal lacks '$1'"; }
jnowant(){ ! grep -qF -- "$1" "$JOURNAL" || fail "journal has unexpected '$1'"; }
count(){  grep -cxF -- "$1" "$STUBLOG"; }
count_is(){ local n; n=$(count "$1"); [ "$n" = "$2" ] || fail "'$1' appears $n times, want $2"; }
rc_is(){  [ "$RC" = "$1" ] || fail "exit code $RC, want $1"; }
gone(){   [ ! -e "$RUN" ] || fail "\$WS_RUN still exists"; }
in_order(){ # whole stub.log lines, in this order (other lines may come between)
  local after=0 p n
  for p in "$@"; do
    n=$(awk -v a="$after" -v p="$p" 'NR > a && $0 == p { print NR; exit }' "$STUBLOG")
    if [ -z "$n" ]; then fail "stub.log lacks '$p' after line $after"; return 1; fi
    after=$n
  done
}
between_count(){ # between_count START END LINE: how many LINEs come after the first START and before the next END
  awk -v s="$1" -v e="$2" -v p="$3" 'on && $0 == e { exit } on && $0 == p { c++ } $0 == s { on = 1 } END { print c + 0 }' "$STUBLOG"
}
common(){ # what every case requires
  if grep -q HAZARD "$STUBLOG"; then fail "hazard: $(grep HAZARD "$STUBLOG" | head -n 3 | tr '\n' ';')"; fi
  local s
  for s in systemctl systemd-run mount umount; do   # lib.sh appends /usr/bin etc.: a missing stub runs the host's own
    [ -x "$T/stubs/$s" ] || fail "the $s stub is gone: the host's real $s was reachable"
  done
  if grep -qv '^\[wifi-signin\] ' "$JOURNAL"; then fail "journal line without the [wifi-signin] prefix: $(grep -v '^\[wifi-signin\] ' "$JOURNAL" | head -n 2 | tr '\n' ';')"; fi
}
locks_held_after(){ # locks_held_after SECONDS: the wakelocks still held SECONDS after stub.log's writes, replayed
  # with the kernel's rule (a timed lock is only ever extended, never shortened; an unlock drops it) as if all were
  # written at one instant: their order is what matters
  awk -v t="$1" '
    $1 ~ /^wifi-signin(-exit)?$/ && NF == 2 && $2 ~ /^[0-9]+$/ { e = $2 / 1e9; if (!($1 in x) || e > x[$1]) x[$1] = e; next }
    $0 ~ /^wifi-signin(-exit)?$/ { delete x[$0] }
    END { for (n in x) if (x[n] > t) printf "%s ", n }' "$STUBLOG"
}
restored(){ # the end of every session that stopped xochitl
  in_order "umount $LIBEXEC" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl reset-failed xochitl" \
    "systemctl start xochitl" "systemctl start xovi-tripletap"
  local held; held=$(locks_held_after 40)
  [ -z "$held" ] || fail "wakelocks still held 40 s after the session (spec acceptance test 4): $held"
}
MOUNT_LINE(){ echo "mount -t overlay overlay -o lowerdir=$LIBEXEC,upperdir=$RUN/ovl/upper,workdir=$RUN/ovl/work $LIBEXEC"; }

# --- cases --------------------------------------------------------------------------------------------------

setup online
run "$APP/launch"
rc_is 20
in_order "wifi-signin 900000000000" "systemctl stop xovi-tripletap" "systemctl stop xochitl" "$(MOUNT_LINE)" "app run" \
  "umount $LIBEXEC" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl reset-failed xochitl" \
  "systemctl start xochitl" "systemctl start xovi-tripletap"
gone; jwant "[wifi-signin] exit 20 (online)"; common
count_is "wifi-signin" 1                          # R27: the 15 min lock is dropped once, after the 30 s exit lock
nowant_re "ActiveEnterTimestamp"                  # the guard never uses the monotonic clock
want "xochitl stopped at age 990 (pid 4242)"
count_is "app run" 1
count_is "systemctl start xochitl" 1; count_is "systemctl reset-failed xochitl" 1   # one start: StartLimitBurst=4
for kv in "RMWEB_KIOSK=captive" "RMWEB_PROFILE=$RUN/profile" "RMWEB_COOKIES=0" "XDG_CACHE_HOME=$RUN/xdg/cache" \
    "XDG_DATA_HOME=$RUN/xdg/data" "XDG_CONFIG_HOME=$RUN/xdg/config" "MESA_SHADER_CACHE_DISABLE=true" \
    "WS_READY_FILE=$RUN/ready" "WS_LOG=$T/user/log" "QT_QPA_PLATFORM=epaper" "LD_LIBRARY_PATH=$T/runtime/lib"; do
  grep -qxF -- "$kv" "$T/app.env" 2>/dev/null || fail "app environment lacks $kv"
done
grep -qx WPEWebProcess "$T/app.upper" 2>/dev/null || fail "the runtime's WPE helpers were not copied into the overlay upper dir"

setup exit_lock_replaces_the_session_lock
# R27: the kernel only ever extends a timed wakelock, so a 30 s rewrite of "wifi-signin" cannot shorten the 15 min
# lock. On the normal exit-20 path cleanup takes "wifi-signin-exit" for 30 s FIRST (autosleep never gets a gap),
# then unlocks "wifi-signin", and nothing writes the long lock again.
run "$APP/launch"
rc_is 20
in_order "wifi-signin 900000000000" "app run" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl start xochitl"
count_is "wifi-signin" 1; count_is "wifi-signin-exit 30000000000" 1
awk '$0 == "wifi-signin" { u = 1 } u && $0 == "wifi-signin 900000000000" { bad = 1 } END { exit bad }' "$STUBLOG" ||
  fail "the 15 min lock was written again after its unlock"
[ "$(locks_held_after 0)" = "wifi-signin-exit " ] ||
  fail "held as xochitl starts: '$(locks_held_after 0)', want only wifi-signin-exit"
[ -z "$(locks_held_after 40)" ] || fail "held 40 s after the session: $(locks_held_after 40)"
gone; common

# Case 2: every exit code is logged with exitMeaning's string (engine/wpeqt/captive/exit_codes.h).
for kv in "20:online" "21:cancelled" "22:idle timeout" "23:no network" "24:use network as is" "0:SIGTERM drain" \
    "1:touch grab failed" "2:start-up failure" "63:GUI watchdog" "134:killed by signal 6"; do
  setup "each_code_meaning ${kv%%:*}"
  ENVX=(APP_MODE="code:${kv%%:*}")
  run "$APP/launch"
  rc_is "${kv%%:*}"; jwant "[wifi-signin] exit ${kv%%:*} (${kv#*:})"; count_is "app run" 1
  restored; gone; common
done
setup "each_code_meaning (ws_exit_meaning table)"
meaning(){ "${TEST_SH:-/bin/sh}" -c '. "$1"; ws_exit_meaning "$2"' sh "$APP/lib.sh" "$1" 2>&1; }
for kv in "20:online" "21:cancelled" "22:idle timeout" "23:no network" "24:use network as is" "0:SIGTERM drain" \
    "1:touch grab failed" "2:start-up failure" "3:start-up failure" "63:GUI watchdog" "11:already running" \
    "12:display busy" "13:systemd-run missing" "128:killed by signal 0" "129:killed by signal 1" \
    "134:killed by signal 6" "137:killed by signal 9" "143:killed by signal 15" "255:killed by signal 127" \
    "4:unknown" "10:unknown" "19:unknown" "25:unknown" "62:unknown" "64:unknown" "127:unknown"; do
  got="$(meaning "${kv%%:*}")"
  [ "$got" = "${kv#*:}" ] || fail "ws_exit_meaning ${kv%%:*} = '$got', want '${kv#*:}'"
  case "${kv#*:}" in
    "killed by signal"*|unknown) ;;
    *) grep -qF "return \"${kv#*:}\";" "$REPO/engine/wpeqt/captive/exit_codes.h" ||
         fail "exit_codes.h has no exitMeaning string '${kv#*:}'" ;;
  esac
done
grep -qF '"killed by signal %d", code - 128' "$REPO/engine/wpeqt/captive/exit_codes.h" || fail "exit_codes.h signal format changed"
grep -qF 'return "unknown";' "$REPO/engine/wpeqt/captive/exit_codes.h" || fail "exit_codes.h unknown string changed"

setup term_mid_run
ENVX=(APP_MODE=hang)
start_bg "$APP/launch"
if wait_ready; then kill -TERM "$LPID"; fi
wait_bg 20
rc_is 0                                           # the app's own code: its TERM handler drains, exits 0
in_order "app run" "app TERM" "umount $LIBEXEC" "wifi-signin-exit 30000000000" "wifi-signin" \
  "systemctl reset-failed xochitl" "systemctl start xochitl" "systemctl start xovi-tripletap"
jwant "caught SIGTERM"; jwant "[wifi-signin] exit 0 (SIGTERM drain)"; gone; common

setup hup_mid_run
ENVX=(APP_MODE=hang)
start_bg "$APP/launch"
if wait_ready; then kill -HUP "$LPID"; fi
wait_bg 20
rc_is 0
in_order "app run" "app TERM"; restored
jwant "caught SIGHUP"; gone; common

setup launch_sigkilled_post_cleanup
ENVX=(APP_MODE=hang)
start_bg "$APP/launch"
if wait_ready; then kill -KILL "$LPID"; fi
wait_bg 10
rc_is 137
for tok in WAKELOCK TRIPLETAP STOPPED_XOCHITL MOUNTED; do
  grep -qxF "$tok" "$RUN/state" 2>/dev/null || fail "state lacks $tok after SIGKILL"
done
grep -q '^APP=[0-9][0-9]*$' "$RUN/state" 2>/dev/null || fail "state lacks APP=<pid> after SIGKILL"
nowant "systemctl start xochitl"
ENVX=(); run "$APP/cleanup" post
rc_is 0
in_order "app TERM" "umount $LIBEXEC" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl reset-failed xochitl" \
  "systemctl start xochitl" "systemctl start xovi-tripletap"
jwant "cleanup (post)"; gone; common

setup lock_live_pid
mkdir -p "$RUN"; /bin/sleep 60 & OWNER=$!; echo "$OWNER" >> "$T/pids"
echo "$OWNER" > "$RUN/pid"; printf 'WAKELOCK\nTRIPLETAP\nSTOPPED_XOCHITL\n' > "$RUN/state"
run "$APP/launch"
rc_is 11
[ -s "$STUBLOG" ] && fail "a refused launch touched something: $(head -n 3 "$STUBLOG" | tr '\n' ';')"
[ "$(cat "$RUN/pid" 2>/dev/null)" = "$OWNER" ] || fail "the live session's pid file was changed"
[ "$(cat "$RUN/state" 2>/dev/null | tr '\n' ' ')" = "WAKELOCK TRIPLETAP STOPPED_XOCHITL " ] || fail "the live session's state was changed"
jwant "[wifi-signin] exit 11 (already running)"; common
{ kill -9 "$OWNER"; wait "$OWNER"; } 2>/dev/null

setup stale_lock_taken_over
mkdir -p "$RUN/ovl/upper"; echo 999999 > "$RUN/pid"; printf 'WAKELOCK\nMOUNTED\n' > "$RUN/state"
echo "overlay $LIBEXEC overlay rw,lowerdir=$LIBEXEC,upperdir=$RUN/ovl/upper,workdir=$RUN/ovl/work 0 0" > "$PROC/mounts"
run "$APP/launch"
rc_is 20
jwant "stale"
in_order "umount $LIBEXEC" "wifi-signin 900000000000" "systemctl stop xochitl" "$(MOUNT_LINE)" "app run"
restored; count_is "umount $LIBEXEC" 2
gone; jwant "[wifi-signin] exit 20 (online)"; common

setup guard_uses_boottime
# xochitl started 100 s after boot (10000 ticks) and the boot clock says 105 s: it is 5 s old, so the guard waits
# 25 s. (A monotonic-clock reading would say otherwise after a suspend; the stub answers anything but MainPID
# and ActiveState with 0, and case 1 checks it is never asked.) The comm "x) (y" checks the last-')' parse.
WST="$T" "$T/lib/mkstat" 4242 10000 'x) (y'; printf '105.00 90.00\n' > "$PROC/uptime"
run "$APP/launch"
rc_is 20
n=$(between_count "systemctl stop xovi-tripletap" "systemctl stop xochitl" "sleep 1")
[ "$n" = 25 ] || fail "guard slept $n s, want 25"
want "xochitl stopped at age 30 (pid 4242)"; nowant_re "ActiveEnterTimestamp"; gone; common

setup guard_age_counts_centiseconds
# xochitl started at 100.99 s (10099 ticks): at 130.00 s it is 29.01 s old, which flooring the two values apart
# (130 - 100) would call 30. The stop must wait for 131.00 s, one more pass.
WST="$T" "$T/lib/mkstat" 4242 10099; printf '105.00 90.00\n' > "$PROC/uptime"
run "$APP/launch"
rc_is 20
n=$(between_count "systemctl stop xovi-tripletap" "systemctl stop xochitl" "sleep 1")
[ "$n" = 26 ] || fail "guard slept $n s, want 26"
want "xochitl stopped at age 30 (pid 4242)"; gone; common

setup guard_restarts_on_new_mainpid
WST="$T" "$T/lib/mkstat" 4242 10000; printf '105.00 90.00\n' > "$PROC/uptime"
printf '#!/bin/sh\n[ "$1" = 3 ] && "$WST/lib/respawn" 5151\nexit 0\n' > "$T/hooks/after_sleep"; chmod +x "$T/hooks/after_sleep"
run "$APP/launch"
rc_is 20
n=$(between_count "systemctl stop xovi-tripletap" "systemctl stop xochitl" "sleep 1")
[ "$n" = 33 ] || fail "guard slept $n s, want 33 (3, then 30 more for the new MainPID)"
want "xochitl stopped at age 30 (pid 5151)"; jwant "restarting the wait"; gone; common

setup guard_rechecks_mainpid_before_stop
# xochitl turns 30 s old at 130 s. The third MainPID query at that time is the re-read just before the stop (the
# first two are the pass's own read and ws_xochitl_age's): xochitl restarts right then, so the stop must wait
# 30 s more. Whatever the query order, the stub's HAZARD check catches a stop of the young process.
WST="$T" "$T/lib/mkstat" 4242 10000; printf '105.00 90.00\n' > "$PROC/uptime"
cat > "$T/hooks/mainpid_query" <<'EOF'
#!/bin/sh
[ "$(awk '{ print int($1) }' "$WST/proc/uptime")" -ge 130 ] || exit 0
n=$(( $(cat "$WST/q130" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$WST/q130"
[ "$n" = 3 ] && "$WST/lib/respawn" 6161
exit 0
EOF
chmod +x "$T/hooks/mainpid_query"
run "$APP/launch"
rc_is 20
want "xochitl stopped at age 30 (pid 6161)"; jwant "restarting the wait"; gone; common

setup xochitl_inactive_exits_12_and_never_starts_xochitl
echo inactive > "$T/units/xochitl.state"; echo 0 > "$T/units/xochitl.mainpid"; rm -rf "$PROC/4242"
run "$APP/launch"
rc_is 12
nowant "systemctl stop xochitl"; nowant "systemctl start xochitl"; nowant "systemctl reset-failed xochitl"
nowant "app run"; nowant_re "^mount "
count_is "sleep 1" 0                              # not active and not starting: refused at once, no guard wait
in_order "wifi-signin 900000000000" "systemctl stop xovi-tripletap" "wifi-signin" "systemctl start xovi-tripletap"
jwant "another program owns the display"; jwant "[wifi-signin] exit 12 (display busy)"; gone; common

setup xochitl_still_active_after_stop_refuses
: > "$T/units/xochitl.stopfail"                   # the stop fails and xochitl keeps the panel
run "$APP/launch"
rc_is 12
nowant "app run"; nowant_re "^mount "; nowant "systemctl start xochitl"; nowant "systemctl reset-failed xochitl"
in_order "systemctl stop xochitl" "systemctl is-active --quiet xochitl" "wifi-signin" "systemctl start xovi-tripletap"
jwant "WARN: systemctl stop xochitl failed"; jwant "xochitl is still active after the stop: not launching"
jwant "[wifi-signin] exit 12 (display busy)"; gone; common

setup card_held_after_wait_skips_app_and_restores
mkdir -p "$PROC/777/fd"; ln -s "$CARD" "$PROC/777/fd/3"
run "$APP/launch"
rc_is 12
nowant "app run"; nowant_re "^mount "
in_order "systemctl stop xochitl" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl reset-failed xochitl" \
  "systemctl start xochitl" "systemctl start xovi-tripletap"
jwant "pid 777"; jwant "[wifi-signin] exit 12 (display busy)"; gone; common

setup card_released_during_wait_launches
mkdir -p "$PROC/777/fd"; ln -s "$CARD" "$PROC/777/fd/3"
printf '#!/bin/sh\n[ "$1" = 3 ] && rm -rf "$WST/proc/777/fd"\nexit 0\n' > "$T/hooks/after_usleep"; chmod +x "$T/hooks/after_usleep"
run "$APP/launch"
rc_is 20
in_order "systemctl stop xochitl" "usleep 200000" "usleep 1500000" "$(MOUNT_LINE)" "app run"
restored; gone; common

setup swtcon_retry_once
ENVX=(APP_MODE=swtcon)
run "$APP/launch"
rc_is 20
count_is "app run" 2
in_order "app run" "usleep 3000000" "app run"; restored
jwant "[wifi-signin] exit 134 (killed by signal 6)"; jwant "retrying once"; jwant "[wifi-signin] exit 20 (online)"
gone; common

setup evioc_grab_retry_once
ENVX=(APP_MODE=evgrab)
run "$APP/launch"
rc_is 20
count_is "app run" 2; in_order "app run" "usleep 3000000" "app run"; restored
jwant "[wifi-signin] exit 1 (touch grab failed)"; gone; common

setup swtcon_twice_no_third_run
ENVX=(APP_MODE=swtcon-twice)
run "$APP/launch"
rc_is 134
count_is "app run" 2; in_order "app run" "usleep 3000000" "app run"; restored
jwant "[wifi-signin] exit 134 (killed by signal 6)"; gone; common

setup gui_watchdog_63_never_retried
ENVX=(APP_MODE=watchdog63)
run "$APP/launch"
rc_is 63
count_is "app run" 1; restored; jwant "[wifi-signin] exit 63 (GUI watchdog)"; gone; common

setup late_swtcon_not_retried
ENVX=(APP_MODE=swtcon-late)                       # the app runs 25 s (fake clock) before failing: past the window
run "$APP/launch"
rc_is 134
count_is "app run" 1; restored; gone; common

setup cancel_no_retry
ENVX=(APP_MODE=cancel)
run "$APP/launch"
rc_is 21
count_is "app run" 1; nowant "usleep 3000000"; restored; jwant "[wifi-signin] exit 21 (cancelled)"; gone; common

# R23: a deliberate outcome is never retried, even with a touch marker in stderr and well inside the window
# (an already-open network exits 20 after about 2 s; a retry would cost an extra xochitl stop and start).
for kv in "20:online" "0:SIGTERM drain" "21:cancelled" "22:idle timeout" "23:no network" "24:use network as is"; do
  setup "deliberate_exit_never_retried ${kv%%:*}"
  ENVX=(APP_MODE="grabretry-code:${kv%%:*}")
  run "$APP/launch"
  rc_is "${kv%%:*}"
  count_is "app run" 1; nowant "usleep 3000000"; jnowant "retrying once"
  jwant "[wifi-signin] exit ${kv%%:*} (${kv#*:})"; restored; gone; common
done

setup start_failure_with_marker_retried_once
ENVX=(APP_MODE="grabretry-code:2")                # the same marker with a start-up failure code: retried, once
run "$APP/launch"
rc_is 2
count_is "app run" 2; in_order "app run" "usleep 3000000" "app run"; restored
jwant "retrying once"; jwant "[wifi-signin] exit 2 (start-up failure)"; gone; common

setup noready_killed_after_timeout
ENVX=(APP_MODE=noready HELPER=1)
run "$APP/launch"
rc_is 137
count_is "app run" 1; restored
jwant "no ready file"; jwant "[wifi-signin] exit 137 (killed by signal 9)"
kill -0 "$(cat "$T/helper.pid" 2>/dev/null || echo 999999)" 2>/dev/null && fail "the WPE helper survived the start-up watchdog"
gone; common

setup watchdog_kill_not_retried
# A 5 s watchdog kill lands inside the 20 s retry window, with a SWTCON line in stderr: still no retry.
ENVX=(APP_MODE=noready-swtcon WS_READY_TIMEOUT_S=5)
run "$APP/launch"
rc_is 137
count_is "app run" 1; nowant "usleep 3000000"; restored; jwant "no ready file"; jnowant "retrying"; gone; common

setup ready_timeout_zero_disables
printf 'WS_READY_TIMEOUT_S=0\n' > "$APP/launch.env"   # the dev-only override hook (plan Task 12)
ENVX=(APP_MODE=noready)
start_bg "$APP/launch"
sleep 1.2                                         # the default 30 s watchdog would have fired after 0.3 s
if kill -0 "$LPID" 2>/dev/null; then kill -TERM "$LPID"; else fail "launch ended on its own"; fi
wait_bg 20
rc_is 0
count_is "app run" 1; in_order "app run" "app TERM"; restored
jnowant "no ready file"; jwant "[wifi-signin] exit 0 (SIGTERM drain)"; gone; common

setup bad_numbers_in_launch_env_fall_back
# A typo in the dev override file: dash and BusyBox ash abort a whole script on $(( 20s * 100 )), so cleanup would
# die before xochitl's start. launch and cleanup (which reads launch.env again) each put every bad number back to
# its default with a WARN. xochitl is 5 s old: the default 30 s guard waits 25 s.
printf 'WS_STOP_DEADLINE_S=20s\nWS_GUARD_S=abc\nWS_APP_TERM_WAIT_S=08\n' > "$APP/launch.env"
WST="$T" "$T/lib/mkstat" 4242 10000; printf '105.00 90.00\n' > "$PROC/uptime"
run "$APP/launch"
rc_is 20
count_is "app run" 1; restored; want "xochitl stopped at age 30 (pid 4242)"
n=$(between_count "systemctl stop xovi-tripletap" "systemctl stop xochitl" "sleep 1")
[ "$n" = 25 ] || fail "guard slept $n s, want 25 (the 30 s default)"
for kv in "WS_STOP_DEADLINE_S='20s' is not a whole number of seconds: using 20" \
    "WS_GUARD_S='abc' is not a whole number of seconds: using 30" \
    "WS_APP_TERM_WAIT_S='08' is not a whole number of seconds: using 3"; do
  n=$(grep -cF "WARN: $kv" "$JOURNAL")
  [ "$n" = 2 ] || fail "journal has 'WARN: $kv' $n times, want 2 (launch and cleanup)"
done
jwant "[wifi-signin] exit 20 (online)"; gone; common

setup bad_stop_deadline_cleanup_still_restores
printf 'WS_STOP_DEADLINE_S=20s\n' > "$APP/launch.env"   # launch never uses it: only cleanup's $(( )) would abort
run "$APP/launch"
rc_is 20
count_is "app run" 1; restored; count_is "systemctl start xochitl" 1
n=$(grep -cF "WARN: WS_STOP_DEADLINE_S='20s' is not a whole number of seconds: using 20" "$JOURNAL")
[ "$n" = 2 ] || fail "journal has the WS_STOP_DEADLINE_S warning $n times, want 2"
jwant "cleanup done"; gone; common

setup huge_stop_deadline_cleanup_still_restores
# Probe P2 (R34): all digits, but too many for $(( )). dash aborted cleanup on $(( WS_STOP_DEADLINE_S * 100 ))
# ("Illegal number") before xochitl's start, and ExecStopPost would have died the same way; bash wraps the value.
printf 'WS_STOP_DEADLINE_S=99999999999999999999\n' > "$APP/launch.env"
run "$APP/launch"
rc_is 20
count_is "app run" 1; restored; count_is "systemctl start xochitl" 1
n=$(grep -cF "WARN: WS_STOP_DEADLINE_S='99999999999999999999' has 10 or more digits: using 20" "$JOURNAL")
[ "$n" = 2 ] || fail "journal has the 20-digit WS_STOP_DEADLINE_S warning $n times, want 2 (launch and cleanup)"
jwant "cleanup done"; gone; common

setup launch_env_syntax_error_ignored
# R34: an unbalanced quote in the dev override file. dash and BusyBox ash abort a script that sources it; bash goes
# on with the lines before the error (WS_GUARD_S=10 here) and prints its error into the journal. launch and cleanup
# each try the file in a subshell first and ignore all of it with a WARN. xochitl is 5 s old: the default 30 s
# guard waits 25 s.
printf 'WS_GUARD_S=10\nWS_GUARD_S="30\n' > "$APP/launch.env"
WST="$T" "$T/lib/mkstat" 4242 10000; printf '105.00 90.00\n' > "$PROC/uptime"
run "$APP/launch"
rc_is 20
count_is "app run" 1; restored; count_is "systemctl start xochitl" 1
n=$(between_count "systemctl stop xovi-tripletap" "systemctl stop xochitl" "sleep 1")
[ "$n" = 25 ] || fail "guard slept $n s, want 25 (the 30 s default: none of the file applies)"
n=$(grep -cF "WARN: ignoring $APP/launch.env: a trial read of it failed" "$JOURNAL")
[ "$n" = 2 ] || fail "journal has the launch.env warning $n times, want 2 (launch and cleanup)"
jnowant "with overrides from"; jwant "cleanup done"; gone; common

setup launch_env_broken_mid_session_cleanup_restores
# The file breaks while the app runs: cleanup alone reads it, and must still restore everything.
ENVX=(APP_MODE=hang)
start_bg "$APP/launch"
if wait_ready; then printf 'WS_GUARD_S="30\n' > "$APP/launch.env"; kill -TERM "$LPID"; fi
wait_bg 20
rc_is 0
in_order "app run" "app TERM"; restored; count_is "systemctl start xochitl" 1
n=$(grep -cF "WARN: ignoring $APP/launch.env: a trial read of it failed" "$JOURNAL")
[ "$n" = 1 ] || fail "journal has the launch.env warning $n times, want 1 (cleanup)"
jwant "cleanup done"; gone; common

setup validate_numbers_bounds
# R34: an accepted value has at most 9 digits, so $(( x * 100 )) always fits; WS_REFRESH_S must be at least 1 (0
# spins the refresher on `sleep 0`). The others keep 0: case 15 sets WS_READY_TIMEOUT_S=0 on purpose.
out="$(env "${ARGS[@]}" "${TEST_SH:-/bin/sh}" -c '
  . "$WS_APP/lib.sh"
  WS_STOP_DEADLINE_S=99999999999999999999 WS_GUARD_S=1000000000 WS_GUARD_MAX_S=999999999 WS_REFRESH_S=0
  WS_READY_TIMEOUT_S=0 WS_CARD_WAIT_S=0 WS_RETRY_WINDOW_S=0
  ws_validate_numbers
  echo "now $WS_STOP_DEADLINE_S $WS_GUARD_S $WS_GUARD_MAX_S $WS_REFRESH_S $WS_READY_TIMEOUT_S $WS_CARD_WAIT_S $WS_RETRY_WINDOW_S"
  WS_REFRESH_S=1; ws_validate_numbers; echo "refresh $WS_REFRESH_S"' 2>&1)"
echo "$out" | grep -qx 'now 20 30 999999999 60 0 0 0' ||
  fail "after ws_validate_numbers: '$(echo "$out" | grep '^now')', want 'now 20 30 999999999 60 0 0 0'"
echo "$out" | grep -qx 'refresh 1' || fail "WS_REFRESH_S=1 was not kept: '$(echo "$out" | grep '^refresh')'"
for kv in "WS_STOP_DEADLINE_S='99999999999999999999' has 10 or more digits: using 20" \
    "WS_GUARD_S='1000000000' has 10 or more digits: using 30" "WS_REFRESH_S='0' is below its minimum of 1: using 60"; do
  echo "$out" | grep -qxF "[wifi-signin] WARN: $kv" || fail "ws_validate_numbers did not log 'WARN: $kv'"
done
n=$(echo "$out" | grep -c WARN)
[ "$n" = 3 ] || fail "ws_validate_numbers logged $n WARN lines, want 3: $(echo "$out" | grep WARN | tr '\n' ';')"
common

setup tripletap_still_active_after_stop_refuses
# The stop fails and xovi-tripletap keeps running: a triple press would then restart xochitl over the app.
: > "$T/units/xovi-tripletap.stopfail"
run "$APP/launch"
rc_is 12
nowant "systemctl stop xochitl"; nowant "app run"; nowant_re "^mount "; count_is "sleep 1" 0
in_order "wifi-signin 900000000000" "systemctl stop xovi-tripletap" "systemctl is-active --quiet xovi-tripletap" \
  "wifi-signin" "systemctl start xovi-tripletap"
jwant "xovi-tripletap is still active after the stop: not stopping xochitl"; jnowant "xovi-tripletap stopped"
jwant "[wifi-signin] exit 12 (display busy)"; gone; common

setup tripletap_inactive_left_alone
echo inactive > "$T/units/xovi-tripletap.state"
run "$APP/launch"
rc_is 20
want "systemctl is-active --quiet xovi-tripletap"
nowant "systemctl stop xovi-tripletap"; nowant "systemctl start xovi-tripletap"
in_order "systemctl stop xochitl" "app run" "umount $LIBEXEC" "wifi-signin-exit 30000000000" "wifi-signin" \
  "systemctl reset-failed xochitl" "systemctl start xochitl"
gone; common

setup cleanup_twice_noop
ENVX=(APP_MODE=hang)
start_bg "$APP/launch"
if wait_ready; then kill -KILL "$LPID"; fi
wait_bg 10
ENVX=(); run "$APP/cleanup" post
rc_is 0; restored; gone
: > "$STUBLOG"; lines=$(wc -l < "$JOURNAL")
run "$APP/cleanup" post; rc_is 0
run "$APP/cleanup" trap; rc_is 0
[ -s "$STUBLOG" ] && fail "a second cleanup touched something: $(head -n 3 "$STUBLOG" | tr '\n' ';')"
[ "$(wc -l < "$JOURNAL")" = "$lines" ] || fail "a second cleanup logged: $(tail -n 2 "$JOURNAL" | tr '\n' ';')"
common

setup cleanup_ignores_a_live_or_foreign_session
ENVX=(APP_MODE=hang)
start_bg "$APP/launch"
if wait_ready; then
  before=$(wc -l < "$STUBLOG")
  ENVX=(); run "$APP/cleanup" post; rc_is 0       # launch is alive: post mode leaves its state alone
  run "$APP/cleanup" trap; rc_is 0                # our parent is not the recorded launch: trap mode too
  [ "$(wc -l < "$STUBLOG")" = "$before" ] || fail "cleanup acted on a live session: $(tail -n 2 "$STUBLOG" | tr '\n' ';')"
  [ -f "$RUN/state" ] || fail "cleanup removed a live session's state"
  kill -TERM "$LPID"
fi
wait_bg 20
rc_is 0; restored; gone; common

setup entry_unit_deactivating_exits_0
for st in deactivating active activating reloading; do
  echo "$st" > "$T/units/wifi-signin.state"
  run "$APP/appload-entry.sh"
  rc_is 0
done
nowant_re "^systemd-run"; nowant_re "xochitl"; common

setup entry_execs_systemd_run
APPQ="$APP"
for st in inactive failed; do
  echo "$st" > "$T/units/wifi-signin.state"
  run "$APP/appload-entry.sh"
  rc_is 0
done
count_is "systemd-run [--unit=wifi-signin] [--collect] [--quiet] [-p] [ExecStopPost=-$APPQ/cleanup post] [-p] [TimeoutStopSec=30] [-p] [MemoryAccounting=yes] [$APPQ/launch]" 2
nowant_re "xochitl"; common

setup entry_no_systemd_run_exits_13
# R30: "missing" comes from the WS_SYSTEMD_RUN seam alone. The stub stays on PATH (and lib.sh appends the standard
# directories anyway), so a host's real /usr/bin/systemd-run can never run here.
ENVX=(WS_SYSTEMD_RUN=/nonexistent/systemd-run)
run "$APP/appload-entry.sh"
rc_is 13
nowant_re "^systemd-run"; nowant_re "xochitl"; jwant "[wifi-signin] exit 13 (systemd-run missing)"; common

setup wakelock_write_fails_never_touches_xochitl
rm -f "$T/sys/power/wake_lock"; mkdir "$T/sys/power/wake_lock"   # a directory: the write fails
run "$APP/launch"
rc_is 12
nowant_re "xochitl"; nowant_re "tripletap"; nowant "app run"
jwant "wakelock"; jwant "[wifi-signin] exit 12 (display busy)"; gone; common

# Every token is recorded BEFORE its action (R24), so a record that fails (a full or broken /run) must refuse 12
# without that action, and the refusal leaves nothing held. The state file turns read-only right after the named
# token ("-": right after the lock, so WAKELOCK is the first record to fail).
if [ "$(id -u)" = 0 ]; then
  echo "  (state_record_fails cases skipped: root writes a read-only file anyway)"
else
  for tok in - WAKELOCK TRIPLETAP STOPPED_XOCHITL MOUNTED; do
    setup "state_record_fails_after $tok"
    echo "$tok" > "$T/freeze_after"
    ENVX=(APP_MODE=online)
    if [ "$tok" = MOUNTED ]; then
      # The app must not outlive the refusal, even while its exec is not yet visible in /proc (0.3 s here, far
      # longer than cleanup takes to start): launch waits up to 1 s for it. Once the state is frozen, a settle
      # returns only after the fake app has registered (at most 5 s), so that wait holds on any host.
      ENVX=(APP_MODE=hang APP_REGISTER_DELAY=0.3)
      cat > "$T/hooks/after_usleep" <<'EOF'
#!/bin/sh
grep -q '^state frozen after' "$WST/stub.log" || exit 0
i=0
while [ "$i" -lt 100 ] && ! grep -qs wifi-signin-wpeqt "$WST"/proc/[0-9]*/cmdline; do /bin/sleep 0.05; i=$((i + 1)); done
EOF
      chmod +x "$T/hooks/after_usleep"
    fi
    start_bg "$APP/launch"; wait_bg 20
    rc_is 12
    want "state frozen after $tok"; jwant "could not record"; jwant "[wifi-signin] exit 12 (display busy)"
    case $tok in
      -) nowant "wifi-signin 900000000000"; nowant "wifi-signin"; nowant_re "tripletap|xochitl" ;;
      WAKELOCK) in_order "wifi-signin 900000000000" "wifi-signin"; nowant_re "(stop|start) (xovi-tripletap|xochitl)" ;;
      TRIPLETAP) in_order "wifi-signin 900000000000" "systemctl stop xovi-tripletap" "wifi-signin" \
                   "systemctl start xovi-tripletap"; nowant "systemctl stop xochitl"; nowant "systemctl start xochitl" ;;
      STOPPED_XOCHITL) in_order "systemctl stop xochitl" "wifi-signin-exit 30000000000" "wifi-signin" \
                         "systemctl reset-failed xochitl" "systemctl start xochitl" "systemctl start xovi-tripletap"
                       nowant_re "^mount "; nowant "umount $LIBEXEC" ;;
      MOUNTED) in_order "$(MOUNT_LINE)" "app run" "app TERM"; restored; count_is "app run" 1 ;;
    esac
    [ "$tok" = MOUNTED ] || nowant "app run"
    [ "$(cat "$T/units/xochitl.state")" = active ] || fail "xochitl is $(cat "$T/units/xochitl.state") at the end"
    [ "$(cat "$T/units/xovi-tripletap.state")" = active ] || fail "xovi-tripletap is not active at the end"
    [ ! -s "$PROC/mounts" ] || fail "an overlay is still mounted: $(cat "$PROC/mounts")"
    [ -z "$(locks_held_after 40)" ] || fail "wakelocks still held 40 s after the refusal: $(locks_held_after 40)"
    gone; common
  done

  # R34: the APP= record fails and the app's exec is still not in /proc when launch's 1 s wait ends (1.5 s here,
  # with no settle hook). Its command line is then still launch's, so cleanup's identity check cannot know it:
  # launch must stop its own child itself, before it refuses. The systemctl stub's app.pids check fails the case if
  # the app is alive as xochitl starts. stubborn: the app ignores TERM, so only launch's kill -9 ends it.
  for variant in slow-exec stubborn; do
    setup "unrecorded_app_stopped_by_launch ($variant)"
    echo MOUNTED > "$T/freeze_after"
    if [ "$variant" = slow-exec ]; then
      ENVX=(APP_MODE=hang APP_REGISTER_DELAY=1.5)
    else
      ENVX=(APP_MODE=stubborn)
      cat > "$T/hooks/after_usleep" <<'EOF'
#!/bin/sh
# once the state is frozen, a settle returns only after the app's ready file (it ignores TERM by then)
grep -q '^state frozen after' "$WST/stub.log" || exit 0
i=0
while [ "$i" -lt 100 ] && [ ! -e "$WST/run/wifi-signin/ready" ]; do /bin/sleep 0.05; i=$((i + 1)); done
EOF
      chmod +x "$T/hooks/after_usleep"
    fi
    start_bg "$APP/launch"; wait_bg 20
    rc_is 12
    want "state frozen after MOUNTED"; jwant "could not record APP="; jwant "[wifi-signin] exit 12 (display busy)"
    restored
    jnowant "survived kill -9"; jnowant "(unrecorded) APP="   # launch collected it: nothing is left to cleanup
    for p in $(cat "$T/app.pids" 2>/dev/null); do
      if kill -0 "$p" 2>/dev/null; then fail "the app (pid $p) outlived the refusal"; fi
    done
    if [ "$variant" = stubborn ]; then
      in_order "app run" "app TERM ignored" "umount $LIBEXEC"
      jwant "WARN: the unrecorded app is still running 3 s after TERM: kill -9"
    fi
    [ "$(cat "$T/units/xochitl.state")" = active ] || fail "xochitl is $(cat "$T/units/xochitl.state") at the end"
    [ ! -s "$PROC/mounts" ] || fail "an overlay is still mounted: $(cat "$PROC/mounts")"
    gone; common
  done
fi

setup guard_gives_up_on_a_stuck_xochitl
echo activating > "$T/units/xochitl.state"
ENVX=(WS_GUARD_MAX_S=10)
run "$APP/launch"
rc_is 12
count_is "sleep 1" 10; nowant "systemctl stop xochitl"; nowant "systemctl start xochitl"; nowant "app run"
in_order "systemctl stop xovi-tripletap" "wifi-signin" "systemctl start xovi-tripletap"
jwant "[wifi-signin] exit 12 (display busy)"; gone; common

setup xochitl_start_retried_once
echo 1 > "$T/units/xochitl.startfail"
run "$APP/launch"
rc_is 20
in_order "app run" "umount $LIBEXEC" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl reset-failed xochitl" \
  "systemctl start xochitl" "usleep 2000000" "systemctl reset-failed xochitl" "systemctl start xochitl" \
  "systemctl start xovi-tripletap"
count_is "systemctl start xochitl" 2; jwant "WARN"; gone; common

setup xochitl_never_returns_tripletap_still_restarted
# Both starts fail: tripletap still comes back, because its triple press (xovi/start) is then the owner's way
# to bring xochitl back.
echo 2 > "$T/units/xochitl.startfail"
run "$APP/launch"
rc_is 20
in_order "app run" "umount $LIBEXEC" "systemctl start xochitl" "usleep 2000000" "systemctl reset-failed xochitl" \
  "systemctl start xochitl" "systemctl start xovi-tripletap"
count_is "systemctl start xochitl" 2; jwant "ERROR: xochitl is still not active"
[ "$(cat "$T/units/xochitl.state")" = failed ] || fail "xochitl is $(cat "$T/units/xochitl.state"), want failed"
[ "$(cat "$T/units/xovi-tripletap.state")" = active ] || fail "xovi-tripletap was not restarted"
gone; common

setup live_app_ignoring_term_gets_kill_9
# A live app (not a zombie) that ignores TERM: after the 3 s drain and the 10 s helper wait, cleanup kills it
# with SIGKILL, and xochitl starts only once it is gone.
ENVX=(APP_MODE=stubborn)
start_bg "$APP/launch"
if wait_ready; then kill -TERM "$LPID"; fi
wait_bg 20
rc_is 137                                         # the app's own end
in_order "app run" "app TERM ignored" "umount $LIBEXEC"; restored
jwant "the app is still running 3 s after TERM"; jwant "WARN: the app or WPE helpers remain after 10 s: kill -9"
jnowant "survived kill -9"; jwant "[wifi-signin] exit 137 (killed by signal 9)"; gone; common

setup exit_lock_fails_keeps_the_session_lock
# The 30 s exit lock cannot be taken: the 15 min lock is then kept (it expires on its own) rather than dropped,
# so xochitl's start is never left without a wakelock.
printf '#!/bin/sh\nrm -f "$WST/sys/power/wake_lock"; mkdir "$WST/sys/power/wake_lock"\n' > "$T/hooks/after_umount"
chmod +x "$T/hooks/after_umount"
run "$APP/launch"
rc_is 20
nowant "wifi-signin-exit 30000000000"; nowant "wifi-signin"
in_order "app run" "umount $LIBEXEC" "systemctl reset-failed xochitl" "systemctl start xochitl" \
  "systemctl start xovi-tripletap"
jwant "WARN: could not take the exit wakelock: keeping the 15 min one"; gone; common

setup helpers_killed_before_xochitl_returns
ENVX=(APP_MODE=hang HELPER=1)
start_bg "$APP/launch"
if wait_ready; then kill -TERM "$LPID"; fi
wait_bg 20
rc_is 0; restored
kill -0 "$(cat "$T/helper.pid" 2>/dev/null || echo 999999)" 2>/dev/null && fail "the WPE helper survived cleanup"
gone; common

setup umount_only_our_overlay
: > "$T/mount.foreign"                            # proc/mounts shows another upperdir on /usr/libexec
run "$APP/launch"
rc_is 20
nowant "umount $LIBEXEC"; in_order "app run" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl start xochitl"
gone; common

setup mount_fails_restores
: > "$T/mount.fail"
run "$APP/launch"
rc_is 12
nowant "app run"; nowant "umount $LIBEXEC"
in_order "systemctl stop xochitl" "$(MOUNT_LINE)" "wifi-signin-exit 30000000000" "wifi-signin" \
  "systemctl reset-failed xochitl" "systemctl start xochitl" "systemctl start xovi-tripletap"
jwant "overlay"; jwant "[wifi-signin] exit 12 (display busy)"; gone; common

setup refresher_rewrites_and_stops
ENVX=(APP_MODE=brief WS_REFRESH_S=1)              # 1 s refreshes during a 0.5 s run (the clock runs 100x fast)
run "$APP/launch"
rc_is 20
n=$(count "wifi-signin 900000000000")
[ "$n" -ge 3 ] || fail "the wakelock was written $n times, want the first write plus refreshes"
[ "$(grep '^wifi-signin' "$STUBLOG" | tail -n 2 | tr '\n' ';')" = "wifi-signin-exit 30000000000;wifi-signin;" ] ||
  fail "the refresher wrote after cleanup's exit lock and unlock: $(grep '^wifi-signin' "$STUBLOG" | tail -n 3 | tr '\n' ';')"
restored; gone; common

setup wait_for_deadline_with_slow_checks
# ws_wait_for is bounded by the boot clock, not by its poll count: each check here costs 1 s of fake clock and
# each settle 0.2 s, so a 5 s wait ends after 5 checks at 5.8 s (the old count-based loop: 26 checks, 31 s).
mkdir -p "$T/stubs-nousleep"; ln -s "$T/stubs/sleep" "$T/stubs-nousleep/sleep"
out="$(env "${ARGS[@]}" "${TEST_SH:-/bin/sh}" -c '
  . "$WS_APP/lib.sh"
  clock() { awk "{ print \$1 }" "$WS_PROC/uptime"; }
  n=0; slow_fail() { "$WST/lib/advance" 1; n=$((n + 1)); return 1; }
  t0=$(clock); ws_wait_for 5 slow_fail; rc=$?; echo "fail rc=$rc checks=$n t0=$t0 t1=$(clock)"
  n=0; slow_ok() { "$WST/lib/advance" 1; n=$((n + 1)); [ "$n" -ge 3 ]; }
  ws_wait_for 5 slow_ok; echo "ok rc=$? checks=$n"
  never() { return 1; }
  mv "$WS_PROC/uptime" "$WS_PROC/uptime.away"
  c0=$(cat "$WST/usleeps"); ws_wait_for 1 never; rc=$?; c1=$(cat "$WST/usleeps")
  mv "$WS_PROC/uptime.away" "$WS_PROC/uptime"
  echo "backstop rc=$rc settles=$((c1 - c0))"
' 2>/dev/null)"
echo "$out" | grep -q '^fail rc=1 checks=5 ' || fail "ws_wait_for 5 with 1 s checks: $(echo "$out" | grep '^fail')"
t="$(echo "$out" | sed -n 's/^fail .* t0=\([0-9.]*\) t1=\([0-9.]*\)$/\1 \2/p')"
awk -v t="$t" 'BEGIN { split(t, a, " "); exit !(a[2] - a[1] <= 6.2) }' || fail "ws_wait_for 5 took '$t' on the fake clock, want at most 6.2 s"
echo "$out" | grep -qx 'ok rc=0 checks=3' || fail "ws_wait_for did not return as soon as the check passed: $(echo "$out" | grep '^ok')"
echo "$out" | grep -qx 'backstop rc=1 settles=5' ||   # the backstop counts time slept: 5 settles of 0.2 s
  fail "unreadable clock: $(echo "$out" | grep '^backstop'), want the 1 s backstop (5 settles of 0.2 s)"
# ws_settle without usleep: a fractional sleep first, whole seconds only if that is refused
if PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin command -v usleep >/dev/null 2>&1; then
  echo "  (settle fallback part skipped: this host has a real usleep)"
else
  : > "$STUBLOG"
  env "${ARGS[@]}" PATH="$T/stubs-nousleep:/usr/bin:/bin" "${TEST_SH:-/bin/sh}" -c '
    . "$WS_APP/lib.sh"; ws_settle 200000; ws_settle 1500000; export SLEEP_NO_FRACTION=1; ws_settle 200000' 2>/dev/null
  [ "$(tr '\n' ' ' < "$STUBLOG")" = "sleep 0.200000 sleep 1.500000 sleep 0.200000 sleep 1 " ] ||
    fail "ws_settle without usleep slept: $(tr '\n' ' ' < "$STUBLOG")"
  # Whole-second settles count 1 s each against the no-clock backstop: a 2 s wait sleeps 2 s (a pass count, 40).
  : > "$STUBLOG"
  env "${ARGS[@]}" PATH="$T/stubs-nousleep:/usr/bin:/bin" SLEEP_NO_FRACTION=1 "${TEST_SH:-/bin/sh}" -c '
    . "$WS_APP/lib.sh"; never() { return 1; }
    mv "$WS_PROC/uptime" "$WS_PROC/uptime.away"; ws_wait_for 2 never; mv "$WS_PROC/uptime.away" "$WS_PROC/uptime"' 2>/dev/null
  [ "$(count "sleep 1")" = 2 ] || fail "a 2 s wait without a clock or fractions slept $(count "sleep 1") whole seconds, want 2"
fi

setup guard_cap_counts_the_clock_not_passes
# Every MainPID query costs 1 s here, so each pass of a stuck guard takes 2 s: the 10 s cap is hit after 5 passes.
echo activating > "$T/units/xochitl.state"
printf '#!/bin/sh\n"$WST/lib/advance" 1\n' > "$T/hooks/mainpid_query"; chmod +x "$T/hooks/mainpid_query"
ENVX=(WS_GUARD_MAX_S=10)
run "$APP/launch"
rc_is 12
count_is "sleep 1" 5; nowant "systemctl stop xochitl"; jwant "did not settle within 10 s"; gone; common

setup guard_cap_without_a_clock_counts_seconds_slept
# /proc/uptime cannot be read: the 10 s cap is then 10 s of sleep (one `sleep 1` per pass), not 20 passes.
echo activating > "$T/units/xochitl.state"; mv "$PROC/uptime" "$PROC/uptime.away"
ENVX=(WS_GUARD_MAX_S=10)
run "$APP/launch"
rc_is 12
count_is "sleep 1" 10; nowant "systemctl stop xochitl"; jwant "did not settle within 10 s"; gone; common

# launch was SIGKILLed and its app is stuck where no signal reaches it; the tablet has no usleep. cleanup post
# must still reach xochitl's and tripletap's start, with step 2 ending WS_STOP_DEADLINE_S (20 s) into it: on the
# boot clock, or, where /proc/uptime cannot be read, in time slept (3 + 10 + 7 s), whether sleep takes a fraction
# or only whole seconds. (Uncapped, the no-clock waits slept 132 s, or 660 s in whole seconds: past systemd's
# final SIGKILL, so xochitl would never start.)
for variant in clock no-clock no-clock-no-fraction; do
  setup "cleanup_bounded_with_a_stuck_app_and_no_usleep ($variant)"
  if PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin command -v usleep >/dev/null 2>&1; then
    echo "  (skipped: this host has a real usleep)"; continue
  fi
  rm -f "$T/stubs/usleep"
  sh -c '/bin/sleep 0.05 & echo "$!" > "$1"; exec /bin/sleep 120' sh "$T/zombie.pid" &
  ZPARENT=$!; echo "$ZPARENT" >> "$T/pids"        # the zombie's parent; ending it ends both
  sleep 0.4; Z="$(cat "$T/zombie.pid" 2>/dev/null)"
  if [ -z "$Z" ] || ! kill -0 "$Z" 2>/dev/null; then
    fail "could not make a zombie to stand in for a stuck app"
  else
    mkdir -p "$PROC/$Z"; printf '%s\000' "$APP/bin/wifi-signin-wpeqt" > "$PROC/$Z/cmdline"
    mkdir -p "$RUN/ovl/upper"; echo 999999 > "$RUN/pid"
    printf 'WAKELOCK\nTRIPLETAP\nSTOPPED_XOCHITL\nMOUNTED\nAPP=%s\n' "$Z" > "$RUN/state"
    echo "overlay $LIBEXEC overlay rw,lowerdir=$LIBEXEC,upperdir=$RUN/ovl/upper,workdir=$RUN/ovl/work 0 0" > "$PROC/mounts"
    echo inactive > "$T/units/xochitl.state"; echo 0 > "$T/units/xochitl.mainpid"; rm -rf "$PROC/4242"
    echo inactive > "$T/units/xovi-tripletap.state"
    case $variant in no-clock*) mv "$PROC/uptime" "$PROC/uptime.away" ;; esac
    case $variant in *no-fraction) ENVX=(SLEEP_NO_FRACTION=1) ;; esac
    run "$APP/cleanup" post
    rc_is 0
    in_order "umount $LIBEXEC" "wifi-signin-exit 30000000000" "wifi-signin" "systemctl reset-failed xochitl" \
      "systemctl start xochitl" "systemctl start xovi-tripletap"
    slept="$(sed -n 's/^slept at umount: //p' "$STUBLOG" | head -n 1)"
    awk -v t="$slept" 'BEGIN { exit !(t != "" && t >= 19.8 && t <= 20.2) }' ||
      fail "cleanup step 2 slept ${slept:-?} s, want WS_STOP_DEADLINE_S = 20 (+/- one 0.2 s poll)"
    if [ "$variant" = clock ]; then
      at="$(sed -n 's/^clock at umount: //p' "$STUBLOG" | head -n 1)"
      awk -v t="$at" 'BEGIN { exit !(t != "" && t - 1000 <= 20.2) }' ||
        fail "cleanup step 2 took $(awk -v t="$at" 'BEGIN { print t - 1000 }') s on the fake clock, want at most 20 (+ one 0.2 s poll)"
    fi
    case $variant in
      *no-fraction) want "sleep 1" ;;                   # every settle rounded up to a whole second
      *) want "sleep 0.200000"; nowant_re '^sleep [0-9]+$' ;;   # fractional settles, never rounded up
    esac
    jwant "survived kill -9"; gone
    if grep HAZARD "$STUBLOG" | grep -qv "HAZARD: xochitl started while pid $Z is alive"; then
      fail "unexpected hazard: $(grep HAZARD "$STUBLOG" | head -n 2 | tr '\n' ';')"
    fi
    if grep -qv '^\[wifi-signin\] ' "$JOURNAL"; then fail "journal line without the prefix: $(grep -v '^\[wifi-signin\] ' "$JOURNAL" | head -n 2)"; fi
  fi
  { kill -9 "$ZPARENT"; wait "$ZPARENT"; } 2>/dev/null
done

setup manifest
python3 - "$SRC/external.manifest.json" <<'EOF' || fail "external.manifest.json is not the spec section 7 manifest"
import json, sys
want = {"name": "Wi-Fi Sign-in",
        "application": "/home/root/xovi/exthome/appload/wifi-signin/appload-entry.sh",
        "workingDirectory": "/home/root/xovi/exthome/appload/wifi-signin",
        "qtfb": False, "disablesWindowedMode": True, "aspectRatio": "original"}
got = json.load(open(sys.argv[1]))
sys.exit(0 if got == want else 1)
EOF

teardown
if [ "$fails" = 0 ]; then echo "wifi_signin_launcher_test: OK ($ncase cases${TEST_SH:+, $TEST_SH})"
else echo "wifi_signin_launcher_test: $fails FAIL"; exit 1; fi
