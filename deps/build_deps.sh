#!/bin/bash
# Compila las dependencias nativas para Android (arm64-v8a): SDL3 (shared), shaderc (static), FFmpeg (static).
# Uso: bash build_deps.sh [abi]   (abi por defecto arm64-v8a; x86_64 para el emulador)
set -e
ABI=${1:-arm64-v8a}
API=29
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-${LOCALAPPDATA:+$LOCALAPPDATA/Android/Sdk}}}"
[ -n "$SDK" ] && [ -d "$SDK" ] || SDK="$HOME/Android/Sdk"
[ -d "$SDK" ] || { echo "build_deps.sh: no Android SDK (set ANDROID_HOME)"; exit 1; }
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.0.12077973}"
[ -d "$NDK" ] || { echo "build_deps.sh: no NDK 27.0.12077973 under $SDK/ndk (set ANDROID_NDK_HOME)"; exit 1; }
CMAKE="$SDK/cmake/3.22.1/bin/cmake"; NINJA="$SDK/cmake/3.22.1/bin/ninja"
[ -x "$CMAKE" ] || { CMAKE=cmake; NINJA=ninja; }
m() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf %s "$1"; fi; }
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/src"
OUT="$HERE/out/$ABI"
mkdir -p "$OUT"
TC="$(m "$NDK/build/cmake/android.toolchain.cmake")"
COMMON=(-G Ninja -DCMAKE_MAKE_PROGRAM="$(m "$NINJA")" -DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI=$ABI
        -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

what=${2:-all}

if [[ $what == all || $what == sdl ]] && [ ! -f "$OUT/sdl3/.built" ]; then
  echo "== SDL3"
  "$CMAKE" -S "$(m "$SRC/SDL3-3.4.18")" -B "$(m "$OUT/sdl3/build")" "${COMMON[@]}" \
    -DCMAKE_INSTALL_PREFIX="$(m "$OUT/sdl3")" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 > "$OUT/sdl3.configure.log" 2>&1 || { tail -30 "$OUT/sdl3.configure.log"; exit 1; }
  "$CMAKE" --build "$(m "$OUT/sdl3/build")" --target install --parallel 12 > "$OUT/sdl3.build.log" 2>&1 || { tail -30 "$OUT/sdl3.build.log"; exit 1; }
  touch "$OUT/sdl3/.built"
fi

if [[ $what == all || $what == shaderc ]] && [ ! -f "$OUT/shaderc/.built" ]; then
  echo "== shaderc"
  "$CMAKE" -S "$(m "$SRC/shaderc")" -B "$(m "$OUT/shaderc/build")" "${COMMON[@]}" \
    -DANDROID_STL=c++_static -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF > "$OUT/shaderc.configure.log" 2>&1 || { tail -30 "$OUT/shaderc.configure.log"; exit 1; }
  "$CMAKE" --build "$(m "$OUT/shaderc/build")" --target shaderc_combined --parallel 12 > "$OUT/shaderc.build.log" 2>&1 || { tail -30 "$OUT/shaderc.build.log"; exit 1; }
  mkdir -p "$OUT/shaderc/lib" "$OUT/shaderc/include"
  cp "$OUT/shaderc/build/libshaderc/libshaderc_combined.a" "$OUT/shaderc/lib/"
  cp -r "$SRC/shaderc/libshaderc/include/shaderc" "$OUT/shaderc/include/"
  touch "$OUT/shaderc/.built"
fi
echo "deps $ABI: ok"
