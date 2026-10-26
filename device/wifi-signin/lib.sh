# shellcheck shell=sh
# wifi-signin launcher library (POSIX sh: the tablet's /bin/sh is bash or BusyBox ash). Sourced by
# appload-entry.sh, launch and cleanup; never run on its own. Spec: docs/wifi-signin/design.md section 6.
#
# Every path and timing comes from the environment, so tests/wifi_signin_launcher_test.sh can point them at a
# fake /proc, /sys/power, /run and /usr/libexec on the host. Functions keep their scratch variables under the
# _ws_ prefix (POSIX sh has no `local`).

ws_defaults() { # every setting that is unset or empty gets its default (ws_validate_numbers runs it again)
  : "${WS_RUN:=/run/wifi-signin}"
  : "${WS_APP:=/home/root/xovi/exthome/appload/wifi-signin}"
  : "${WS_RUNTIME:=/home/root/.vellum/share/rmweb-runtime}"
  : "${WS_USERDIR:=/home/root/.wifi-signin}"
  : "${WS_SYSPOWER:=/sys/power}"
  : "${WS_PROC:=/proc}"
  : "${WS_EPD_LOCK:=/tmp/epframebuffer.lock}"
  : "${WS_CARD:=/dev/dri/card0}"
  : "${WS_LIBEXEC:=/usr/libexec}"
  : "${WS_GUARD_S:=30}"            # xochitl must have run this long before we stop it
  : "${WS_GUARD_MAX_S:=180}"       # give up (exit 12, xochitl untouched) if it never settles: about 4 restarts
  : "${WS_CARD_WAIT_S:=5}"         # after the stop, how long the panel may stay held
  : "${WS_READY_TIMEOUT_S:=30}"    # start-up watchdog: no ready file this long after exec -> SIGKILL (0 disables)
  : "${WS_RETRY_WINDOW_S:=20}"     # an early failure within this window may be retried once
  : "${WS_REFRESH_S:=60}"          # the 15 min wakelock is rewritten this often while the app runs
  : "${WS_APP_TERM_WAIT_S:=3}"     # cleanup: how long the app gets to drain after TERM
  : "${WS_HELPER_WAIT_S:=10}"      # cleanup and retry: how long the app and WPE helpers get to disappear
  : "${WS_STOP_DEADLINE_S:=20}"    # cleanup step 2 as a whole ends this long after it starts, whatever the app does
  : "${WS_SYSTEMD_RUN:=systemd-run}" # what appload-entry.sh runs (the host tests point it at a missing path)
}
ws_defaults
export WS_RUN WS_APP WS_RUNTIME WS_USERDIR WS_SYSPOWER WS_PROC WS_EPD_LOCK WS_CARD WS_LIBEXEC WS_GUARD_S \
  WS_GUARD_MAX_S WS_CARD_WAIT_S WS_READY_TIMEOUT_S WS_RETRY_WINDOW_S WS_REFRESH_S WS_APP_TERM_WAIT_S \
  WS_HELPER_WAIT_S WS_STOP_DEADLINE_S WS_SYSTEMD_RUN

# Under systemd the environment is minimal: make sure the standard directories are searched. Appended, so a
# caller's own PATH (the host tests' stub directory) keeps priority.
PATH="${PATH:+$PATH:}/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export PATH

# ws_log MSG: one journal line (stdout of a systemd service goes to the journal).
ws_log() { printf '[wifi-signin] %s\n' "$*"; }

# ws_validate_numbers: launch and cleanup run it right after launch.env. A number of seconds that is not plain
# decimal (empty, "20s", "abc", or a leading 0 that $(( )) reads as octal) goes back to its default, with a WARN:
# dash and BusyBox ash abort the whole script on $(( 20s * 100 )), and cleanup must still reach xochitl's start.
# shellcheck disable=SC2154  # _ws_v and _ws_d are assigned through eval
ws_validate_numbers() {
  for _ws_k in WS_GUARD_S WS_GUARD_MAX_S WS_CARD_WAIT_S WS_READY_TIMEOUT_S WS_RETRY_WINDOW_S WS_REFRESH_S \
      WS_APP_TERM_WAIT_S WS_HELPER_WAIT_S WS_STOP_DEADLINE_S; do
    eval "_ws_v=\${$_ws_k-}"
    case $_ws_v in '' | *[!0-9]* | 0?*) ;; *) continue ;; esac
    eval "$_ws_k="
    ws_defaults
    eval "_ws_d=\$$_ws_k"
    ws_log "WARN: $_ws_k='$_ws_v' is not a whole number of seconds: using $_ws_d"
  done
}

