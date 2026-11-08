# xxgu-gateway — MHXX ↔ MHGU 联机网关（POSIX 版）

本目录是 **独立 GPL 程序**，用于让 Citra 上的 3DS《怪物猎人XX》与本项目的 MHGU 客户端进入
同一个房间联机。它**不是** MIT 代码，也不被链接进 `mhgurecomp` / `libmain.so`；只作为独立进程
运行。线格式说明见 [PROTOCOL.md](PROTOCOL.md)。

## 来源与改动

上游：<https://github.com/caaaaaaaaaaaat/MHXX-GU-Gateway>（GPL-2.0-or-later，官方只有 Windows
构建，CLI 锁回环监听）。本目录是它的 POSIX 移植，改动集中在：

- `citra-gateway/src/gateway.cpp`：Winsock → POSIX socket（`close`/`fcntl`/`errno`/`nanosleep`），
  保留 Windows 分支；
- `citra-gateway/src/main.cpp`：`stat` 替代 `GetFileAttributesA`、`signal(SIGPIPE, SIG_IGN)`、
  `--allow-remote-listen` / `--allow-remote-room` / `--seconds 0`、`--max-clients` 限制 1..512、
  trace 平台字段按构建平台填写；
- `room-client`：按 **suyu v0.0.4** 对齐房间线格式（见 PROTOCOL.md 第 3、5 节），
  `JoinSuccess` 先到也能完成入厅；
- 新增 `Makefile`：只用生产单元 + vendored ENet + 系统 zstd；**不需要 Yuzu/Citra 上游源码**
  （上游测试程序才需要，未随本目录分发）。

## 构建（Linux）

依赖：g++ ≥ 13（C++20）、make、zstd 开发包。

```sh
sudo apt install g++ make libzstd-dev     # Debian/Ubuntu
cd xxgu-gateway
make -j8                                  # 产物：build/citra-gateway
```

## 运行

```sh
./build/citra-gateway \
  --listen 0.0.0.0 --allow-remote-listen --listen-port 8888 \
  --room 127.0.0.1 --room-port 24872 \
  --seconds 0 --quiet \
  --control-plane --ldn-dual-host --ldn-gu-host-discovery --ldn-gu-host-admission \
  --ldn-proxy-port 12345 --ldn-comm-id 0100770008dd8000 --uds-profile mhxx
```

- Citra 端直连 `网关地址:8888`，密码与房间密码一致（本项目测试服务器为空）。
- MHGU 端用 `Crossplay = 多人房间`，`RoomServer` 指向同一个 `mhgu-roomsrv`（默认 UDP 24872）。
- `--seconds 0` 常驻；`SIGTERM`/`SIGINT` 正常退出。
- 诊断：环境变量 `XXGU_GATEWAY_DEBUG=1` 才输出 `[citra]`/`[room->ldn]`/`[gu_host]` 日志，
  默认安静（避免公网对端刷日志）。

systemd 示例见项目 `docs/CROSSPLAY-zh-CN.md`。

## 目录

| 路径 | 内容 |
|---|---|
| `citra-gateway/` | 网关核心：Citra v4 ⇄ Yuzu room v1、UDS/LDN 控制面、Pia 数据面 |
| `citra-room/` | Citra 房间协议 codec（network_version 4） |
| `room-client/` | Yuzu room v1 codec/状态机（对齐 suyu v0.0.4） |
| `room-transport/` | ENet 传输层；`vendor/` 为 ENet、fmt 源码 |
| `citra-control/` | UDS/LDN 控制面映射库（beacon、EAPoL、节点表） |
| `PROTOCOL.md` | 两侧线格式、消息表、顺序兼容与移植偏差 |

## 许可证

- `citra-gateway/`、`citra-room/`、`room-client/`、`citra-control/`：**GPL-2.0-or-later**
  （各文件头 SPDX；`citra-gateway/NOTICE.md`、`citra-room/NOTICE.md`、`citra-control/NOTICE`）。
- `room-transport/`：**GPL-3.0**（`room-transport/LICENSE.txt`）。
- `room-transport/vendor/enet-*`：MIT；`room-transport/vendor/fmt-*`：MIT；系统 `libzstd`：BSD。

本目录与仓库其余 MIT 代码之间只通过 UDP/ENet 协议交互，不存在链接或源码复制。
