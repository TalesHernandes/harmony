#!/usr/bin/env bash
# Checks the NVIDIA Video Codec SDK (approved by the owner, 05/10/2026, W1).
# It cannot be fetched by a script: NVIDIA asks for a developer login. The owner
# downloads it from https://developer.nvidia.com/video-codec-sdk and leaves the
# zip in $HARMONY_DEPS/nvidia (default ~/harmony-deps/nvidia); this unpacks it
# after checking the pinned checksum.
# What we use: Interface/nvEncodeAPI.h and the NvEncoder D3D11 sample classes
# (MIT headers). Nothing from the SDK ships beside the .exe — the encoder itself
# is in every NVIDIA driver. Its licence asks for the line "This software
# contains source code provided by NVIDIA Corporation." in the program's notices.
set -euo pipefail
VERSION=13.0.37
SHA256=828c5382a9999191eebd26fc5e3c62dc545aeccd95b6e307d9f7de5103889954
deps=${HARMONY_DEPS:-$HOME/harmony-deps}
dir="$deps/nvidia"
zip="$dir/Video_Codec_SDK_$VERSION.zip"
dest="$dir/Video_Codec_SDK_$VERSION"
if [ -f "$dest/Interface/nvEncodeAPI.h" ]; then
	echo "Video Codec SDK $VERSION already present in $dest"
	exit 0
fi
if [ ! -f "$zip" ]; then
	echo "Missing $zip — download Video Codec SDK $VERSION from developer.nvidia.com/video-codec-sdk (login needed) and put the zip there." >&2
	exit 1
fi
echo "$SHA256  $zip" | sha256sum -c --status || { echo "checksum mismatch: $zip is not SDK $VERSION as pinned" >&2; exit 1; }
unzip -q -o "$zip" -d "$dir"
echo "unpacked Video Codec SDK $VERSION into $dest"
