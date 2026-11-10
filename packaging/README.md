# 打包

| 产物 | 位置 | 方法 |
|---|---|---|
| Linux 压缩包（任意发行版） | `dist/mhgu-hub-0.1.6-linux-x86_64.tar.gz` | `cargo build --release -p mhgu-hub`，复制二进制、`mhgu-hub.service`、`packaging/web-example` |
| **游戏 Arch 包** | `packaging/arch-game/PKGBUILD` | 先在根目录 `make FFMPEG=system`，再 `cd packaging/arch-game && makepkg -f`，得到 `mhgu-relink-0.1.6-6-x86_64.pkg.tar.zst` |
| Hub Arch 包 | `packaging/arch/PKGBUILD` | `cd packaging/arch && makepkg -f`，得到 `mhgu-hub-0.1.6-2-x86_64.pkg.tar.zst` |
| Windows（Rust 部分） | `packaging/windows/build.sh` | 需要 `mingw-w64-gcc` 和 `rustup target add x86_64-pc-windows-gnu` |

游戏 PKG 安装 `/usr/lib/mhgu-relink/mhgu-relink`、默认 INI、许可和启动器；不包含游戏代码。
Hub PKG 安装 `/usr/bin/mhgu-hub`、systemd unit 和示例 Web UI（`/usr/share/mhgu-hub/web`）。

版本以根目录 `VERSION` 为准；`PKGBUILD` 的 `pkgver` 要同步。