# ws_state_add TOKEN / ws_state_has TOKEN: the session's undo list, one token per line in $WS_RUN/state.
# Tokens: WAKELOCK, TRIPLETAP, STOPPED_XOCHITL, MOUNTED, APP=<pid>. ws_state_add fails quietly (a full or
# broken /run); every caller refuses rather than act unrecorded.
ws_state_add() { { printf '%s\n' "$1" >> "$WS_RUN/state"; } 2>/dev/null; }
ws_state_has() { grep -qxF -- "$1" "$WS_RUN/state" 2>/dev/null; }

# ws_settle MICROSECONDS: usleep; where there is none, a fractional sleep (upstream device/rmweb relies on
# `sleep 0.5` on this tablet); only if that fails too, sleep rounded up to whole seconds.
ws_settle() {
  if command -v usleep >/dev/null 2>&1; then
    usleep "$1"
    return
  fi
  _ws_us=$(( $1 % 1000000 + 1000000 ))           # 1000000-1999999: drop the leading 1 for six digits
  sleep "$(( $1 / 1000000 )).${_ws_us#1}" 2>/dev/null || sleep $(( ($1 + 999999) / 1000000 ))
}

# ws_wait_cs CENTISECONDS FUNCTION: polls FUNCTION every 0.2 s until it succeeds. Returns 1 once the boot clock
# is past the deadline (its value at the start + CENTISECONDS) with FUNCTION still failing, however long each
# check or settle takes. FUNCTION is tried at least once, and again after the settle that crosses the deadline.
# A pass count (one pass per 5 cs, i.e. 4x the nominal polls) is only a backstop for an unreadable clock.
ws_wait_cs() {
  _ws_left=$(( $1 / 5 ))
  if _ws_end=$(ws_uptime_cs); then _ws_end=$(( _ws_end + $1 )); else _ws_end=; fi
  while ! "$2"; do
    [ "$_ws_left" -gt 0 ] || return 1
    if [ -n "$_ws_end" ] && _ws_now=$(ws_uptime_cs) && [ "$_ws_now" -ge "$_ws_end" ]; then return 1; fi
    _ws_left=$(( _ws_left - 1 ))
    ws_settle 200000
  done
  return 0
}

# ws_wait_for SECONDS FUNCTION: ws_wait_cs in whole seconds.
ws_wait_for() { ws_wait_cs $(( $1 * 100 )) "$2"; }

# ws_uptime_cs: the boot-time clock (first field of $WS_PROC/uptime, which counts suspended time too) in
# centiseconds.
ws_uptime_cs() {
  read -r _ws_up _ 2>/dev/null < "$WS_PROC/uptime" || return 1
  _ws_int=${_ws_up%%.*}
  case $_ws_up in *.*) _ws_frac=${_ws_up#*.}00 ;; *) _ws_frac=00 ;; esac
  _ws_frac=${_ws_frac%"${_ws_frac#??}"}          # first two digits; "1$x - 100" keeps 08 from reading as octal
  case $_ws_int$_ws_frac in '' | *[!0-9]*) return 1 ;; esac
  echo $(( _ws_int * 100 + 1$_ws_frac - 100 ))
}

# ws_cmdline PID: the process's command line, NULs turned into spaces (for log lines).
ws_cmdline() { tr '\000' ' ' 2>/dev/null < "$WS_PROC/$1/cmdline"; }

