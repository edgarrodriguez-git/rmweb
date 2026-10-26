#!/usr/bin/env bash
set -euo pipefail
# Build + run ALL pure-logic host unit tests (no device, no SDK). clang++ C++17.
cd "$(dirname "$0")/.."
mkdir -p build
fail=0
for t in tests/*_test.cpp; do
  name="$(basename "$t" .cpp)"
  if clang++ -std=c++17 -Wall -Wextra -o "build/$name" "$t"; then
    "./build/$name" || { echo "FAIL (runtime): $name"; fail=1; }
  else
    echo "FAIL (compile): $name"; fail=1
  fi
done
# Shell unit tests (launcher / installer no-brick logic) — pure bash + stubbed systemctl/mount.
for t in tests/*_test.sh; do
  [ -e "$t" ] || continue
  name="$(basename "$t" .sh)"
  if bash "$t"; then :; else echo "FAIL (shell): $name"; fail=1; fi
done
# Fake captive portal (tools/fake-portal) — Python stdlib unittest; its TLS cases skip without an OpenSSL 3 binary,
# except under CI, where a skip would read as a pass: WIFI_SIGNIN_REQUIRE_TLS makes the module fail instead.
if [ -n "${CI:-}" ] || [ -n "${GITHUB_ACTIONS:-}" ]; then export WIFI_SIGNIN_REQUIRE_TLS=1; fi
if python3 -m unittest discover -s tools/fake-portal -p 'test_*.py' 2>&1 | tee build/fake-portal-tests.log; then :
else echo "FAIL (python): fake-portal"; fail=1; fi
ran="$(sed -n -E 's/^Ran ([0-9]+) tests? in .*/\1/p' build/fake-portal-tests.log | tail -n 1)"
# The skip count comes from the last summary line only: RequireTlsTests' assertion messages can carry a
# subprocess's own "OK (skipped=N)", which must not stand in for a summary that has no skipped= of its own.
summary="$(sed -n -E '/^(OK|FAILED)( |$)/p' build/fake-portal-tests.log | tail -n 1)"
skipped="$(printf '%s\n' "$summary" | sed -n -E 's/.*skipped=([0-9]+).*/\1/p')"
echo "[tests] fake-portal unittest: ran ${ran:-0} tests, ${skipped:-0} skipped"
# Lint of the shipped shell: skips cleanly if shellcheck isn't installed, except under CI, where it fails.
if command -v shellcheck >/dev/null 2>&1; then
  for f in device/rmweb device/rmweb-env.sh device/install.sh device/wifi-signin/lib.sh \
      device/wifi-signin/appload-entry.sh device/wifi-signin/launch device/wifi-signin/cleanup; do
    [ -e "$f" ] && { shellcheck -s sh "$f" || { echo "FAIL (shellcheck): $f"; fail=1; }; }
  done
elif [ -n "${CI:-}" ] || [ -n "${GITHUB_ACTIONS:-}" ]; then
  echo "FAIL (shellcheck): shellcheck not found, and under CI the shell lint must run, not skip"; fail=1
else
  echo "[tests] shellcheck not found — skipping shell lint"
fi
if [ "$fail" = 0 ]; then echo "ALL HOST TESTS OK"; else echo "SOME TESTS FAILED"; exit 1; fi
