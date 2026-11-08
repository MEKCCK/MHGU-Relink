#!/bin/bash
# FFmpeg mÃ­nimo (MP4 + H.264 + AAC, como el Makefile del port) estÃ¡tico para Android.
# Uso: bash build_ffmpeg.sh [abi]
set -e
mkdir -p "$(dirname "$0")/tmp"; export TMPDIR="$(cd "$(dirname "$0")" && pwd)/tmp" TEMP= TMP=
ABI=${1:-arm64-v8a}
API=29
case $ABI in
  arm64-v8a) ARCH=aarch64; TRIPLE=aarch64-linux-android ;;
  x86_64)    ARCH=x86_64;  TRIPLE=x86_64-linux-android ;;
esac
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-${LOCALAPPDATA:+$LOCALAPPDATA/Android/Sdk}}}"
[ -n "$SDK" ] && [ -d "$SDK" ] || SDK="$HOME/Android/Sdk"
[ -d "$SDK" ] || { echo "build_ffmpeg.sh: no Android SDK (set ANDROID_HOME)"; exit 1; }
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.0.12077973}"
# MSYS2 on Windows, or a Linux host (the NDK ships both toolchain trees)
HOSTTAG=windows-x86_64
[ "$(uname -s)" = Linux ] && HOSTTAG=linux-x86_64
m() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf %s "$1"; fi; }
TCB="$NDK/toolchains/llvm/prebuilt/$HOSTTAG"
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/out/$ABI/ffmpeg"
BUILD="$HERE/out/$ABI/ffmpeg-build"
[ -f "$OUT/.built" ] && { echo "ffmpeg $ABI: ya estÃ¡"; exit 0; }
rm -rf "$BUILD" && mkdir -p "$BUILD" "$OUT"
export PATH="$TCB/bin:$NDK/prebuilt/$HOSTTAG/bin:$PATH"
cd "$BUILD"
"$(m "$HERE/src/ffmpeg")/configure" --prefix="$(m "$OUT")" \
  --enable-cross-compile --target-os=android --arch=$ARCH \
  --cc="clang --target=$TRIPLE$API" --cxx="clang++ --target=$TRIPLE$API" \
  --host-cc="clang --target=$TRIPLE$API" --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip \
  --sysroot="$(m "$TCB/sysroot")" --enable-pic \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov \
  --enable-parser=h264,aac --enable-decoder=h264,aac \
  --extra-cflags="-ffile-prefix-map=$(m "$HERE/src/ffmpeg")=ffmpeg" \
  $( [ $ARCH = x86_64 ] && echo --disable-x86asm ) > configure.log 2>&1 || { tail -30 configure.log; tail -40 ffbuild/config.log; exit 1; }
sed -i 's|^#define FFMPEG_CONFIGURATION .*|#define FFMPEG_CONFIGURATION ""|' config.h  # (no paths of this machine in the library)
# (Windows make needs drive-qualified paths rather than MSYS paths.)
if command -v cygpath >/dev/null 2>&1; then
  DRIVE=$(m "$HERE" | cut -d: -f1)
  DRIVE_LOWER=$(printf "%s" "$DRIVE" | tr "[:upper:]" "[:lower:]")
  sed -i "s|/$DRIVE_LOWER/|$DRIVE:/|g" Makefile ffbuild/config.mak
  export TMPDIR="$(m "$TMPDIR")" TMP="$(m "$TMPDIR")" TEMP="$(m "$TMPDIR")"
fi
make -j12 > build.log 2>&1 || { tail -40 build.log; exit 1; }
mkdir -p "$OUT/lib"
for l in libavformat libavcodec libavutil; do cp "$BUILD/$l/$l.a" "$OUT/lib/"; done
# (la instalación de headers falla con el make de Windows: se copian)
for l in libavutil libavcodec libavformat; do
  mkdir -p "$OUT/include/$l"; cp "$HERE/src/ffmpeg/$l/"*.h "$OUT/include/$l/"; cp "$BUILD/$l/"*.h "$OUT/include/$l/" 2>/dev/null || true
done
touch "$OUT/.built"
echo "ffmpeg $ABI: ok"
