#!/usr/bin/env bash
# Fetches Shiguredo's libwebrtc build for Windows x64 (approved by the owner,
# 05/10/2026, W1) and checks it against the pinned checksum. It is a static
# library built with Microsoft's STL and /MT, so it links into the .exe and
# nothing from here ships beside it except the NOTICE text.
# Lives outside the repository (751 MB zip, ~740 MB unpacked): the repository is
# inside OneDrive, which would upload it. HARMONY_DEPS overrides the place.
set -euo pipefail
VERSION=m155.8059.4.1
SHA256=f661b9e66112b6bb2377d3ead64de46af8352607429daa4439f025a6efb1d0bf
deps=${HARMONY_DEPS:-$HOME/harmony-deps}
dest="$deps/webrtc-$VERSION"
if [ -f "$dest/webrtc/lib/webrtc.lib" ] && [ -f "$dest/VERSION" ]; then
	echo "libwebrtc $VERSION already present in $dest"
	exit 0
fi
mkdir -p "$deps/dl"
zip="$deps/dl/webrtc-$VERSION.zip"
if ! { [ -f "$zip" ] && echo "$SHA256  $zip" | sha256sum -c --status; }; then
	curl -fsSL -o "$zip.part" \
		"https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/$VERSION/webrtc.windows_x86_64.zip"
	echo "$SHA256  $zip.part" | sha256sum -c --status || { echo "checksum mismatch" >&2; rm -f "$zip.part"; exit 1; }
	mv "$zip.part" "$zip"
fi
rm -rf "$dest.part"
mkdir -p "$dest.part"
unzip -q "$zip" -d "$dest.part"
echo "$VERSION" >"$dest.part/VERSION"
rm -rf "$dest"
mv "$dest.part" "$dest"
echo "fetched libwebrtc $VERSION into $dest"
