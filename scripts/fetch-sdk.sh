#!/usr/bin/env bash
set -euo pipefail
# Download reMarkable's official "ferrari" aarch64 Yocto SDK installer into toolchain/sdk/.
# The installer is large (~497 MB) and gitignored; this script makes it reproducible.
# SHA-256 is pinned to the known-good build that produced our verified toolchain.
# 3.28.0.172 / 5.8.203 is the newest published SDK: no 3.29 SDK existed when this was pinned.
# The storage bucket reports md5 YyZX2wvtMWETPKx4FXsgNQ== (x-goog-hash, base64) for it; the SHA-256 below was
# recorded after that md5 matched.
URL="https://storage.googleapis.com/remarkable-codex-toolchain/3.28.0.172/remarkable-production-image-5.8.203-ferrari-public-aarch64-toolchain.sh"
SHA256="65e5b98f9f7c83d857c5c720f7f6cb2d61fe20f4513dec023ad00aac27c85ae4"
SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)/toolchain/sdk"
out="$SDK_DIR/$(basename "$URL")"

verify() {
  if command -v sha256sum >/dev/null 2>&1; then echo "$SHA256  $out" | sha256sum -c -
  else echo "$SHA256  $out" | shasum -a 256 -c -; fi
}

mkdir -p "$SDK_DIR"
if [ -f "$out" ]; then
  echo "Already present, verifying checksum…"
  verify || { echo "CHECKSUM FAILED — removing $out"; rm -f "$out"; exit 1; }
  exit 0
fi
echo "Downloading ferrari aarch64 SDK (~497 MB)…"
curl -fL --progress-bar -o "$out" "$URL"
echo "Verifying SHA-256…"
verify || { echo "CHECKSUM FAILED — removing $out"; rm -f "$out"; exit 1; }
echo "Saved & verified: $out ($(du -h "$out" | cut -f1))"
