# auction — 集合竞价 LOB Builder(面向生产)

[A-share overview / A 股入口](../README.md) · [Repository / 全仓](../../README.md) · [Setup / 环境](../../docs/SETUP.md)

## English quick start

Auction book reconstruction and indicative matching snapshots. Read the A-share data contracts before changing auction semantics.

Commands below run from the repository root, using conda `mx` for Python:

```bash
cmake -S ashare/auction/cpp -B ashare/auction/cpp/build
cmake --build ashare/auction/cpp/build -j4
```

See the [A-share guide](../README.md) for data contracts, paths and research limitations.
The detailed Chinese implementation reference follows. Its component-local commands assume
`cd ashare/auction` unless they explicitly start from the repository root.

## 中文技术说明

下文保留原项目技术细节；项目内相对命令以 `ashare/auction` 为工作目录，仓库根命令另行标明。

面向生产环境的 A 股逐笔订单簿重建管线。在全天普通 LOB 重建之外,**专门针对开盘(09:15–09:25)
和收盘(14:57–15:00)两个集合竞价窗口做逐事件虚拟撮合**,产出一张独立的 action 快照表。
这是排查用的验证工具(`datacheck_final`)完全不具备的能力。

路径写法 `<tl_zip>` / `<auction_mx>` 是 `qr.toml` 里的键,由 `qr/qr_paths.h` 解析,
也可以用 `QR_TL_ZIP` / `QR_AUCTION_MX` 环境变量临时覆盖。

```
<tl_zip>/<date>/mdl_*.csv[.zip]
        │  (直接吃原始通联 mdl 行情, 不经 Flow 转换)
        ▼
  auction_lob_main  ──►  {sh|sz}{symbol}_action.bin   每股一个分片
        │                 (紧凑列主序二进制)
        ▼
  merge_action_parquet.py  ──►  <auction_mx>/<date>_auction.parquet   当日全市场
```

---

## 1. 与其他项目的关系

| 项目 | 定位 | 与本项目的关系 |
|---|---|---|
| `datacheck_final` | 排查/验证工具 | **同源但独立演化**。本项目的多处规则修复标注"移植自 datacheck_final"(源码内 13 处) |
| `vorder_sim` | 虚拟订单模拟器 | 反向复用:vsim 的集合竞价清算逻辑是从本项目移植过去的 |
| `build_tick3s` | 3 秒降频 | 下游消费者,依赖同一套 **pre-event 盘口快照**惯例 |

**共享的规则修复**(源码注释里都有标注,改动时需三处同步检查):
- **Fix5 SH「先吃后挂」双重扣减** — `auction_lob_builder.cpp:1020, 1109, 1763`
- **SH 委托量补全**(`max(A_qty, 成交+撤单)`) — `:1199, 1756`
- **SZ 特殊委托价格解析**('U' 本方最优 → best-self;市价 → 对手最优) — `:1766`
- **盘口快照取「本笔事件发生前」状态** — `:1748`
- **分类/位置用插入前盘口算**(`calcType` 必须在 `handleOrder` 之前) — `:1779, 1826`

---

## 2. 集合竞价虚拟撮合(本项目的核心新增能力)

在两个竞价窗口内,真实订单簿按交易所规则**允许交叉累积、不做实时撮合**。
每来一笔新事件,就在订单簿的**副本**上跑一次 A 股集合竞价撮合规则
(最大成交量优先 → 买卖失衡量最小 → 价格优先),算出理论撮合价/成交量/失衡度,
产出一行 `ActionRecord`,**不影响真实簿子状态**。

`ActionRecord` 字段(`auction_lob_builder.h:303`):

| 组 | 字段 | 说明 |
|---|---|---|
| 标识 | `dateTime`(17 位整秒), `eventSeq` | eventSeq = 当日该股逐笔处理顺序,用于同秒多事件稳定排序 |
| 撮合结果 | `close`, `totalVolume`, `totalAmt`, `totalNum` | 虚拟均衡价 / 匹配量 / 匹配额 / 订单级 FIFO 成交笔数;**无可撮合时 close 为 NaN** |
| 剩余盘口 | `bdp/bdv/akp/akv[10]` | 撮合**后**剩余盘口前 10 档 |
| Tier1 失衡 | `imbalanceVol`, `imbalanceRatio` | 撮合价处 demand−supply(带符号,+为买方剩余)及其归一化 |
| Tier1 全深度 | `totalBidVol/AskVol/BidNum/AskNum` | 撮合**前**真实全深度挂单量/笔数(仅 C++ 全簿可得) |
| Tier2 事件流 | `cancelVolCum`, `cancelNumCum` | 当前竞价段内累计撤单(开盘/尾盘分别累计) |
| Tier2 | `bidVwapFull`, `askVwapFull`, `bigBidVol`, `bigAskVol` | 全簿加权均价 / 大单量 |

**验证状态**:用四只测试股票与一份独立 Python 原型生成的参考数据集(覆盖开盘竞价窗口)
逐字段比对,`close`、总成交量、总成交额、十档买卖盘价格与挂单量、总笔数等
**44 个重叠字段 100% 匹配**。

---

## 3. 构建

依赖：CMake、C++17 编译器和系统 HDF5；OpenMP 可选。`third_party/hdf5/` 是历史回退，不能视为跨平台自包含安装。

```bash
cd cpp
./build.sh              # 默认 Release
./build.sh debug        # 带符号
./build.sh clean        # 清理构建目录
./build.sh fullclean    # 连 CMake 缓存一起清
```

