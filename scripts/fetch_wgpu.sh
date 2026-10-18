#!/bin/sh
# Download the prebuilt wgpu-native static library used by the GPU renderer (HP_GS=gpu).
# Usage: scripts/fetch_wgpu.sh [macos|ios|ios-sim]
set -e
version=v29.0.1.1
target=${1:-macos}
case "$target" in
macos)   name=wgpu-macos-aarch64-release; dir=third_party/wgpu-native/lib ;;
ios)     name=wgpu-ios-aarch64-release; dir=third_party/wgpu-native/lib-ios ;;
ios-sim) name=wgpu-ios-aarch64-simulator-release; dir=third_party/wgpu-native/lib-ios-sim ;;
*) echo "unknown target $target" >&2; exit 1 ;;
esac
tmp=$(mktemp -d)
curl -fL -o "$tmp/wgpu.zip" "https://github.com/gfx-rs/wgpu-native/releases/download/$version/$name.zip"
mkdir -p "$dir"
unzip -o -q "$tmp/wgpu.zip" -d "$tmp/x"
cp "$(find "$tmp/x" -name libwgpu_native.a | head -1)" "$dir/"
rm -rf "$tmp"
echo "wgpu-native $version ($target) in $dir"
