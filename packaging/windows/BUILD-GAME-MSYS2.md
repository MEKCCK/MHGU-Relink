# Building the Windows game (mhgurecomp.exe)

Cross-compiling this from Linux is not supported: on Windows the Makefile links **MSYS2's** SDL3 and
shaderc (`SDL3=system SHADERC=system`, their DLLs get bundled), and Arch/Debian have no mingw builds of
either. Build it on Windows with MSYS2 instead — that is the configuration the project ships and tests.

## 1. Toolchain (MSYS2 UCRT64 shell)

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{clang,lld,cmake,ninja,make,sdl3,shaderc} \
                    git python
```

FFmpeg is not needed as a package: the Makefile builds the pinned release from source (into `obj/`).

## 2. The Rust link library (replaces the C ldn)

```sh
rustup target add x86_64-pc-windows-gnu
cargo build --release --target x86_64-pc-windows-gnu -p mhgu-link-ffi
# -> target/x86_64-pc-windows-gnu/release/libmhgu_link_ffi.a
```

## 3. The game code (your own dump — never distributed)

```sh
cd base
make gen GAME=/c/path/to/MHGU-1.4        # translates exefs/ into generated/ (hours, once)
make -j12                                # build/mhgurecomp.exe + the game DLL + build/data
```

`build/` is the folder a player copies: `mhgurecomp.exe`, `mhgugame.dll`, the SDL3/FFmpeg/shaderc DLLs,
`mhgurecomp.ini`, `pipelines.list`, and `data/` (links to your exefs/romfs).

## 4. Verify before shipping

```sh
tools/check_release.sh build/mhgurecomp.exe build/*.dll
```

It fails if recompiled game code (mhgugame.dll) or the dump ends up somewhere you would publish.

## Why not cross-compile

- SDL3 and shaderc are MSYS2 packages on Windows (no mingw builds in Arch's repos);
- FFmpeg's Windows build here expects the MSYS2 environment;
- the game DLL must be produced from the player's own dump anyway.

The Rust parts (hub server, link library) do cross-compile fine — `packaging/windows/build.sh`.