# ws_card_holder: prints the first PID with an fd linked to $WS_CARD; returns 1 if there is none. One ls over
# every fd link (a readlink per fd would fork thousands of times per poll).
ws_card_holder() {
  # shellcheck disable=SC2012  # fd entries are numbers; the link targets are compared whole
  ls -ld "$WS_PROC"/[0-9]*/fd/* 2>/dev/null | awk -v proc="$WS_PROC/" -v card="$WS_CARD" '
    { a = index($0, " -> ")
      if (a == 0 || substr($0, a + 4) != card) next
      p = index($0, proc); if (p == 0) next
      rest = substr($0, p + length(proc)); s = index(rest, "/"); if (s < 2) next
      print substr(rest, 1, s - 1); found = 1; exit }
    END { exit !found }'
}

# ws_helper_pids: live PIDs whose $WS_PROC/<pid>/cmdline contains wpe-webkit-2.0/WPE (WPEWebProcess,
# WPENetworkProcess, WPEGPUProcess). The bracket stops grep from matching its own command line, which holds
# the pattern too.
ws_helper_pids() {
  grep -l 'wpe-webkit-2\.0/WP[E]' "$WS_PROC"/[0-9]*/cmdline 2>/dev/null |
    while IFS= read -r _ws_f; do
      _ws_p=${_ws_f%/cmdline}; _ws_p=${_ws_p##*/}
      if kill -0 "$_ws_p" 2>/dev/null; then echo "$_ws_p"; fi
    done
  return 0
}

# ws_app_alive PID: the recorded app PID is still the app. A recorded PID may have been reused by another
# process since (a stale session), and a zombie's command line is empty: neither may be signalled as the app.
ws_app_alive() {
  kill -0 "$1" 2>/dev/null && grep -qF wifi-signin-wpeqt "$WS_PROC/$1/cmdline" 2>/dev/null
}

# ws_xochitl_age: xochitl's age in whole seconds on the boot-time clock. MainPID from systemd; the start time
# is field 22 of $WS_PROC/<pid>/stat (USER_HZ = 100 ticks since boot, so ticks are centiseconds), counted after
# the LAST ')' because the command name may itself contain ')'; now is the first field of $WS_PROC/uptime. The
# difference is taken in centiseconds and floored once (flooring both values first could call 29.01 s 30).
# Never compared with ActiveEnterTimestampMonotonic, which stops while the device is suspended. Returns 1 if it
# cannot tell.
ws_xochitl_age() {
  _ws_pid=$(systemctl show -p MainPID --value xochitl 2>/dev/null)
  case $_ws_pid in '' | 0 | *[!0-9]*) return 1 ;; esac
  _ws_stat=$(cat "$WS_PROC/$_ws_pid/stat" 2>/dev/null) || return 1
  _ws_start=$(printf '%s\n' "${_ws_stat##*')'}" | awk '{ print $20 }')
  case $_ws_start in '' | *[!0-9]*) return 1 ;; esac
  _ws_now=$(ws_uptime_cs) || return 1
  echo $(( (_ws_now - _ws_start) / 100 ))
}

# ws_exit_meaning CODE: the same strings as exitMeaning() in engine/wpeqt/captive/exit_codes.h.
ws_exit_meaning() {
  case $1 in
    20) echo "online" ;;
    21) echo "cancelled" ;;
    22) echo "idle timeout" ;;
    23) echo "no network" ;;
    24) echo "use network as is" ;;
    0) echo "SIGTERM drain" ;;
    1) echo "touch grab failed" ;;
    2 | 3) echo "start-up failure" ;;
    63) echo "GUI watchdog" ;;
    11) echo "already running" ;;
    12) echo "display busy" ;;
    13) echo "systemd-run missing" ;;
    *)
      if [ "$1" -ge 128 ] 2>/dev/null; then
        echo "killed by signal $(( $1 - 128 ))"
      else
        echo "unknown"
      fi ;;
  esac
}

# ws_wakelock SPEC / ws_wakeunlock NAME: the kernel's wakelock interface ("name timeout_ns" holds a timed lock;
# the kernel only ever extends its timeout, so a shorter one needs another name). Appends rather than truncates:
# on sysfs that is the same write (the store ignores the offset), and it lets the host tests log every write in
# order.
ws_wakelock() { { printf '%s\n' "$1" >> "$WS_SYSPOWER/wake_lock"; } 2>/dev/null; }
ws_wakeunlock() { { printf '%s\n' "$1" >> "$WS_SYSPOWER/wake_unlock"; } 2>/dev/null; }
