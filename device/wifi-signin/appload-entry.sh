#!/bin/sh
# wifi-signin AppLoad entry point (external.manifest.json "application"). AppLoad runs it as a child of xochitl,
# so it must not stop xochitl itself: `systemctl stop xochitl` kills every process in xochitl's cgroup. It
# hands the session to a transient systemd SERVICE instead (not a scope), so ExecStopPost runs cleanup even if
# launch is killed with SIGKILL or by the OOM killer. Spec: docs/wifi-signin/design.md section 6.
set -u

WS_APP=${WS_APP:-/home/root/xovi/exthome/appload/wifi-signin}
# shellcheck source=/dev/null
. "$WS_APP/lib.sh"

# 1. A session is already running or still finishing (its cleanup runs while the unit deactivates).
state=$(systemctl show -p ActiveState --value wifi-signin 2>/dev/null)
case $state in
  inactive | failed | '') ;;
  *) ws_log "the wifi-signin unit is $state: a session is running or finishing"; exit 0 ;;
esac

# 2. No direct-exec fallback: inside xochitl's cgroup, stopping xochitl would kill the launcher itself.
#    ($WS_SYSTEMD_RUN is systemd-run; the host tests point it at a missing path, never at the host's own.)
if ! command -v "$WS_SYSTEMD_RUN" >/dev/null 2>&1; then
  ws_log "systemd-run is missing: not starting (xochitl untouched)"
  ws_log "exit 13 ($(ws_exit_meaning 13))"
  exit 13
fi

# 3.
exec "$WS_SYSTEMD_RUN" --unit=wifi-signin --collect --quiet \
  -p "ExecStopPost=-$WS_APP/cleanup post" -p TimeoutStopSec=30 -p MemoryAccounting=yes \
  "$WS_APP/launch"
