# Packaging

| What | Where | How |
|---|---|---|
| Linux tarball (any distro) | `dist/mhgu-hub-0.1.0-linux-x86_64.tar.gz` | `cargo build --release -p mhgu-hub` then copy the binary, `mhgu-hub.service`, `packaging/web-example` |
| **Game** Arch package (pacman) | `packaging/arch-game/PKGBUILD` | 先在仓库根目录 `make FFMPEG=system`；再 `cd packaging/arch-game && makepkg -f` → `mhgu-recomp-0.1.0-1-x86_64.pkg.tar.zst`；`sudo pacman -U …`，然后 `mhgu-recomp <游戏目录>` |
| Hub Arch package (pacman) | `packaging/arch/PKGBUILD` | `cd packaging/arch && makepkg -f` → `mhgu-hub-0.1.0-1-x86_64.pkg.tar.zst` |
| Windows (Rust parts) | `packaging/windows/build.sh` | 需要 `mingw-w64-gcc` + `rustup target add x86_64-pc-windows-gnu`；产出 `dist/windows/mhgu-hub.exe` 与联机静态库 |
| Your own web UI | `packaging/web-example/` | copy it anywhere, edit, run `mhgu-hub -web-dir <that dir>` |

The Arch package installs `/usr/bin/mhgu-hub`, the systemd unit and the example UI under
`/usr/share/mhgu-hub/web` (point `-web-dir` there, or at your own copy).
