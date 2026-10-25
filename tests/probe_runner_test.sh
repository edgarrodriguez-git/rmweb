#!/usr/bin/env bash
# Host integration test of the libsoup probe runner (engine/wpeqt/captive/probe_runner.h), driven through
# tools/probe-cli against the fake captive portal (tools/fake-portal), one fresh portal per case.
#
# Needs libsoup-3.0 (skips cleanly without it, except under CI), a GIO TLS backend (glib-networking) and an OpenSSL 3
# binary for the portal's TLS listeners. A missing TLS backend or a portal without TLS FAILS the test: CI must exercise
# TLS. So does a missing libsoup when CI or GITHUB_ACTIONS is set: a skip there would read as a pass.
# Portable to macOS's bash 3.2. PROBE_CLI_CXXFLAGS adds compiler flags (e.g. "-fsanitize=address -g").
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

if ! pkg-config --exists libsoup-3.0 2>/dev/null; then
  if [ -n "${CI:-}" ] || [ -n "${GITHUB_ACTIONS:-}" ]; then
    echo "probe runner: FAIL — libsoup-3.0 not found by pkg-config, and under CI this test must run, not skip"
    exit 1
  fi
  echo "[tests] libsoup-3.0 not found — skipping probe runner test"
  exit 0
fi

CLI=build/probe-cli
mkdir -p build
# shellcheck disable=SC2046,SC2086  # pkg-config output and the extra flags are meant to word-split
if ! clang++ -std=c++17 -Wall -Wextra ${PROBE_CLI_CXXFLAGS:-} -o "$CLI" tools/probe-cli/probe-cli.cpp \
     $(pkg-config --cflags --libs libsoup-3.0 gio-2.0); then
  echo "probe runner: FAIL (compile)"
  exit 1
fi

TMP="$(mktemp -d)"
PORTAL_PID=""
stop_portal(){
  if [ -n "$PORTAL_PID" ]; then
    kill "$PORTAL_PID" 2>/dev/null
    wait "$PORTAL_PID" 2>/dev/null
    PORTAL_PID=""
  fi
}
cleanup(){ stop_portal; rm -rf "$TMP"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

# Base port N such that N..N+6 are all free on 127.0.0.1: the portal takes N..N+5, N+6 stays closed (partial-closed).
cat > "$TMP/ports.py" <<'EOF'
import socket
for _ in range(500):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    base = probe.getsockname()[1]
    probe.close()
    if base + 6 > 65535:
        continue
    held = []
    try:
        for port in range(base, base + 7):
            s = socket.socket()
            held.append(s)
            s.bind(("127.0.0.1", port))
    except OSError:
        continue
    finally:
        for s in held:
            s.close()
    print(base)
    break
else:
    raise SystemExit("no 7 contiguous free ports")
EOF

now_ms(){ python3 -c 'import time; print(int(time.time() * 1000))'; }

grant(){ # POST /_control/grant on the portal's HTTP port (no proxy, whatever the environment says)
  python3 - "http://127.0.0.1:$P/_control/grant" <<'EOF'
import sys, urllib.request
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
opener.open(urllib.request.Request(sys.argv[1], data=b"", method="POST"), timeout=5).read()
EOF
}

# start_portal [fake_portal options...] — sets P (base port). Retries on a port race.
start_portal(){
  local attempt _
  for attempt in 1 2 3; do
    P="$(python3 "$TMP/ports.py")" || return 1
    python3 tools/fake-portal/fake_portal.py --bind 127.0.0.1 --base-port "$P" --cert-dir "$TMP/certs" "$@" \
      >"$TMP/portal.out" 2>"$TMP/portal.err" &
    PORTAL_PID=$!
    for _ in $(seq 1 300); do   # up to 30 s: the first start also makes the CA
      grep -q '^fake-portal: ready$' "$TMP/portal.out" && break
      kill -0 "$PORTAL_PID" 2>/dev/null || break
      sleep 0.1
    done
    if grep -q '^fake-portal: ready$' "$TMP/portal.out"; then
      if grep -q 'TLS ports are dead' "$TMP/portal.err"; then
        echo "probe runner: FAIL — the fake portal has no TLS listeners (needs an OpenSSL 3 binary):"
        cat "$TMP/portal.err"
        exit 1
      fi
      return 0
    fi
    stop_portal
    echo "  (portal start attempt $attempt on base port $P failed: $(tail -n 1 "$TMP/portal.err"))"
  done
  return 1
}

# run_cli [probe-cli args...] — sets OUT, RC, ELAPSED_MS.
run_cli(){
  local t0 t1
  t0="$(now_ms)"
  OUT="$("$CLI" "$@" 2>"$TMP/cli.err")"
  RC=$?
  t1="$(now_ms)"
  ELAPSED_MS=$((t1 - t0))
  if grep -q 'no TLS backend' "$TMP/cli.err"; then
    echo "probe runner: FAIL — GIO has no TLS backend, so libsoup cannot do HTTPS (install glib-networking)."
    echo "  The TLS cases must run, not skip. probe-cli said:"
    cat "$TMP/cli.err"
    exit 1
  fi
}

pass=0; total=0
EXTRA_FAIL=""
# check NAME WANT... — every WANT must be a substring of the CLI's single output line; EXTRA_FAIL, when set by the
# case, is one more failure reason.
check(){
  local name="$1" why="" want
  shift
  total=$((total + 1))
  [ "$RC" = 0 ] || why="exit code $RC"
  case "$OUT" in
    result=*) ;;
    *) why="${why:+$why; }output does not start with result=" ;;
  esac
  case "$OUT" in
    *$'\n'*) why="${why:+$why; }more than one output line" ;;
  esac
  for want in "$@"; do
    case "$OUT" in
      *"$want"*) ;;
      *) why="${why:+$why; }missing '$want'" ;;
    esac
  done
  [ -n "$EXTRA_FAIL" ] && why="${why:+$why; }$EXTRA_FAIL"
  if [ -z "$why" ]; then
    pass=$((pass + 1))
    printf '  ok   %-15s %s\n' "$name" "$OUT"
  else
    printf '  FAIL %-15s %s\n' "$name" "$OUT"
    echo "       reason: $why"
    sed 's/^/       cli stderr: /' "$TMP/cli.err"
    tail -n 5 "$TMP/portal.err" | sed 's/^/       portal: /'
  fi
  EXTRA_FAIL=""
}

