# Hub 联邦（MHGF v2）

两台或多台 `mhgu-hub` 可以互相共享集会所：玩家仍然只连自己的服务器，列表里会出现对端服务器的 hub，
选中后由本服代理加入。客户端协议、`HUBS4`、会话代码与游戏链路都不变。

当前版本是 **v2**：只与显式批准的对端联邦；默认全部直连，可选用传递广告（最多 N 跳）。
跨服链路固定走中继，不做 P2P 打洞。
服务器身份使用 **Ed25519**（`ed25519:<指纹>`），支持 `.well-known` 发现与局域网候选嗅探；嗅探**只出候选，
不会自动并入**。仍兼容 v1 的共享 token（HMAC）握手。好友在线状态（PRESENCE）也通过联邦实时同步。

## 身份

- 每台服首次启动在 `-identity`（默认 `mhgu-hub.identity`）写入 32 字节种子，Unix 上权限 0600；
  `-identity ''` 使用临时身份（重启即变）。
- `server_id = ed25519:` + 公钥 SHA-256 前 16 个 base32 字符；公钥以 hex 发布。
- 双方握手互相验证：对端的 id 必须等于公钥指纹；本方只接受 `-federation-allow` 中列出的 id，
  或显式 `*`（接受任何有效身份，不建议公网使用）。

## 配置

```sh
# B：接受 A 的身份，并提供 well-known
mhgu-hub -identity /var/lib/mhgu-hub/identity -advertise hub-b.example:39510 \
         -federation-allow 'ed25519:ABCDEFGH12345678' -web :39511

# A：通过 well-known 找到 B，并用 B 的 id 固定
mhgu-hub -identity /var/lib/mhgu-hub/identity \
         -federate 'http://hub-b.example:39511'

# 或直接写地址 + 期望 id（跳过 well-known）
mhgu-hub -federate 'hub-b.example:39510,ed25519:ABCDEFGH12345678,B'
```

- `-federate ADDR[,TOKEN_OR_ID[,NAME]]`：可重复。第二项以 `ed25519:` 开头 = 期望的对端 id（v2）；
  否则是 v1 共享 token。`ADDR` 以 `http://` 开头时，启动/刷新时抓取
  `/.well-known/mhgu-federation`，自动取得 endpoint 与公钥。
- `-federation-allow ID[,ID...]`：本服接受的 Ed25519 身份；可重复。`*` = 任意有效身份。
- `-federation-token T`：v1 token（可选，向后兼容）。
- `-advertise HOST:PORT`：写进 well-known 的对外地址；不填时用 `-addr`（`:39510` 会写成
  `127.0.0.1:39510`，公网部署必须设置）。
- `-discover lan`：监听/发送局域网候选广播（UDP 39513）。
- `-federation-presence[=false]`：与 peer 双向同步好友在线状态（默认开）。
- `-federation-transitive N`：传递广告的最大额外跳数（默认 `0` 关闭；`1` = 能看到 A→B→C 的 hub）。
  中间服务器也要设置大于 0，加入会沿同一路径逐跳转发。
- `-federation-state FILE`：管理页“批准候选”写在这里（默认 `mhgu-hub.federation`，`''` 关闭持久化）。
- 双方都要配置对方；联邦是双向的。

## `.well-known/mhgu-federation`

Web 端口（`-web`，默认 `:39511`）提供：

```json
{"server_id":"ed25519:...","endpoint":"hub-b.example:39510","versions":[2,1],
 "keys":[{"key_id":"ed25519:...","public_key":"<64 hex>"}],
 "capabilities":["list","find","join","identity"]}
```

只支持 `http://` 抓取；TLS 请放在反向代理后（当前客户端不校验/不发起 TLS）。地址迁移时只改
well-known 的 `endpoint`，对端在 60 秒内自动跟随。

## 行为

- **列表**：A 收到 `LIST(room, link_proto)` 时，并行向所有配置的 peer 请求同一房间，按
  `link_pool`/`min-proto`/`max-proto` 过滤后与本地 hub 合并、按原规则排序；同一 hub
  （`session id + BSSID`）本地优先。远程 hub 在 A 这里有一个**本地别名**（64 位），客户端只看到别名。
