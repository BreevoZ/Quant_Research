# Repository validation · 仓库验证

[Repository / 全仓](../README.md) · [Setup / 环境](SETUP.md) · [Migration / 迁移](MIGRATION.md)

Validation performed on 2026-09-12 after separating the market tracks, followed by a clean-snapshot
pre-push audit on the same date.
This records software checks, not new financial research results.

以下是 2026-09-12 市场目录拆分后及同日干净副本提交前审查的软件验证记录，不是新的金融研究结论。

## Environment · 环境

| Component / 组件 | Tested / 已验证版本 |
|---|---|
| Python environment / Python 环境 | conda `mx`, Python 3.12.13 |
| NumPy / pandas / PyArrow | 2.5.1 / 2.3.3 / 25.0.0 |
| exchange_calendars | 4.13.2 |
| Platform / 平台 | macOS arm64 |
| C++ / HDF5 | AppleClang 21 / system HDF5 2.2.0 |

The existing scientific packages were reused. The missing calendar package and its missing dependencies
were added to `mx`, and the repository was installed in editable mode. The previous `.venv` was not used.

复用已有科学计算包，只补齐缺失的日历包及其缺失依赖，并在 `mx` 中 editable 安装仓库。
没有使用之前的 `.venv`。

## Results · 结果

| Check / 检查 | Result / 结果 |
|---|---|
| US semantic tests / 美股语义测试 | 13 passed, including malformed configuration rejection / 13 项通过，含错误配置拒绝测试 |
| Pre-push guard tests / 提交检查测试 | 8 passed, including staged secrets and deleted historical secrets / 8 项通过，含暂存内容和已删除历史密钥检测 |
| US demo / 美股 demo | DST-before, DST-after, half-day; synthetic provenance retained / 夏令时前后、半日市通过，保留合成标记 |
| Simulator CTest / 模拟器 CTest | 2/2 passed: queue invariants + Flow fixtures / 2/2 通过 |
| LOB drift / 盘口漂移 | Existing baseline unchanged; passed / 原有基线不变，通过 |
| C++ builds / C++ 构建 | All five projects built / 五个项目均编译成功 |
| Paths / 路径 | Python/C++ defaults match; runtime Python/C++/Shell agree from an external directory / 默认值及仓库外三语言运行结果一致 |
| Source/document checks / 源码文档检查 | Python syntax, Shell syntax, local link targets and package layout passed / 语法、链接目标及包结构通过 |
| Package dependencies / 包依赖 | `python -m pip check` passed in `mx` / `mx` 中通过 |
| Relocation audit / 搬迁核对 | All 172 relocated tracked files accounted for / 172 个搬迁的已跟踪文件均有对应位置 |
| Candidate completeness / 候选提交完整性 | 334 working files exactly matched the temporary index; no missing files / 334 个候选文件与临时索引完全一致，无遗漏 |
| Candidate diff / 候选差异 | `git diff --cached --check` passed in the temporary index / 临时索引中的差异检查通过 |
| Sensitive content / 敏感内容 | No findings in candidate content or locally reachable history; 4 distinct local key values compared without displaying them / 候选内容及本地可达历史无命中，4 个不同本机密钥值经不回显比对 |
| File sizes / 文件体积 | Largest candidate file: 3,481,004 bytes; no file reaches the 50 MiB guard / 最大候选文件约 3.5 MB，无文件达到 50 MiB 检查阈值 |
| Packaging / 打包 | Editable installation and wheel build passed from the clean snapshot; packaged C++/Shell/CMake helpers and experiment template present / 干净副本 editable 安装和 wheel 构建通过，共享辅助文件及实验模板齐全 |

The five built projects are `tltoflow`, `datacheck`, `auction/cpp`, `build_tick3s` and `vorder_sim`,
all under `ashare/`. Full market-data runs were not executed as part of the directory reorganization.

五个已构建项目均位于 `ashare/`。本次目录整理没有运行全量真实行情生产任务。

## Clean snapshot audit · 干净副本审查

The audit copied only Git candidate files into a temporary checkout, excluding ignored datasets,
credentials and local build directories. All five C++ projects were configured and built there from
scratch. Python checks also ran in a temporary installation based on `mx`'s existing scientific
dependencies, with editable imports verified to resolve to the snapshot from outside the checkout.
This checks source completeness with the tested dependency stack; it is not a fresh conda solver run.

审查仅将 Git 候选文件复制到临时 checkout，排除已忽略的数据、密钥及本机构建目录。
五个 C++ 项目在副本中从零配置并构建。Python 另使用临时安装，复用 `mx` 的科学计算依赖，
且从 checkout 外确认 editable 导入指向副本。此项验证已测试依赖组合下的源码完整性，
不是一次全新的 conda 依赖求解。

The source audit covers 82 Python files, 14 Shell scripts, 28 Markdown files, local navigation
links and 20 shared path keys. Python files also parse under the Python 3.11 grammar. Five generated
egg-info files were intentionally removed from version control; no other tracked deletion is unaccounted for.
The existing LOB drift baseline is unchanged.

源码审查覆盖 82 个 Python 文件、14 个 Shell 脚本、28 个 Markdown 文件、本地导航链接及
20 个共享路径键；Python 文件也通过 3.11 语法解析。5 个生成的 egg-info 文件有意退出版本控制，
其余已跟踪删除均有搬迁去向。盘口漂移基线未改变。

The five documented `run.sh` / `build.sh` entry points under `tltoflow`, `build_tick3s` and
`auction/cpp` now have executable permission, which is also checked by the layout guard.

`tltoflow`、`build_tick3s`、`auction/cpp` 下文档使用的 5 个 `run.sh` / `build.sh`
已补齐执行权限，并纳入目录检查，避免新 checkout 执行时出现 Permission denied。

The guard is heuristic and scans local state only. Remote write access and branch rules were not
tested. The user's Git index, commits and remote were not changed by the audit. Follow the
[pre-push guide](PRE_PUSH.md) to stage and review the final changes.

安全检查为启发式本地扫描，未测试远端写入权限或分支规则。审查未更改用户的 Git 索引、提交或远端。
最终暂存及查看差异的步骤见[提交前说明](PRE_PUSH.md)。

## Known limits · 已知边界

- The local pandas 2.3 / NumPy 2.5 combination emits timedelta deprecation warnings during tests.
  All current semantic assertions pass; packages were not upgraded or downgraded just to silence warnings.
  本机 pandas 2.3 / NumPy 2.5 组合在测试中会发出 timedelta 弃用提示；断言均通过，没有为消除提示改动科学计算包版本。
- OpenMP is unavailable in the tested system compiler, so the auction build proceeds without it.
  本机编译器未配置 OpenMP，竞价程序以不启用该可选功能的方式构建。
- `build_tick3s` uses the fallback ClickHouse stub in this environment. Successful compilation does
  not mean database writes were tested. Existing unused-variable compiler warnings remain.
  本机 `build_tick3s` 使用 ClickHouse 桩，编译成功不代表数据库写入已验证；已有未使用变量告警保留。
- Historical A-share results remain subject to their original data and modeling assumptions.
  US demo outputs remain synthetic, and the minute baseline has no executable cost model.
  历史 A 股结论仍受原数据与模型假设约束；美股 demo 为合成数据，分钟基线没有可执行成本模型。

To reproduce the standard checks, use the [setup guide](SETUP.md). A generated bilingual demo was
also written locally to `data/us_equity/mx_bilingual_demo/`; generated data and reports are ignored by Git.

标准复现命令见环境说明。本机另生成了 `data/us_equity/mx_bilingual_demo/` 双语示例，
数据与报告不进入 Git。