# begin_case NAME [fake_portal options...] — starts the case's portal and sets the URLs; when the portal cannot
# start, the case counts as failed and the caller skips it.
begin_case(){
  CASE="$1"
  shift
  stop_portal
  if start_portal "$@"; then
    G="http://127.0.0.1:$P/generate_204"
    A="http://127.0.0.1:$P/hotspot-detect.html"
    CA="$TMP/certs/ca.pem"
    TLS_VALID="https://127.0.0.1:$((P + 2))/confirm"
    TLS_SELF="https://127.0.0.1:$((P + 3))/confirm"
    TLS_EXPIRED="https://127.0.0.1:$((P + 4))/confirm"
    TLS_NOTYET="https://127.0.0.1:$((P + 5))/confirm"
    CLOSED="https://127.0.0.1:$((P + 6))/confirm"
    return 0
  fi
  total=$((total + 1))
  echo "  FAIL $CASE: the fake portal did not start"
  cat "$TMP/portal.err"
  return 1
}
ROUTE=(--route-file tests/fixtures/route-default --iface test0)

if begin_case open --variant plain-form; then
  grant
  run_cli --google "$G" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  check open "result=Online " "g_addr=127.0.0.1 "
fi

if begin_case captive --variant plain-form; then
  run_cli --google "$G" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  check captive "result=Captive view=http://127.0.0.1:$P/generate_204 location=http://127.0.0.1:$P/portal/plain-form/ "
fi

if begin_case relative --variant relative-location; then
  run_cli --google "$G" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  check relative "result=Captive " "location=http://127.0.0.1:$P/portal/relative-location/ "
fi

if begin_case 511 --variant status-511; then
  run_cli --google "$G" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  check 511 "result=Captive view=http://127.0.0.1:$P/generate_204 "
fi

if begin_case limited --variant plain-form; then
  EMPTY="http://127.0.0.1:$P/raw?status=200&body=&len=0"
  run_cli --google "$EMPTY" --apple "$EMPTY" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  check limited "result=Limited "
fi

# A non-empty 2-byte body is Limited only through the declared Content-Length the runner reads (probe_runner.h).
if begin_case limited-cl --variant plain-form; then
  SHORT="http://127.0.0.1:$P/raw?status=200&body=ab&len=2"
  run_cli --google "$SHORT" --apple "$SHORT" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  check limited-cl "result=Limited "
fi