- **缓存**：每个 `(peer, room)` 缓存 3 秒；过期后下一次搜索并行刷新，单 peer 超时 1.5 秒
  （传递查询按跳数放宽，最多 10 秒）。某个 peer 挂了只影响它的 hub，本地与其他 peer 不受影响。
- **加入**：客户端 `JOIN(别名[, code])` 时，A 校验房间、链接池和代码，然后向对端发 `FED_JOIN`。
  对端把它当成普通玩家：`INCOMING` → 主机 `ATTACH` → `SPLICED`，随后双向拼接链路。
  A 再给玩家发自己的 `SPLICED`，并在两条 relay 腿之间做 TCP 转发与 UDP 前缀翻译。
- **传递广告（可选）**：A 的 `LIST` 在开启传递后发出 `FED_LIST_EXT`，携带还允许的跳数 depth 和已访问
  服务器路径；B 返回自己的本地 hub（origin=B，hops=0），并（depth>0 时）并行向不在路径里的 peer 继续查询，
  把结果 origin/hops+1 合并后返回。A 按 `(origin, hub id)` 去重并保留更短路径，客户端仍只看到本地别名。
- **传递加入**：选中一条 hops>0 的别名时，A 发 `FED_JOIN_EXT`（目标 origin、hub id、剩余跳数）给直接 peer；
  中间服务器按记录的下一条 peer 继续转发，直到 origin 本地执行 `JOIN`，再逐跳 `SPLICED` 回来。
  TCP 与 UDP 都是嵌套桥：每个中间服务器在自己的两条腿之间做前缀翻译，玩家和主机都感知不到。
- **环路与上限**：列表带 depth 和服务器 id 路径，join 带逐跳递减的预算；中间服务器拒绝超过自身
  `-federation-transitive` 的转发。广告缓存 30 秒，过期后 join 会先重新查询一次再决定。
- **代码（v4）**：本地代码优先；本地没有才向 peer 发 `FIND`。开启 `-federation-transitive N` 时改发
  `FED_FIND_EXT`，携带 depth 和已访问路径，中间服务器递归查询不在路径里的 peer，返回带 origin/hops 的
  `FED_HUBS_EXT`；命中后同样返回别名，加入走 `FED_JOIN_EXT` 逐跳回来。老 peer 不支持时自动退回普通 `FIND`。
  代码是邀请：加入时带对代码即可跨房间；代码跨服不保证全局唯一，冲突时本地优先、其后按 peer 顺序。
- **UDP**：玩家 → A 的 `MHGR(pairA, secretA)` 改写为 `MHGR(pairB, secretB)` 发给 B；
  B 回给 A 的数据保留前缀（对端把该 pair 标记为 federated），A 去掉前缀后原样交给玩家。
  跨服腿不响应 `MHGQ`，双方始终走中继。
- **好友在线（PRESENCE）**：每对服务器之间保持一条经过认证的双向 presence 流。两侧各自发送本服在线
  玩家的好友代码、状态（在线/开房/所在集会所代码）、好友列表、已删除列表和猎人名；对端保存为远端在线
  记录，90 秒无更新即过期，连接断开立即清除。回答客户端 `PRESENCE` 时把本地与远端记录按同一规则合并：
  互为好友才显示在线；单向列出成为好友请求；被对方删除则进入“移除”列表。状态只发给已互相列出的玩家。
  一条流双向发送，所以只有一侧配置 `-federate` 时，好友状态也能双向同步（握手通过后对端会回发自己的在线表）。
  若两端都配置对方，会有两条方向相反的流，数据一致但带宽翻倍；只在一端配置联邦可避免。
