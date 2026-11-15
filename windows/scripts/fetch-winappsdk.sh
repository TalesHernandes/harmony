#!/usr/bin/env bash
# Fetches the Windows App SDK parts the screens use (approved by the owner,
# 06/10/2026, W1: "only the WinUI parts"), pinned by version and checksum,
# into $HARMONY_DEPS/nuget (default ~/harmony-deps/nuget), outside OneDrive.
# Not the umbrella Microsoft.WindowsAppSDK package: it drags in AI, ML and
# widgets that the program never uses. Redistributing the runtime beside the
# .exe is allowed by Microsoft's licence on condition that the program's own
# terms protect Microsoft at least as much (the Codex research, 05/10/2026).
# The last three are required by those packages to BUILD (found and approved
# by the owner on 06/10/2026): the SDK build tools (22 MB), the MSIX tools
# (3 MB) and the WebView2 SDK (9 MB). None of them ships beside the .exe.
# The build points NuGet at this folder (no network, nothing floating).
set -euo pipefail
deps=${HARMONY_DEPS:-$HOME/harmony-deps}
dir="$deps/nuget"
mkdir -p "$dir"
# id version sha256
PACKAGES="
microsoft.windowsappsdk.winui 2.3.9 d36145bd6c0f46fc04e07fb4af365d093fb42c39b625948d0ead07a0b26fce96
microsoft.windowsappsdk.foundation 2.3.12 ce04d01d68cb16b5dc85a5c9efb8162dba876bc2e5b766b5968979899d4efa9e
microsoft.windowsappsdk.interactiveexperiences 2.1.9 f9159d73e3e46717f159606e43b4acf8336aaff5bdd4d395fe3a93fdb41437ac
microsoft.windowsappsdk.base 2.0.4 e3e13478c4c80c59ed5f8f89542fe49a2985daa484753e93a5858e90c2d46a4d
microsoft.windowsappsdk.dwrite 2.1.0 91746abd6a780b84001f9c465507830d5a24c79912bc4aa0d35938c71c1c6b5f
microsoft.windows.cppwinrt 3.0.260818.1 a8993608ae9263a8288e1a1d9ee2684a5632f4acc59e1e89ddd86b3b889a7367
microsoft.windows.sdk.buildtools 10.0.26100.4654 5f5783658a200f83ea2addfd406e2303a901b27ad7c49b0347c34aefcc21883d
microsoft.windows.sdk.buildtools.msix 1.7.251221100 e61c560805029f5ae15433be52e153f3862b6faaf725292638a9ece592f7a060
microsoft.web.webview2 1.0.3719.77 2f6be3a10a1a8d6d1fde986af4131dab344f8181fffe75590824e0f4b037ed73
"
echo "$PACKAGES" | while read -r id version sha; do
	[ -n "$id" ] || continue
	file="$dir/$id.$version.nupkg"
	if [ ! -f "$file" ]; then
		curl -fsSL -o "$file.part" "https://api.nuget.org/v3-flatcontainer/$id/$version/$id.$version.nupkg"
		mv "$file.part" "$file"
	fi
	echo "$sha  $file" | sha256sum -c --status || { echo "checksum mismatch: $file" >&2; rm -f "$file"; exit 1; }
	echo "ok $id $version"
done
