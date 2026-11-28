# MHGU 百科

本程序内置 MHGU 百科，打开 **设置 → 应用 → MHGU 百科** 即可查看。

- 怪物：弱点、部位伤害、状态阈值、栖息地、素材与任务
- 道具：类型、稀有度、买卖价、说明、调和、采集与报酬来源
- 任务：等级、目标、副任务、怪物与报酬表
- 武器、防具、随从装备、技能、装饰品、狩猎笛旋律、地点
- 怪物和道具图标显示在列表与详情页

顶部可切换中文/英文，搜索框按名称过滤。数据直接编译在程序内，不需要额外文件。

## 数据来源与更新

数据来自 **MHGU Database 2.4.5** 的 `mhgu.db`（`MHGUDatabase.apk` 内
`assets/databases/mhgu.db.zip`）：`monsters`、`items`、`quests`、`weapons`、`armor`（按套装归组）、
`palico_weapons` / `palico_armor`、`skills`、`decorations`、`horn_melodies`、`locations` 及弱点/部位/素材/报酬表。

运行时只带编译后的精简数据，不带 APK 或完整数据库。中文详情里的属性、怪物状态等英文标签由
`tools/localize_encyclopedia_zh.py` 统一整理；该脚本可重复运行且幂等。

列表类详情（怪物任务、任务/怪物报酬、采集地点、调和）由 `tools/fill_encyclopedia_lists.py` 从同一数据库补齐，不再保留“另有 N 个”“仅显示部分地点”的截断；两个脚本都在 APK 数据更新后重新运行。
