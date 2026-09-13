# A-shares · A 股研究

[Repository / 全仓](../README.md) · [US equities / 美股](../us_equity/README.md) ·
[Setup / 环境](../docs/SETUP.md) · [Architecture / 结构](../docs/ARCHITECTURE.md)

This track contains mainland China equity data infrastructure and L3 order-flow research.
The main research project is `l3_factor`; `factor` contains earlier, superseded experiments.

本目录集中放置中国 A 股数据基础设施与 L3 订单流研究。主线是 `l3_factor`；
`factor` 保留早期、已被取代的基础盘口因子实验。

## Pipeline · 研究链路

```text
Raw TL events → tltoflow → unified flow → datacheck / auction → build_tick3s
                                     ↘ vorder_sim → cached order-level events
                                                   ↓
                                             l3_factor
                                      features → evaluation → execution tests
```

The L3 research path reads order-level events, not just three-second panels. `vorder_sim` tracks
hypothetical orders on historical queues without inserting them into the real order book.

L3 主线读取逐订单事件，不只依赖三秒降频面板。`vorder_sim` 在历史队列上追踪虚拟订单，
虚拟订单不进入真实订单簿。

| Component / 模块 | Role / 职责 |
|---|---|
| [tltoflow](tltoflow/README.md) | TL records → unified flow / 通联原始数据转统一事件流 |
| [datacheck](datacheck/README.md) | Reference LOB builder, dual-source validation / 基准盘口重建、双源比对 |
| [auction](auction/README.md) | Auction reconstruction and indicative matching / 竞价重建与虚拟撮合 |
| [build_tick3s](build_tick3s/README.md) | Three-second panel generation / 三秒面板生成 |
| [vorder_sim](vorder_sim/README.md) | Hypothetical queues, fills and auctions / 虚拟订单排队、成交与竞价 |
| [l3_factor](l3_factor/README.md) | Features, IC, ML combinations, execution tests / 因子、评估、组合与执行研究 |
| [factor](factor/README.md) | Earlier A-share factor code; legacy / 早期 A 股因子，已归档定位 |

## Build and verify · 构建与验证

Run from the **repository root** in the `mx` conda environment. CMake, a C++17 compiler and HDF5 are
required; installation instructions are in [Setup](../docs/SETUP.md).

以下命令均在**仓库根目录**执行，使用 `mx` conda 环境。需要 CMake、C++17 编译器和 HDF5，
安装说明见[环境配置](../docs/SETUP.md)。

```bash
conda activate mx
python -m pip install -e .
cmake -S ashare/vorder_sim -B ashare/vorder_sim/build
cmake --build ashare/vorder_sim/build -j4
ctest --test-dir ashare/vorder_sim/build --output-on-failure
python ashare/datacheck/scripts/check_lob_drift.py
```

The simulator has two self-contained CTest suites: synthetic queue invariants and hand-written Flow
parser/integration fixtures. The drift check compares the three LOB implementations against the
existing baseline. These checks require no external market data.

模拟器有两组自包含 CTest：合成队列不变量与手写 Flow 解析/集成校验；
漂移检查对照已有基线检查三份盘口实现。这些检查均无需真实行情。

Other builds / 其他构建入口：

```bash
cmake -S ashare/tltoflow -B ashare/tltoflow/build
cmake --build ashare/tltoflow/build -j4
cmake -S ashare/datacheck -B ashare/datacheck/build
cmake --build ashare/datacheck/build -j4
cmake -S ashare/auction/cpp -B ashare/auction/cpp/build
cmake --build ashare/auction/cpp/build -j4
cmake -S ashare/build_tick3s -B ashare/build_tick3s/build
cmake --build ashare/build_tick3s/build -j4
```

## Run research · 运行研究

If local configuration does not exist, copy `qr.example.toml` to `qr.toml` and configure the data
locations. Preserve any existing settings. Absolute paths can point to an external disk.

没有本机配置时，将 `qr.example.toml` 复制为 `qr.toml` 并设置数据位置；已有配置请直接编辑。
绝对路径可指向外部数据盘。

| Key / 路径键 | Default / 默认值 |
|---|---|
| `tl_zip` | `data/ashare/TL` |
| `data` | `ashare/l3_factor/data` |
| `bt` | `ashare/l3_factor/bt` |
| `vsim` | `ashare/vorder_sim/build/vsim` |

With the expected TL archives present / 对应 TL 压缩包就绪后：

```bash
python ashare/l3_factor/runday.py 20260707
python ashare/l3_factor/eval/eval_multiday.py --days 20260707
python ashare/l3_factor/eval/ic_horizon.py --panels ashare/l3_factor/data/panels
```

`runday.py` extracts CSVs, builds the simulator cache and computes features. Temporary CSVs are removed
by default; use `--keep-csv` to retain them. Full research requires actual data and substantial disk
space. Offline tests do not validate the data-dependent research results.

`runday.py` 解压 CSV、建立缓存并抽取特征；默认清理临时 CSV，需保留时加 `--keep-csv`。
完整研究依赖真实行情和足够磁盘空间，离线测试不验证真实数据研究结论。

## Four data contracts · 四条关键约定

1. **Pre-event snapshots.** Each row contains the book state before its event. Changing this can cause
   an extra shift in `build_tick3s`.
   **事件前盘口。** 每行记录事件发生前的状态，改变口径可能令降频链路多偏移一次。
2. **Three builder implementations.** `datacheck` is the reference, `vorder_sim` adds read-only hooks,
   and `auction` is independent. Review all three when changing exchange rules.
   **三份盘口实现。** `datacheck` 是基准，`vorder_sim` 增加只读扩展，`auction` 独立实现；规则修改须同步检查。
3. **Shanghai source limitations.** This repository's TL Shanghai feed has incomplete order records.
   SH L3 research needs corrected events; current `l3_factor` work primarily uses SZ.
   **上海源数据限制。** 当前 TL 上海委托记录不完整；SH 研究需补全数据，主线主要使用 SZ。
4. **Unknown auction direction.** Shanghai auction trades may have unknown side. Keep three-way mapping.
   **竞价方向可未知。** 上海竞价方向按三路映射处理，不强行二值化。

## Evidence and limitations · 证据与限制

Historical reports distinguish predictive signals from profitable strategies. Reported A-share
round-trip tests lost money after modeled costs. An execution-timing experiment improved on TWAP,
although its report identifies selection leakage and the need for independent validation.
These findings depend on the documented samples and assumptions.

历史报告区分“有预测力”和“可盈利”：已记录的 A 股往返交易在模型成本后亏损；
执行择时相对 TWAP 出现改善，但报告也指出选择偏差及独立验证需求。结论受当时样本和假设约束。

Detailed evidence is currently in Chinese / 以下详细证据目前为中文：

- [Factor book / 因子手册（中文）](l3_factor/FACTOR_BOOK.md)
- [Feature dictionary / 特征字典（中文）](l3_factor/FEATURES.md)
- [Strategy comparison / 策略对照（中文）](l3_factor/docs/REPORT_S123.md)
- [Execution timing and limitations / 执行择时及限制（中文）](l3_factor/docs/REPORT_EXEC_TIMING.md)
- [Shanghai/Shenzhen data rules / 沪深数据规则差异（中文）](datacheck/沪深数据与规则差异.md)
