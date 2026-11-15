#!/usr/bin/env bash
# Fetches Shiguredo's libwebrtc AAR (the WebRTC engine, arm64 only) into
# app/libs and checks it against the pinned checksum. Run once after cloning,
# and again after bumping VERSION/SHA256 (the release page lists both).
set -euo pipefail
VERSION=155.8059.1.0
SHA256=65ebb4faf70d32b606d2132ca2f327aa92c365ac8928278cfc7b51b8a9407fc4
android_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$android_dir/app/libs"
out=libwebrtc-$VERSION.aar
if [ -f "$out" ] && echo "$SHA256  $out" | sha256sum -c --status; then
  echo "$out already present (verified)"
else
  curl -fsSL -o "$out.part" \
    "https://github.com/shiguredo/shiguredo-webrtc-android/releases/download/$VERSION/libwebrtc.aar"
  echo "$SHA256  $out.part" | sha256sum -c --status || { echo "checksum mismatch" >&2; rm -f "$out.part"; exit 1; }
  mv "$out.part" "$out"
  echo "fetched $out"
fi
# Always verify/recompile the local patch, including on repeat invocations.
# Python uses only its standard library; javac comes from Android Studio's JBR.
python "$android_dir/webrtc-patch/build.py"
