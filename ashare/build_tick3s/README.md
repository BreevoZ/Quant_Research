# build_tick3s — LOB 3 秒降频处理 (lob_processor)

[A-share overview / A 股入口](../README.md) · [Repository / 全仓](../../README.md) · [Setup / 环境](../../docs/SETUP.md)

## English quick start

Generate three-second panels from reconstructed pre-event snapshots. The fallback ClickHouse client is a no-op stub; it does not write to a database.

Commands below run from the repository root, using conda `mx` for Python:

```bash
cmake -S ashare/build_tick3s -B ashare/build_tick3s/build
cmake --build ashare/build_tick3s/build -j4
```

See the [A-share guide](../README.md) for data contracts, paths and research limitations.
The detailed Chinese implementation reference follows. Its component-local commands assume
`cd ashare/build_tick3s` unless they explicitly start from the repository root.

## 中文技术说明

下文保留原项目技术细节；项目内相对命令以 `ashare/build_tick3s` 为工作目录，仓库根命令另行标明。

把逐笔重建的订单簿（LOB H5）降频成 **3 秒切片快照**，输出到 ClickHouse 和/或 Parquet。对应 Python 版 `Lob_data_save_sh.py` 的 C++ 重写。

---

## 1. 它做什么

```
逐笔 LOB H5  ──►  build_tick3s (lob_processor)  ──►  3 秒快照
(builder 产出)      按 3s 桶聚合 + 取盘口快照        ClickHouse: Tick_3sec_data
                                                    Parquet:   {date}_tick.parquet
```

- **输入**：builder（datacheck / tl_lob / flow_lob 等）产出的 LOB H5，每行一个逐笔事件（`wt` 委托 / `zb` 成交 / `cl` 撤单），含 10 档盘口 + 每档挂单笔数 `bid_num/ask_num`。
- **处理**：按 3 秒桶（`time_slot = floor(t/3)*3`，左闭右开 `[s, s+3)`）聚合每桶的订单流，并取桶内最后状态的 10 档盘口快照。
- **输出**：每行 = 一个 `(股票, 3秒桶)` 的特征向量（约 215 列），写入 ClickHouse 表 `Tick_3sec_data` 和/或 Parquet 分片。

---

## 2. 关键设计：pre-event 输入 → post-event 快照

> ⚠️ **这是整条流水线的核心约定，改 builder 时务必遵守。**

builder 产出的 LOB H5 行内 10 档是 **pre-event 口径**（第 i 行的盘口 = 事件 i *发生前* 的状态，与 tl_lob / flow_lob 基线一致）。

build_tick3s 需要 **post-event** 盘口做快照，它**自己推导**（`lob_data_processor.cpp` 约 L1470）：

```cpp
order_df_[i].post_bdp = order_df_[i+1].bdp;   // 第 i 行的 post = 第 i+1 行的 pre
// 最后一行无后继 → 退回自身
```

每个 3 秒桶取**桶内最后一条记录的 `post_bdp`** 作为该桶的盘口快照。

**含义**：
- builder **必须输出 pre-event**。若改成 post-event，build_tick3s 会再 shift 一次 → 双重偏移 → 3 秒盘口系统性错位一格（look-ahead），且**静默不报错**。
- 收盘集合竞价（14:57–15:00）的**交叉簿**（bid1 ≥ ask1，如 bid 涨停 / ask 跌停）是**预期表示**，不是 bug——pre-event 下交叉残量本就该显示，build_tick3s 照常消费。

---

## 3. 输出 schema（约 215 列）

| 分组 | 列（示例） | 说明 |
|---|---|---|
| 标识/时间 | `tradedate, instrumentID, symbol, event_time, dateTime`(17位 YYYYMMDDHHMMSSmmm), `dataFlag` | dataFlag: 0=正常 / 1=集合竞价 / 2=无变动 |
| 订单流(wt/zb/cl×买卖) | `wtBidNum…clAskVol…clAskAmt, wtBidp…, wtBidp1/v1…` | 3 秒内委托/成交/撤单的笔数/量/金额/价 |
| 主动·被动拆分 | `wtABidVol/wtAAskNum…, wtPBidVol/wtPAskPos…` | 主动(A)/被动(P) |
| 按档位分桶 | `wtBid1…, wtBid2_4…, wtBid5_10…, wtBid11Inf…` | 1档 / 2-4档 / 5-10档 / 11档外 |
| 按 type 分类 | `bidType1Vol…bidType6Num, askType1…6` | 用 builder 的 `type` 字段(1-6) |
| 挂单 pending | `pendingBid1/2_4/5_10Num`(快照 bid_num) + `pendingBidTotalNum/Vol/Amt`(全深度 FIFO 簿) | ⚠️ Total 用**全深度自建簿**(所有价位), 分档用**top-10 快照** |
| 10 档盘口 | `bidPrice1-10, bidVolume1-10, askPrice1-10, askVolume1-10` | 来自 post_bdp(下一行 pre-event 推导) |
| OHLC/汇总 | `totalVolume, totalAmount, open/high/low/lastPrice, dayHigh/dayLow, vwap` | |

