# Architecture · 仓库结构与约定

[Repository / 全仓](../README.md) · [Setup / 环境](SETUP.md) · [Migration / 迁移](MIGRATION.md)

## Ownership · 模块边界

| Location / 位置 | Owns / 负责 |
|---|---|
| `ashare/` | China-specific feeds, exchange rules, L3 factors and execution / 中国市场数据、规则、L3 因子和执行 |
| `us_equity/` | US normalization, calendars, features, labels and research diagnostics / 美股清洗、日历、特征、标签和评估 |
| `data_sources/` | Vendor ingestion without strategy logic / 数据商接入，不放策略逻辑 |
| `qr/` | Shared paths and binary-format readers / 公共路径和二进制格式读取 |
| `third_party/` | External build dependencies and compatibility stubs / 外部构建依赖与兼容桩 |
| `archive/` | Superseded experiments with historical context / 已被取代的实验与历史记录 |
| `data/` | Ignored local inputs and outputs / 不入版本库的本地输入输出 |

Neither research track imports strategy logic from the other. `data_sources/massive` serves multiple
asset classes. `qr/l3cache.py` and `qr/shardbin.py` currently serve A-share binary formats; sharing the
toolkit does not imply that these formats apply to US SIP data.

两条主线不互相导入策略逻辑；`data_sources/massive` 支持多资产。
`qr/l3cache.py` 和 `qr/shardbin.py` 当前读取 A 股二进制格式，共享工具目录不代表这些格式适用于美股 SIP。

Add a market-specific feature or execution rule under that market. Extract a common utility into `qr`
only after both use cases justify a stable interface. Avoid another top-level directory per experiment.

市场专属特征和执行规则放在所属主线；两侧都有实际需求后再抽取稳定的 `qr` 公共接口。
单次实验不要再新增仓库顶层目录。

## Paths and storage · 路径与存储

All configurable paths are resolved by `qr/paths.py`, `qr/qr_paths.h` and `qr/qr_paths.sh`.
Environment variables override `qr.toml`; defaults are the final fallback. Relative paths are
repository-relative. Existing externally configured data directories need not move.

可配置路径统一由 Python/C++/Shell 的 `qr` 解析器读取。环境变量覆盖 `qr.toml`，默认值兜底；
相对路径锚定仓库根，已配置的外部数据目录无需搬迁。

| Key / 路径键 | Default / 默认值 |
|---|---|
| `tl_zip` | `data/ashare/TL` |
| `data` | `ashare/l3_factor/data` |
| `bt` | `ashare/l3_factor/bt` |
| `vsim` | `ashare/vorder_sim/build/vsim` |
| `us_minute` | `data/massive/unified/flatfiles/stocks/minute_aggs_v1` |
| `us_research` | `data/us_equity` |

The A-share main project's local `data/` and `bt/` layout is retained because historical batch scripts
use it. Some older batch scripts still assume project-relative `data/` and `bt/`; Python entry points
use `qr`. When changing storage roots, review those scripts rather than assuming every override reaches them.

A 股主项目保留内部 `data/` 和 `bt/` 布局，避免破坏历史批跑脚本。
部分旧脚本仍直接引用项目相对目录；Python 入口使用 `qr`。改变存储根时须检查旧脚本，不能假定全部支持覆盖。

US runs use fresh output directories and store configuration, source/code hashes and dependency versions.
Input prices and their adjustment basis stay explicit; no feature generator silently repairs a source contract.

美股实验使用新输出目录，记录配置、数据/代码指纹和依赖版本。
输入价格与复权口径显式声明，因子生成器不静默修复来源口径。

## C++ and source consistency · C++ 与源码一致性

CMake projects are independent under `ashare/`. Shared includes live at the repository root, one level
above `ashare`. `auction/cpp` is one level deeper. Use the documented CMake commands after moving directories;
old build caches embed absolute source paths and should be kept aside or recreated.

CMake 子项目独立放在 `ashare/`；共享头文件位于仓库根。`auction/cpp` 额外多一层。
迁移后按新路径构建；旧 CMake 缓存含绝对源码路径，需移到备份位置或重建。

`datacheck` remains the reference LOB builder. Simulator and auction copies must be reviewed together
when rules change. `python ashare/datacheck/scripts/check_lob_drift.py` checks against the existing baseline.
The reorganization does not change snapshot semantics or matching rules.

`datacheck` 仍是基准盘口实现，规则修改须同步检查模拟器和竞价副本。
漂移检查继续使用已有基线；目录整理不改变盘口快照语义或撮合规则。

## Documentation · 文档

Maintain three primary READMEs: repository navigation, A-share research and US research. Put shared setup
and path guidance under `docs/`. Main entry points should be usable by an English-only reader and a
Chinese-only reader. Historical research notes can retain their original language with a clear label.

维护三个主 README：全仓导航、A 股研究、美股研究。公共环境和路径说明放进 `docs/`。
主入口应让只会英文或只会中文的读者都能上手；历史研究记录可保留原语言并明确标注。

Record research assumptions separately from validated results. Synthetic demos, price-return diagnostics
and executable strategy backtests are distinct outputs.

研究假设与验证结果分开记录，合成 demo、价格收益诊断、可执行策略回测应明确区分。
