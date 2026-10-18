#!/bin/sh
# Download the prebuilt wgpu-native static library used by the GPU renderer (HP_GS=gpu).
# Usage: scripts/fetch_wgpu.sh [macos|ios|ios-sim]
#   macos    third_party/wgpu-native/lib          (macOS arm64)
#   ios      third_party/wgpu-native/lib-ios      (iPhone/iPad arm64)
#   ios-sim  third_party/wgpu-native/lib-ios-sim  (iOS Simulator arm64)
# The archive goes into a fresh temporary directory and must match the release's SHA-256.
set -e
version=v29.0.1.1
target=${1:-macos}
case "$target" in
macos)   name=wgpu-macos-aarch64-release; dir=third_party/wgpu-native/lib
         sum=a5797a37b1adf720bcd5dcffb291edbbd5b7b14be0a3874c28e6393a655a7a3e ;;
ios)     name=wgpu-ios-aarch64-release; dir=third_party/wgpu-native/lib-ios
         sum=e36c9913b9e5095a530fa9121c50b16a4e3dd020e1eebf601f2f47ce24d56941 ;;
ios-sim) name=wgpu-ios-aarch64-simulator-release; dir=third_party/wgpu-native/lib-ios-sim
         sum=750e706765bef3744313745194774d095c916fc21d2a0e7d4d7b0bc4d0c92789 ;;
*) echo "unknown target $target" >&2; exit 1 ;;
esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/wgpu.zip" "https://github.com/gfx-rs/wgpu-native/releases/download/$version/$name.zip"
got=$(shasum -a 256 "$tmp/wgpu.zip" | cut -d' ' -f1)
if [ "$got" != "$sum" ]; then echo "$name.zip: SHA-256 $got, expected $sum" >&2; exit 1; fi
mkdir -p "$dir"
unzip -o -q "$tmp/wgpu.zip" -d "$tmp/x"
cp "$(find "$tmp/x" -name libwgpu_native.a | head -1)" "$dir/"
echo "wgpu-native $version ($target) in $dir"