> 编译历史坑(已修复,勿回退):**LTO 会把 HDF5 的库依赖错误裁剪掉**,导致链接失败。
> 当前优先使用系统 HDF5；历史回退库存在缺失的软链接目标，安装方式见公共环境说明。

---

## 4. 运行

```bash
cd cpp
./build/auction_lob_main -d 20251231                   # 单日全市场
./build/auction_lob_main -d 20251201 -e 20251231       # 日期区间
./build/auction_lob_main -d 20251231 -s 600638 -m SH   # 单只股票
./build/auction_lob_main -d 20251231 -m SZ             # 只处理深圳
./build/auction_lob_main -d 20251231 -f /path/TLData -o /path/output
```

| 选项 | 说明 |
|---|---|
| `-d, --date DATE` | 交易日期 YYYYMMDD(**必需**) |
| `-e, --end-date DATE` | 结束日期,处理区间 |
| `-f, --flow PATH` | 通联数据基础目录 |
| `-o, --output DIR` | 输出基础目录 |
| `-s, --symbol CODE` | 证券代码过滤(**会下推到读取阶段**,单股场景提前跳过无关行) |
| `-m, --market SH\|SZ` | 市场过滤 |
| `-w, --workers N` | 并行线程数(默认 CPU 核心数) |
| `-v, --verbose` | 详细输出 |

**输出**:每股一个 `{sh|sz}{symbol}_action.bin` 分片(紧凑列主序二进制)。
逐笔 LOB order/trade 表由 `TLLobBuilder::writeH5` 支持,但**主程序当前不写 order/trade H5**,
只输出 action 分片。

### 批量生产:`run.sh`

`run.sh` 是三步日期区间批处理(解压 mdl zip → 重建 + 撮合 → merge 成 parquet)。
⚠️ 日期区间**硬编码在脚本体内**(`start_date` / `end_date`),需编辑脚本而非传参。

第 3 步的合并也可以单独跑:

```bash
./cpp/build/auction_lob_main -d 20240130 -m SH   # 出 *_action.bin 分片
python merge_action_parquet.py -d 20240130       # 合并成 <date>_auction.parquet
python merge_action_parquet.py -d 20240130 --clean   # 合并后删分片
```

不传 `--staging` / `--out` 时按 `qr.toml` 的 `auction_mx` 解析,与 `run.sh` 的默认值一致。
合并结果比分片多三列:`symbol`(带市场前缀的文件名)、`market`(SH/SZ)、`tradedate`。
`market` 取自文件名而不是 `instrumentID`,因为后者是纯数字,深圳的 `000001` 会被解析成 `1`。

---

## 5. 性能

原读取为单线程顺序扫描,全市场耗时较长。两项优化叠加后从**十几分钟降到一分钟以内**:

1. **分块并行读取** — 文件按行边界切分成若干块、多线程分别解析后按块序合并
   (保证与单线程结果完全一致),约 **4–5×**;
2. **代码过滤下推** — 把 `-s` 指定的证券代码过滤下推到读取阶段,单股场景提前跳过
   无关行的解析与内存占用,再约 **3×**。

并行化前后用**逐字段比对**(而非文件哈希)验证结果完全一致。

---

## 6. 已知问题与待办

1. **输出盘口表缺两个挂单笔数字段**
   相比验证工具(`datacheck_final`),本项目输出的盘口表还少两个挂单笔数字段,待补齐。

2. **同毫秒事件排序惯例未与验证工具统一**
   同一毫秒内多笔事件的排序和成交归属惯例与 `datacheck_final` 不同。
   已确认这是**展示口径差异、不影响数据内容**,但要做跨工具逐行比对时需专门处理。

### 已修复(勿回退)

- **盘口快照时序惯例**:原为"先改簿、后拍快照",正确约定是**先拍快照、后改簿**——
  每行记录的是这笔事件**发生前**的盘口状态,这也是下游工具消费这份数据时的默认约定。
  时序错位曾导致几乎每一行的挂单量/委托分类/位置字段整体错一拍,是比对中影响面最大的一处。
- **SZ 市价单/本方最优单价格解析缺失**:这两类委托在原始数据里价格字段是无意义的占位值
  (涨停/地板价),需在入簿前翻译成当时盘口的真实可用价。原本这段翻译逻辑完全缺失,
  导致该类委托被按占位值插入到错误价位。
  (已确认集合竞价阶段本身不存在这两类委托——交易所规则不允许——故不影响竞价快照功能。)
- **收市后自动追加汇总行**:已去掉,使输出行数与验证工具完全对齐。

两处修复后重新做全字段比对(排除同毫秒排序造成的展示顺序差异),四只股票的委托身份、
十档盘口、分类、位置等**全部字段 100% 一致**,且集合竞价快照比对结果保持不变(无回归)。

---

## 7. 文件

```
auction/
├── merge_action_parquet.py       合并每股 action 分片为当日 parquet(生产第 3 步)
└── cpp/
    ├── auction_lob_builder.h     Builder 接口 + LobRecord/ActionRecord 结构 (496 行)
    ├── auction_lob_builder.cpp   重建 + 虚拟撮合实现 (2320 行)
    ├── auction_lob_main.cpp      CLI 入口 / 扫描代码 / 并行调度 (540 行)
    ├── build.sh                  编译脚本(依赖检测/Debug-Release/清理)
    ├── run.sh                    批量生产流程(见 §4,注意 §6.1)
    └── CMakeLists.txt
```

> HDF5 依赖不在本项目内,而在仓库根 [`third_party/hdf5/`](../../third_party/),
> 与 `datacheck` / `vorder_sim` 共用同一份。
