#!/bin/sh
# Cross-compiles the Rust parts for Windows from Linux (or run it under MSYS2 on Windows).
#
#   sudo pacman -S mingw-w64-gcc          # Arch: the cross toolchain (also provides the import libs)
#   rustup target add x86_64-pc-windows-gnu
#   ./packaging/windows/build.sh
#
# Output: dist/windows/mhgu-hub.exe and the link library for the C base.
set -eu
cd "$(dirname "$0")/../.."
TARGET=x86_64-pc-windows-gnu
cargo build --release --target $TARGET -p mhgu-hub
cargo build --release --target $TARGET -p mhgu-link-ffi
mkdir -p dist/windows
cp target/$TARGET/release/mhgu-hub.exe dist/windows/
cp target/$TARGET/release/mhgu_link_ffi.lib dist/windows/ 2>/dev/null || cp target/$TARGET/release/libmhgu_link_ffi.a dist/windows/
cp -r packaging/web-example dist/windows/web
cat > dist/windows/README.txt <<'TXT'
MHGU hub for Windows
====================
mhgu-hub.exe -addr :39510 -web :39511 [-web-token TOKEN] [-key KEY] [-web-dir web]

  -web-dir web      serve your own UI from the "web" folder (index.html + /api/hubs)

The link library (mhgu_link_ffi.lib / libmhgu_link_ffi.a) is what the C base
(the C runtime at the repository root) links against; build it with the MSYS2 mingw-w64 toolchain.

Wire-compatible with MHGURecomp and gu-native (server protocol 1, link protocol 2).
TXT
echo "-> dist/windows"