- **传递 PRESENCE（可选）**：新版本默认就尝试扩展 presence（即使 N=0，也可收到自然离线 tombstone），
  老 peer 自动退回普通流。`-federation-transitive N` 控制逐跳转发深度。客户端服务器使用
  `FED_PRESENCE_EXT`（带 depth/requester），连接上的 `FED_PRESENCE_UPDATE_EXT` 每条记录附带
  origin 与 hops；中间服务器把收到的远端记录按 `hops+1 <= min(depth, 本服 N)` 转发给其他对端，
  快照与增量都包含本服在线玩家和可转发的远端玩家。origin 等于本服或 requester 的记录不回传；
  本服配置的 N 同时限制自己接受/展示的远端跳数。
  广告式信任仍然成立：中间服务器会看到并短暂保存经过的玩家好友代码、好友/删除列表与猎人名；
  只对可信服务器开启，N 保持 1–2。老 peer 不支持 EXT 时自动退回普通 PRESENCE（仅直连玩家）。
  presence 流断开时，中间服务器会向其他扩展流发送 tombstone（origin+hops），让下游立即清掉该条路径的玩家；
  其他路径的同 id 记录若更短则保留。玩家自然离线（`-presence-ttl`，默认 90 秒未再上报）也会由 origin
  直接广播 tombstone；中间服务器自己的远端记录到期时同样向下游转发，不再等下游 TTL。
  只有扩展流能承载 tombstone；连接到老版本服务器的普通流仍按 TTL 收敛。
- **传递信任**：广告没有 origin 签名，A 信任 B 转发的内容；只对可信 peer 开启传递，跳数保持 1–2。
  中间服务器会看到经过的 hub 元数据与会话代码，但不解析玩家流量。
- **断开**：玩家 TCP 断开 → A 关闭到 B 的连接并删除桥；B 的对端连接关闭 → 删除 pair。
  peer 列表消失后，已有跨服对局仍可继续，直到链路本身断开。

## 握手

- v2（Ed25519）：
  1. A → B：`HELLO2` = `MHGF` + `u16 2` + id + 公钥[32] + 名字 + `client_nonce[16]`；
  2. B → A：`CHALLENGE2` = id + 公钥[32] + `server_nonce[16]` + B 的签名；
  3. A 验 B 的 id/公钥/签名，并核对配置的期望 id；
  4. A → B：`AUTH2` = A 对同一 transcript 的签名；
  5. B 验签并确认 A 的 id 在 `-federation-allow` 中，回 `OK`。
  - transcript = `"MHGU federation auth v2"` + B 公钥 + A 公钥 + A nonce + B nonce + B id + A id。
- v1（共享 token，向后兼容）：`HELLO`/`CHALLENGE`/`AUTH`，HMAC-SHA256(token, 盐 + 两个 nonce)。
- 随后同一个请求集：
  - `FED_LIST`：`room[16]` + `u16 link_proto` → `FED_HUBS`；
  - `FED_FIND`：`code[8]` → `FED_HUBS`；
  - `FED_JOIN`：`room[16]` + `u16 link_proto` + `u64 hub` + `u8 has_code` + `code[8]` → 之后与普通
    `JOIN` 相同（`SPLICED`／`ERROR`，随后是游戏链路）。
  - `FED_LIST_EXT`：`room[16]` + `u16 link_proto` + `u8 depth` + `u8 path_len` + 路径项
    （`u8 len` + 服务器 id）→ `FED_HUBS_EXT`；
  - `FED_HUBS_EXT` 条目：`u8 origin_len` + origin + `u8 hops` + `u64 id` + `u16 link_proto`
    + `code[8]` + `NetworkInfo[0x480]`；一帧最多 64 个；
  - `FED_JOIN_EXT`：`room[16]` + `u16 link_proto` + `u8 hops` + `u8 origin_len` + origin + `u64 hub`
    + `u8 has_code` + `code[8]`；中间服务器按 origin/hub 找下一跳，直到 origin 本地执行 JOIN；
  - `FED_FIND_EXT`：`code[8]` + `u8 depth` + `u8 path_len` + 路径项 → `FED_HUBS_EXT`。
