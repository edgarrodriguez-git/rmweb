#!/usr/bin/env bash
# fast-build.sh — rebuild ONLY the app (engine/wpeqt) in minutes, reusing the prebuilt WPE WebKit /
# Mesa / soup runtime from the upstream rmweb release tarball instead of the multi-hour engine build
# (scripts/build-wpe.sh). Valid while engine/wpeqt keeps targeting WPE WebKit 2.48.x — the engine
# itself is NOT rebuilt, so engine changes still need the full build.
#
# Ported from rmbrowser (https://github.com/jhasubhash/rmbrowser, scripts/fast-build.sh), MIT,
# Copyright (c) 2026 Eugene Tarakanov and contributors. Differences from the original:
#   * `sdk` verifies both downloads (rmweb release tarball, WPE source tarball) and fails closed;
#     the generator container is pinned by digest instead of the floating debian:trixie tag.
#   * `app` compiles against the committed third_party/appsdk (not build/appsdk), writes
#     build/wifi-signin-wpeqt, and keeps the SDK's own $LDFLAGS (the original replaced them).
#   * The `package` and `all` modes are left out (packaging is a later task).
#
#   ./scripts/fast-build.sh sdk   # fetch + verify the 0.9.7 release and WPE 2.48.5 source, unpack the
#                                 # release into build/release/rmweb-0.9.7, generate headers (build/appsdk)
#   ./scripts/fast-build.sh app   # compile build/wifi-signin-wpeqt in the rmweb-sdk container
#
# Prereqs: Docker (linux/arm64) and the rmweb-sdk image (toolchain/README.md).
set -euo pipefail
cd "$(dirname "$0")/.."

BASE_VER=0.9.7      # upstream rmweb release whose runtime libs we reuse
BASE_SHA256=8df3e420ef02992b79e548ad6dd71c8c8324ba641e92477e7bd28707952c3e8e   # GitHub's recorded asset digest
WPE_VER=2.48.5
WPE_SHA256=01f36010705adb14404c56baf033147f7927cc7c6badec81bb141266fcdd8d0b   # = WPE_SHA256 in scripts/build-wpe.sh
# Header-generator container. Pinned by digest (index digest of debian:trixie = trixie-20261020,
# created 2026-10-20, resolved 2026-10-26 with `docker buildx imagetools inspect debian:trixie`);
# bump deliberately, not by chance.
DEBIAN_IMAGE=debian:trixie@sha256:4c25275446ca371766b8b093c696782eabce06263fccacb8d5d813b3dc44b308
REL=build/release/rmweb-$BASE_VER
OUT_BIN=build/wifi-signin-wpeqt

verify_sha256() { # file expected-sha256
  if command -v sha256sum >/dev/null 2>&1; then echo "$2  $1" | sha256sum -c -
  else echo "$2  $1" | shasum -a 256 -c -; fi
}

# fetch_verified <url> <dest> <sha256>: download only when <dest> is absent, then verify it before
# anyone uses it. A mismatch removes the file and fails.
fetch_verified() {
  local url="$1" dest="$2" sha="$3" tmp
  if [ -f "$dest" ]; then
    verify_sha256 "$dest" "$sha" || { echo "[fast] CHECKSUM FAILED — removing $dest" >&2; rm -f "$dest"; return 1; }
    return 0
  fi
  echo "[fast] fetching $url"
  tmp="$(mktemp "$dest.XXXXXX")"
  curl -fL "$url" -o "$tmp" || { rm -f "$tmp"; return 1; }
  verify_sha256 "$tmp" "$sha" || { echo "[fast] CHECKSUM FAILED — removing $tmp" >&2; rm -f "$tmp"; return 1; }
  mv "$tmp" "$dest"
}

sdk() {
  mkdir -p build/src build/release
  if [ ! -d "$REL/lib" ]; then
    local tb="build/release/rmweb-$BASE_VER.tar.gz"
    fetch_verified "https://github.com/exp78/rmweb/releases/download/v$BASE_VER/rmweb-$BASE_VER.tar.gz" \
                   "$tb" "$BASE_SHA256" || exit 1
    # Unpack beside the target and move into place, so an interrupted tar can't leave a half tree.
    rm -rf "$REL.partial" && mkdir -p "$REL.partial"
    tar -xzf "$tb" -C "$REL.partial" || { rm -rf "$REL.partial"; exit 1; }
    rm -rf "$REL" && mv "$REL.partial" "$REL"
  fi
  local src="build/src/wpewebkit-$WPE_VER.tar.xz"
  fetch_verified "https://wpewebkit.org/releases/wpewebkit-$WPE_VER.tar.xz" "$src" "$WPE_SHA256" || exit 1
  rm -rf build/src/wpesrc && mkdir -p build/src/wpesrc
  tar -xJf "$src" -C build/src/wpesrc --strip-components=1 \
      "wpewebkit-$WPE_VER/Source/WebKit/UIProcess/API" "wpewebkit-$WPE_VER/Source/WebKit/WPEPlatform" \
      "wpewebkit-$WPE_VER/Source/WebKit/Scripts/glib" "wpewebkit-$WPE_VER/Source/WebKit/PlatformWPE.cmake" \
      "wpewebkit-$WPE_VER/Source/JavaScriptCore/API/glib" "wpewebkit-$WPE_VER/Source/JavaScriptCore/GLib.cmake"
  docker run --rm --platform linux/arm64 -v "$PWD":/work "$DEBIAN_IMAGE" bash /work/engine/gen-app-sdk.incontainer.sh
}

app() {
  [ -d third_party/appsdk/usr/include ] || { echo "[fast] no third_party/appsdk — it is committed; restore it or run: $0 sdk" >&2; exit 1; }
  [ -d "$REL/lib" ] || { echo "[fast] no $REL — run: $0 sdk" >&2; exit 1; }
  docker run --rm -v "$PWD":/work -w /work -e REL="/work/$REL" -e OUT_BIN="/work/$OUT_BIN" rmweb-sdk bash -lc '
    set -e
    . /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux
    SR=$SDKTARGETSYSROOT
    cp -a /work/third_party/appsdk/usr/. "$SR/usr/"
    # Link against the release runtime: the libs we have headers for go into the sysroot (never
    # clobbering an SDK lib); the whole bundle lib/ is the rpath-link for their transitive deps.
    for l in libWPEWebKit-2.0 libWPEPlatform-2.0 libsoup-3.0 libwpe-1.0; do cp -an "$REL/lib/$l".so* "$SR/usr/lib/"; done
    rm -rf /work/build/wpeqt
    # Keep the SDK LDFLAGS (--as-needed and co.): setting CMAKE_EXE_LINKER_FLAGS replaces them otherwise.
    cmake -S /work/engine/wpeqt -B /work/build/wpeqt -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -Wl,-rpath-link,$REL/lib"
    cmake --build /work/build/wpeqt -j"$(nproc)"
    cp -f /work/build/wpeqt/rmweb-wpeqt "$OUT_BIN"
    ls -l "$OUT_BIN"'
}

case "${1:-}" in
  sdk) sdk ;;
  app) app ;;
  *) sed -n '15,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
