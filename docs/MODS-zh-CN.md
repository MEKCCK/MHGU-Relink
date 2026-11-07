# 模组导入与兼容范围

在游戏设置中选择“模组 → 添加模组”，导入后在模组列表调整顺序和启用状态，再保存并重启游戏。模组会覆盖游戏读取到的资源，不会改写原游戏目录。

可以选择包含 `romfs`、`exefs` 或 `nativeNX` 的模组文件夹或 ZIP/RAR 压缩包。导入器识别最多 8 层包装目录；压缩包里有多套完整模组时，先选择其中一套，避免混装互斥版本。中文名称会保留，长名称按运行时的 UTF-8 长度限制缩短并附加摘要。复制或解包失败时保留原先已安装的同名模组。

也可以直接选择单个文件：

| 文件 | 安装位置 |
| --- | --- |
| `.nro` | `romfs/nativeNX/plugins/` |
| `.ips`、`.ips32`、`.pchtxt` | `exefs/` |

IPS/IPS32 的文件名必须以目标模块的 Build ID 开头，PCHTXT 使用其版本标签；不匹配的补丁不会强行应用。损坏的 IPS/IPS32 记录在完整检查通过前不会应用。运行时最多列出并处理 256 个模组；插件还受模块数量和插件内存区域限制。

## 模板范围

| 样本 | 结构 |
| --- | --- |
| MHGU 见切居合登龙，改 R 键 | 两套独立变体，各含 `subsdk9`、两个 Forge NRO 和资源；导入时二选一 |
| 怪物：巨戟龙 & 大轰龙 | 单包装目录，含 `subsdk2`、敌人/地图/装备资源、任务和音频 |
| MHGU 崛起喝药 | 顶层 `romfs/nativeNX/plugins/` 下的单个 NRO |
| MHGU 15 合一强化 10.25 | RAR5，单包装目录，含 `subsdk9`、Forge NRO 和大量 ARC 资源 |

目录和字节完整性验证只证明文件能正确导入。Forge API、Hook 指令、主程序固定地址、游戏版本和不同模组之间的冲突仍会影响实际运行。当前不支持 Thumb Hook，无法承诺所有未来模组都能运行。多个模组修改相同资源时，请查看模组页面的冲突提示并调整加载顺序；不应同时启用互斥版本。

## 本地验证

原生目录搜索回归：`python tests/mods/run_mod_search.py`。

生产补丁解析和 ARC 合并边界回归：`python tests/mods/run.py`。

Java 导入器回归见 `tests/java/ModFilesTest.java`，使用 UTF-8 编码编译并提供实际样本路径。解包库随 Android 构建固定版本，许可证随 APK 提供。

重新构建使用项目原有入口：`native/build_native.sh arm64-v8a Release`，随后 `android/gradlew.bat -p android :app:assembleRelease`。构建和主机回归通过不代表已完成手机上的插件或游戏行为验收。