---

## 4. 构建

依赖：**HDF5** (必需)、**clickhouse-cpp** (写 ClickHouse 时需要；含 abseil/cityhash/lz4/zstd 传递依赖)。

```bash
./build.sh deps      # 首次：安装依赖 (apt + 源码编 abseil/cityhash/clickhouse-cpp)
./build.sh build     # 编译 → build/lob_processor
./build.sh           # 编译并打包到 dist/
./build.sh clean
```

### 只要 Parquet、不连 ClickHouse

若环境没有 clickhouse-cpp（或无网装），可用 header-only stub 替身让其链接通过（parquet 路径不会连服务器）：

```bash
# 1) 写 stub 头 (clickhouse/{client,block}.h + columns/{column,numeric,string,date}.h)
#    只需为这些类提供空实现:
#      ClientOptions(.SetHost/Port/User/Password/SetDefaultDatabase)
#      Client(ctor, Insert), Block(AppendColumn), ColumnXxx(Append)
# 2) 直接 g++ 编译 (HDF5 + stub, 无需 absl/cityhash/clickhouse 库):
g++ -O2 -std=c++17 -I<ch_stub> -I<hdf5>/include \
    main.cpp lob_data_processor.cpp -o lob_processor \
    -L<hdf5>/lib -lhdf5_cpp -lhdf5 -lpthread -lstdc++fs
```

---

## 5. 运行

```bash
# 当天 / 指定日期 / 日期范围
./build/lob_processor
./build/lob_processor 20260611
./build/lob_processor 20260601 20260611
```

输入默认从 `{h5_base}/{YYYYMMDD}/` 读 `sh600396.h5` / `sz002138.h5`（带市场前缀的文件名）。
Level2 数据**可选**（`{level2_base}/{年}/{日期}/`，缺失则跳过，主数据来自 LOB H5）。

四个数据目录都不写在代码里，默认值由 `qr.toml` / `QR_*` 环境变量解析（见 `qr/qr_paths.h`），
命令行还能再覆盖。`lob_processor --help` 会打印出当前实际生效的值。

### 选项

| 选项 | 说明 |
|---|---|
| `--market <sz\|sh\|all>` | 市场过滤（默认 all，按文件名前两字符 sh/sz） |
| `-s, --symbol <code>` | 股票过滤（如 `600396` / `000001`） |
| `-t, --table <name>` | ClickHouse 表名（默认 `Tick_3sec_data`） |
| `--time-start <HH:MM:SS>` | 插入起始时间 |
| `--time-end <HH:MM:SS>` | 插入结束时间 |
| `--output <ch\|parquet\|both>` | 输出目标（默认 ch） |
| `--h5-dir <path>` | 逐笔 LOB 的 H5 输入根目录（默认取 `qr.toml` 的 `lob_h5`） |
| `--level2-dir <path>` | Level2 快照输入根目录（默认 `lob_level2`） |
| `--csv-dir <path>` | 3 秒切片 CSV 输出根目录（默认 `lob_3s`） |
| `--parquet-out-dir <path>` | Parquet 分片根目录（默认 `tick3s_parquet`） |

写 ClickHouse 的连接信息从环境变量读，不进源码:
`QR_CH_HOST` / `QR_CH_PORT` / `QR_CH_USER` / `QR_CH_PASSWORD` / `QR_CH_DATABASE`。

```bash
# 例: 只生成 parquet, 单只股票
./build/lob_processor --output parquet --parquet-out-dir /tmp/out -s 600396 20260611
```

`run.sh` 会自动探测并设置 `HDF5_PLUGIN_PATH`（PyTables/blosc 格式必需）。

---

## 6. Parquet 输出 = 二进制分片 + 合并