# G unreachable, A answers its success page: A alone is enough for Online, and G's address stays empty.
if begin_case a-only --variant plain-form; then
  grant
  run_cli --google "http://127.0.0.1:$((P + 6))/generate_204" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" \
    "${ROUTE[@]}"
  check a-only "result=Online " "g_addr= "
fi

# Any captive probe is conclusive (design §5): G captive, A granted.
if begin_case mixed --variant plain-form; then
  grant
  PORTAL="http%3A%2F%2F127.0.0.1%3A$P%2Fportal%2Fplain-form%2F"
  run_cli --google "http://127.0.0.1:$P/raw?status=302&location=$PORTAL" --apple "$A" --confirm "$TLS_VALID" \
    --ca "$CA" "${ROUTE[@]}"
  check mixed "result=Captive " "location=http://127.0.0.1:$P/portal/plain-form/ "
fi

if begin_case partial-self --variant plain-form; then
  grant
  run_cli --google "$G" --apple "$A" --confirm "$TLS_SELF" --ca "$CA" "${ROUTE[@]}"
  check partial-self "result=Partial "
fi

if begin_case clock --variant plain-form; then
  grant
  run_cli --google "$G" --apple "$A" --confirm "$TLS_EXPIRED" --ca "$CA" "${ROUTE[@]}"
  check clock "result=OnlineClockWrong "
fi

if begin_case clock-notyet --variant plain-form; then
  grant
  run_cli --google "$G" --apple "$A" --confirm "$TLS_NOTYET" --ca "$CA" "${ROUTE[@]}"
  check clock-notyet "result=OnlineClockWrong "
fi

if begin_case partial-closed --variant plain-form; then
  grant
  run_cli --google "$G" --apple "$A" --confirm "$CLOSED" --ca "$CA" "${ROUTE[@]}"
  check partial-closed "result=Partial "
fi

if begin_case noroute --variant plain-form; then
  grant
  run_cli --google "$G" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" --route-file tests/fixtures/route-none --iface test0
  check noroute "result=NoNetwork "
fi

# Both probes refused at once: NoNetwork without waiting out the 5 s deadline. The limit leaves room for the one 1 s
# SYN retry macOS can add when G and A hit the same closed port together (measured: ~1000 ms there, ~10 ms after).
if begin_case refused --variant plain-form; then
  run_cli --google "http://127.0.0.1:$((P + 6))/generate_204" --apple "http://127.0.0.1:$((P + 6))/hotspot-detect.html" \
    --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  [ "$ELAPSED_MS" -lt 2500 ] || EXTRA_FAIL="took ${ELAPSED_MS} ms, limit 2500 ms"
  check refused "result=NoNetwork "
  echo "       (returned in ${ELAPSED_MS} ms)"
fi

if begin_case slow --variant plain-form; then
  SLOW="http://127.0.0.1:$P/raw?status=204&delay_s=7"
  run_cli --google "$SLOW" --apple "$SLOW" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  [ "$ELAPSED_MS" -lt 6500 ] || EXTRA_FAIL="took ${ELAPSED_MS} ms, limit 6500 ms"
  check slow "result=NoNetwork "
  echo "       (returned in ${ELAPSED_MS} ms)"
fi

if begin_case skew --variant plain-form --date-offset-s 3600; then
  grant
  run_cli --google "$G" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  SKEW="$(printf '%s\n' "$OUT" | sed -n 's/.* skew=\(-\{0,1\}[0-9][0-9]*\)\( .*\)\{0,1\}$/\1/p')"   # more fields may follow
  if [ -z "$SKEW" ]; then EXTRA_FAIL="no numeric skew"
  elif [ "$SKEW" -lt 3595 ] || [ "$SKEW" -gt 3605 ]; then EXTRA_FAIL="skew $SKEW not within 3600±5"
  fi
  check skew "skew="
fi

# A captive portal's own Date reflects the portal's clock: only a 204 from G may carry the clock check (design §5).
if begin_case skew-captive --variant plain-form --date-offset-s 3600; then
  run_cli --google "$G" --apple "$A" --confirm "$TLS_VALID" --ca "$CA" "${ROUTE[@]}"
  check skew-captive "result=Captive " " skew=unknown"
fi

stop_portal
if [ "$pass" = 17 ] && [ "$total" = 17 ]; then
  echo "probe runner: 17/17 OK"
else
  echo "probe runner: FAIL ($pass/$total passed, 17 expected)"
  exit 1
fi
