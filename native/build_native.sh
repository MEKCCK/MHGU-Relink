#!/bin/bash
# Compila libmain.so (runtime) y, si existe generated/, libmhgugame.so (código del juego) para Android.
# Uso: bash build_native.sh [abi] [Release|RelWithDebInfo]
set -e
ABI=${1:-arm64-v8a}
TYPE=${2:-Release}
API=29
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
SRC="$ROOT"
# The SDK: $ANDROID_HOME / $ANDROID_SDK_ROOT, else the usual Windows or Linux place
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
  if [ -n "$LOCALAPPDATA" ] && [ -d "$LOCALAPPDATA/Android/Sdk" ]; then SDK="$LOCALAPPDATA/Android/Sdk"
  elif [ -d "$HOME/Android/Sdk" ]; then SDK="$HOME/Android/Sdk"; fi
fi
[ -n "$SDK" ] && [ -d "$SDK" ] || { echo "build_native.sh: no Android SDK (set ANDROID_HOME)"; exit 1; }
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.0.12077973}"
[ -d "$NDK" ] || { echo "build_native.sh: no NDK 27.0.12077973 under $SDK/ndk (set ANDROID_NDK_HOME)"; exit 1; }
CMAKE="$SDK/cmake/3.22.1/bin/cmake"
NINJA="$SDK/cmake/3.22.1/bin/ninja"
if [ "$(uname -s)" = Linux ]; then
  [ -x "$CMAKE" ] || { CMAKE=cmake; NINJA=ninja; }
elif [ -x "$CMAKE.exe" ] && [ -x "$NINJA.exe" ]; then
  CMAKE="$CMAKE.exe"; NINJA="$NINJA.exe"
else
  CMAKE=cmake; NINJA=ninja
fi
OUT="$HERE/build/$ABI"
mkdir -p "$OUT"
m() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf %s "$1"; fi; }

# The runtime and generated game library share the pinned compatibility ID.
cd "$SRC"
GAMELIB_ID=$(tr -d '\r\n' < "$ROOT/android/app/src/main/assets/gamelib_id.txt")
[[ "$GAMELIB_ID" =~ ^[0-9a-f]{16}$ ]] || { echo "build_native.sh: invalid GameLib ID: $GAMELIB_ID"; exit 1; }
SHADER_ID=$(cat runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h | tr -d '\r' | sha256sum | cut -c1-16)
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo dev)"
# The Local Play link (runtime/net/ldn.c links it): crates/mhgu-link-ffi, for this ABI
case $ABI in
  arm64-v8a) RT=aarch64-linux-android ;;
  x86_64)    RT=x86_64-linux-android ;;
  *) echo "build_native.sh: unsupported ABI $ABI"; exit 1 ;;
esac
HTAG=windows-x86_64; [ "$(uname -s)" = Linux ] && HTAG=linux-x86_64
RUST_CC="$NDK/toolchains/llvm/prebuilt/$HTAG/bin/clang.exe"
[ -x "$RUST_CC" ] || { echo "build_native.sh: no NDK clang at $RUST_CC"; exit 1; }
case $RT in
  aarch64-*) CARGO_LINKER=CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER; CCVAR=CC_aarch64_linux_android; ARVAR=AR_aarch64_linux_android; CFLAGSVAR=CFLAGS_aarch64_linux_android; RUSTFLAGSVAR=CARGO_TARGET_AARCH64_LINUX_ANDROID_RUSTFLAGS ;;
         *) CARGO_LINKER=CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER; CCVAR=CC_x86_64_linux_android; ARVAR=AR_x86_64_linux_android; CFLAGSVAR=CFLAGS_x86_64_linux_android; RUSTFLAGSVAR=CARGO_TARGET_X86_64_LINUX_ANDROID_RUSTFLAGS ;;