`--output parquet` 写 **二进制分片** 到 `{parquet-out-dir}/.staging/{YYYYMMDD}/{symbol}.bin`，
再用 [`scripts/merge_3s_parquet.py`](scripts/merge_3s_parquet.py) 合并为 `{YYYYMMDD}_tick.parquet`：

```bash
./build/lob_processor --output parquet 20260611     # 出分片
python scripts/merge_3s_parquet.py -d 20260611      # 合并
python scripts/merge_3s_parquet.py -d 20260611 --clean   # 合并后删分片
```

不传 `--staging` / `--out` 时按 `qr.toml` 的 `tick3s_parquet` 解析，与 `lob_processor` 的默认值一致。
不合并也行，`scripts/compare_quick.py` 能直接对 `.bin` 分片做校验。
读分片的代码在 [`qr/shardbin.py`](../../qr/shardbin.py)，`auction` 的 action 分片共用同一个读取器。

`.bin` 格式（little-endian, 列主序, 自描述）：

```
[0..8)   magic "L3SBIN01"
[8..12)  uint32 num_cols
[12..16) uint32 num_rows
每列:    uint16 name_len | name(UTF-8) | uint8 type_code (1=int32 2=int64 3=float64)
body:    按列顺序, 每列 num_rows * sizeof(type) 字节
```

列顺序对齐 `nova/export_tick_3s.py` 的 FIELDS（约 209 列），Python 端按序 `np.frombuffer` 即可还原 Arrow Table。

---

## 7. 文件

```
build_tick3s/
├── main.cpp                  CLI 解析、日期/市场/股票调度、多线程 worker
├── lob_data_processor.h      OrderRecord/TradeRecord/AggregatedData 结构 + 类声明
├── lob_data_processor.cpp    核心实现（4215 行，本项目最大文件）
├── scripts/                  Python 对比工具
├── build.sh / Makefile / CMakeLists.txt   三套构建入口（见下）
└── run.sh                    运行封装（自动设 HDF5 blosc 插件路径）
```

> clickhouse-cpp 桩头不在本项目内,而在仓库根 [`third_party/clickhouse/`](../../third_party/)。

| 文件 | 作用 |
|---|---|
| `main.cpp` | CLI 解析、日期/市场/股票调度、多线程 worker |
| `lob_data_processor.h` | `OrderRecord`/`TradeRecord`/`AggregatedData` 结构 + 类声明 |
| `lob_data_processor.cpp` | `H5DataReader`(读 LOB H5)、`TickAggregator`(3s 聚合)、`LobDataProcessor`、`ClickHouseManager`、二进制分片写出 |
| `scripts/compare_parquet.py` | 与参考 Tick_3s parquet 逐列比对（215 列，容差 1%）<br>`python3 scripts/compare_parquet.py <generated.parquet> <ref_dir_or_file> [--date 20260611] [--symbol 600396]` |
| `scripts/compare_quick.py` | 快速对比 `.bin` 分片与参考 parquet（不必先合并成 parquet）<br>`python scripts/compare_quick.py [--bin-dir DIR] [--ref FILE] [--date DATE] [--cutoff HH:MM]` |
| `run.sh` | 运行封装（自动检测并设置 HDF5 blosc 插件路径，PyTables 格式必需） |

### 三套构建入口的分工

| 入口 | 用途 | 备注 |
|---|---|---|
| `Makefile` | **日常开发首选**：`make` / `make run-4stocks` / `make compare` / `make clean` | HDF5 默认取 `/tmp/hdf5_extracted/usr`，首次用 `make setup-hdf5` 从系统 deb 解压；可 `make HDF5_BASE=/your/path` 覆盖。ClickHouse 用仓库根 `third_party/clickhouse/` 的桩头，无需额外安装 |
| `build.sh` | **构建 + 打包**：`./build.sh` / `clean` / `deps` / `build` / `pack` | 含依赖自动安装 |
| `CMakeLists.txt` | 跨平台优先（`add_executable(lob_processor ...)`） | 三者产出同一个 `build/lob_processor` |

---

## 8. 重要约定速记

1. **输入 LOB H5 必须是 pre-event 口径**（builder 端别改成 post-event）。
2. **14:57–15:00 收盘集合竞价显示交叉簿**是预期行为。
3. `pendingBidTotalNum/Vol/Amt` = **全深度自建簿**统计（所有价位），与 top-10 快照的 `pendingBid1/2_4/5_10` 不是一回事——前者通常远大于后十档之和（深市买盘可达数十倍）。
4. 输出含开盘集合竞价(09:15–09:25)桶，下游若不需要可自行过滤。
