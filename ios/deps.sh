#!/bin/sh
# iOS dependencies for one SDK: ios/deps.sh iphonesimulator|iphoneos
#   deps/ios-<sdk>/{lib/libSDL3.a,include/SDL3}   SDL3 static library
#   third_party/wgpu-native/lib-ios[-sim]          wgpu-native (scripts/fetch_wgpu.sh)
# SDL3 is copied from a prebuilt install when one is found (SDL_PREFIX, else the sibling ports'
# deps/ios-<sdk>), otherwise built from the official libsdl-org release source with CMake.
set -e
SDK=${1:?usage: ios/deps.sh iphonesimulator|iphoneos}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case "$SDK" in
iphonesimulator) WGPU=ios-sim ;;
iphoneos)        WGPU=ios ;;
*) echo "unknown SDK $SDK" >&2; exit 1 ;;
esac
WDIR=$ROOT/third_party/wgpu-native/lib-$WGPU
[ -f "$WDIR/libwgpu_native.a" ] || (cd "$ROOT" && sh scripts/fetch_wgpu.sh $WGPU)

PREFIX=$ROOT/deps/ios-$SDK
[ -f "$PREFIX/lib/libSDL3.a" ] && exit 0
mkdir -p "$PREFIX/lib" "$PREFIX/include"
for p in "$SDL_PREFIX" "$HOME/nfsmw/deps/ios-$SDK" "$HOME/wolverine-/deps/ios-$SDK" "$HOME/cossacks2/deps/ios-$SDK"; do
    if [ -n "$p" ] && [ -f "$p/lib/libSDL3.a" ] && [ -f "$p/include/SDL3/SDL.h" ]; then
        cp "$p/lib/libSDL3.a" "$PREFIX/lib/"
        cp -R "$p/include/SDL3" "$PREFIX/include/"
        echo "SDL3 ($SDK) copied from $p"
        exit 0
    fi
done
VER=3.4.14
SUM=30d4aa2b3037718142b32dffd4e72f917ebb6cc5227150e7bb9c45efb2153aeb
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "building SDL3 $VER for $SDK from the release source"
curl -fsSL -o "$TMP/sdl.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$VER/SDL3-$VER.tar.gz"
GOT=$(shasum -a 256 "$TMP/sdl.tar.gz" | cut -d' ' -f1)
[ "$GOT" = "$SUM" ] || { echo "SDL3-$VER.tar.gz: SHA-256 $GOT, expected $SUM" >&2; exit 1; }
mkdir "$TMP/src"
tar -xzf "$TMP/sdl.tar.gz" -C "$TMP/src"
cmake -S "$TMP/src/SDL3-$VER" -B "$TMP/build" -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDK \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$TMP/build.log" 2>&1 \
    && cmake --build "$TMP/build" -j4 >> "$TMP/build.log" 2>&1 && cmake --install "$TMP/build" >> "$TMP/build.log" 2>&1 \
    || { tail -30 "$TMP/build.log"; exit 1; }
echo "SDL3 ($SDK) built into $PREFIX"