- `FED_HUBS` 条目：`u64 id` + `u16 link_proto` + `code[8]` + `NetworkInfo[0x480]`；一帧最多 64 个。
- PRESENCE：握手后发送 `FED_PRESENCE`（空），收到 `OK` 后该连接变为双向状态流；扩展版发送
  `FED_PRESENCE_EXT` = `u8 depth` + `u8 requester_len` + requester id，随后使用 `FED_PRESENCE_UPDATE_EXT`：
  每条在普通更新字段后追加 `u8 origin_len` + origin + `u8 hops` + `u8 flags`（bit 0 = tombstone）。
  普通 `FED_PRESENCE` 仍使用 `FED_PRESENCE_UPDATE`：
  `FED_PRESENCE_UPDATE` = `u16 count`，每条 = 好友代码[10] + 状态 + 集会所代码[8] + 好友数/代码列表
  + 删除数/代码列表 + 名字。连接建立时先发全量在线快照（每帧最多 128 条），之后增量与 20 秒心跳；
  120 秒无数据由对端关闭。
- 认证失败 `ERR_FED_AUTH (7)`，版本不符 `ERR_VERSION`。

## 局域网候选嗅探

`-discover lan` 时：

- 每 30 秒向 `255.255.255.255:39513` 和 `127.0.0.1:39513` 发送已签名广播：
  `MHGFDISC` + 版本 + id + 公钥 + 广播地址（endpoint）+ 名字 + Ed25519 签名；
- 收到后验签、忽略自己，按 id 存入候选表，90 秒过期；
- **只出现在 Web/API 的候选表，不会变成 peer、不会被列表、不会自动连接**；
- 要真正联邦，仍需两端写 `-federate` 与 `-federation-allow`。

候选表在管理页 **Discovered peers (LAN)**，显示 id、地址、名称、存活时间、是否已配置；
`/api/hubs` 的 `candidates` 也包含 `public_key`。同一主机只能有一个进程监听 39513。

每行候选的 **Approve** 会立即：

1. 把该 id 加入 `-federation-allow`（入站接受）；
2. 把地址+id 加入联邦对端（出站列出它的 hub）；
3. 写入 `-federation-state`（默认 `mhgu-hub.federation`，原子替换，Unix 0600），重启后自动加载。

对端仍需同样批准本服，双方才能互相列表；批准页面会提示这一点。`federation_state` 中只写 id、
地址和名字，**不写 v1 token**。

## 安全与限制

- 认证证明身份，**不加密**；跨公网请只连 WireGuard/VPN，或在前面放 TLS 终止代理。
- 联邦 PRESENCE 会把本服在线玩家的好友代码、好友/删除列表和猎人名发给已认证 peer；开启传递后会再经中间
  peer 转发给其它已认证服务器。只应联邦信任的服务器；远端状态按 90 秒 TTL 过期，直达流断开立即清除，
  传递流断开由 tombstone 清理该路径。`-federation-presence=false` 可完全关闭。
- v2 默认封闭：不在 `-federation-allow` 的 id 一律拒绝；well-known 只用于发现，不产生信任。
- 所有对端数据都校验：`NetworkInfo`、协议版本、房间、hub 数/帧长上限；请求受每 IP 限速。
- 默认不做传递广告；`-federation-transitive N` 开启后，A 信任 B 转发的 C 广告（广告无 origin 签名），中间服务器也能看到 hub 元数据与会话代码。只对可信 peer 开启，并把 N 保持在 1–2。
- 别名不写入磁盘；服务器重启后客户端重新搜索即可。身份文件是唯一需要备份的联邦状态。

## 运维

- 启动日志打印 `ed25519:` 身份、接受的身份列表、联邦对端与发现开关。
- Web 管理页：**Federation peers**（状态/RTT/错误）与 **Discovered peers**；`/api/hubs` 增加
  `peers`、`candidates`、`federated_joins`、`federated_bytes`、`federated_presence`。
- 常见失败：身份不在允许列表（`ERR_FED_AUTH`）、well-known 不可达、`-advertise` 写错、版本不一致。
- 备份：身份文件与 `-federation-state` 是仅有的联邦状态；两者丢失会改变 id/已批准列表。