esac
TARGET_FLAGS="--target=$RT$API"
LLVM_AR="$NDK/toolchains/llvm/prebuilt/$HTAG/bin/llvm-ar"
(cd "$ROOT" && /usr/bin/env "$CARGO_LINKER=$RUST_CC" "$CCVAR=$RUST_CC" "$ARVAR=$LLVM_AR" "$CFLAGSVAR=$TARGET_FLAGS" "$RUSTFLAGSVAR=-C link-arg=$TARGET_FLAGS" cargo build --locked --release --lib -p mhgu-link-ffi -p mhgu-crossplay --target "$RT" --target-dir "$(m "$ROOT/target")") \
  || { echo "build_native.sh: cargo build -p mhgu-link-ffi -p mhgu-crossplay --target $RT failed"; exit 1; }
MHGU_LINK_LIB="$ROOT/target/$RT/release/libmhgu_link_ffi.a"
MHGU_XP_LIB="$ROOT/target/$RT/release/libmhgu_crossplay.a"
[ -f "$MHGU_LINK_LIB" ] || { echo "build_native.sh: $MHGU_LINK_LIB is missing"; exit 1; }
[ -f "$MHGU_XP_LIB" ] || { echo "build_native.sh: $MHGU_XP_LIB is missing"; exit 1; }
cd "$HERE"

"$CMAKE" -S "$(m "$HERE")" -B "$(m "$OUT")" -G Ninja -DCMAKE_MAKE_PROGRAM="$(m "$NINJA")" \
  -DCMAKE_TOOLCHAIN_FILE="$(m "$NDK/build/cmake/android.toolchain.cmake")" -DANDROID_ABI=$ABI \
  -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=$TYPE \
  -DMHGU_SRC="$(m "$SRC")" -DDEPS="$(m "$ROOT/deps/out/$ABI")" -DGEN="$(m "$SRC/generated")" \
  -DGAMELIB_ID=$GAMELIB_ID -DSHADER_ID=$SHADER_ID -DMHGU_VERSION="$VERSION" -DMHGU_DEV=${MHGU_DEV:-OFF} \
  -DMHGU_LINK_LIB="$(m "$MHGU_LINK_LIB")" -DMHGU_LINK_INC="$(m "$ROOT/crates/mhgu-link-ffi/include")" \
  -DMHGU_XP_LIB="$(m "$MHGU_XP_LIB")" > "$OUT.configure.log" 2>&1 \
  || { tail -30 "$OUT.configure.log"; exit 1; }
"$CMAKE" --build "$(m "$OUT")" --parallel ${JOBS:-12} 2>&1 | grep -E "error|warning: implicit|FAILED|^ninja: build stopped" | head -60
"$CMAKE" --build "$(m "$OUT")" --parallel ${JOBS:-12} > "$OUT.build.log" 2>&1 || { tail -40 "$OUT.build.log"; exit 1; }

# a jniLibs del proyecto Gradle
J="$ROOT/android/app/src/main/jniLibs/$ABI"
mkdir -p "$J"
mkdir -p "$ROOT/android/app/src/main/assets"
rm -f "$ROOT/android/app/src/main/assets/libmhgugame.id"
printf '%s\n' "$GAMELIB_ID" > "$ROOT/android/app/src/main/assets/gamelib_id.txt"  # (GameFolderActivity: which game code fits)
cp "$OUT/libmain.so" "$OUT/libmhguffmpeg.so" "$ROOT/deps/out/$ABI/sdl3/lib/libSDL3.so" "$J/"
# (MSYS2 or Linux: the NDK ships both prebuilt trees)
HTAG=windows-x86_64; [ "$(uname -s)" = Linux ] && HTAG=linux-x86_64
cp "$NDK/toolchains/llvm/prebuilt/$HTAG/sysroot/usr/lib/$( [ $ABI = arm64-v8a ] && echo aarch64 || echo x86_64)-linux-android/libc++_shared.so" "$J/"
# (a driver of the player's own: runtime/platform/android_gpu.c; these are found in the app's nativeLibraryDir)
[ "$ABI" = arm64-v8a ] && cp "$OUT"/adrenotools/src/hook/lib*_hook.so "$OUT/adrenotools/src/hook/libhook_impl.so" "$J/"
# Game code stays in the player's selected folder; never stage it for the APK.
rm -f "$J/libmhgugame.so"
echo "native $ABI: ok (GAMELIB_ID $GAMELIB_ID)"
ls -la "$J"
