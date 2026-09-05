# datacheck

沪深两市 Level-2 数据检查与 LOB 重建工具集，包含两个可执行文件：

- **`datacheck`**：事件对比（Flow vs TL）+ LOB 重建，输出 CSV 或 H5
- **`lob_compare`**：对比两个 LOB 输出目录（支持 CSV 和 H5），输出 Markdown 报告

两种模式可以同时运行，共享一次 IO 扫描。

---

## 编译

```bash
cd /home/yhzhou/datacheck
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物：`build/datacheck`、`build/lob_compare`

依赖 HDF5 1.10（已本地化至仓库根 `third_party/hdf5/`，与 vorder_sim / auction 共用一份，无需系统安装）。

---

## 数据源

| 类型 | 路径示例 | 格式 |
|---|---|---|
| Flow | `/home/yhzhou/Flow/{date}/` | 目录，每个时间片一个 CSV，沪深均有 |
| TL SH | `/home/yhzhou/TLData/{date}/mdl_4_24_0.csv` | 单文件，含 BizIndex / Type A/D/T |
| TL SZ 委托 | `/home/yhzhou/TLData/{date}/mdl_6_33_0.csv` | 深圳委托，含 ApplSeqNum/Side/OrdType |
| TL SZ 成交 | `/home/yhzhou/TLData/{date}/mdl_6_36_0.csv` | 深圳成交/撤单，含 BidApplSeqNum/OfferApplSeqNum/ExecType |
| TL LOB H5 | `/home/yhzhou/Lob_new/{date}/{sh\|sz}{sym}.h5` | compound-type HDF5，含 order / trade_1500 |

---

## LOB Builder 架构

`lob_builder.cpp` 是数据源无关的统一引擎，只接受 `UnifiedRecord`。
`datacheck_main.cpp` 中的 parser（`parseFlowDirCombined` / `parseTLFileCombined` / `parseTLSZFilesCombined`）负责把各自格式转成 `UnifiedRecord`，再统一送入 `LobBuilder::load()` + `build()`。

因此 **`--primary-type flow` 和 `--primary-type tl` 共享完全相同的 LOB 重建逻辑**，可以互换使用，结果差异仅来自数据源本身。

### ⚠️ 本 Builder 已被两个项目复制，改动需三处同步

`lob_builder.{h,cpp}` **没有做成共享库，而是被拷贝到了另外两个项目里**。
本文件是**基准实现**，下面记录的每一处规则修复都只落在这一份上——
**在这里修了 bug，不会自动传播到另外两处。**

| 位置 | 关系 | 状态 |
|---|---|---|
| `datacheck_final/lob_builder.cpp` | **基准** | — |
| `vorder_sim/lob_builder.cpp` | **拷贝 + 附加扩展** | 相对基准 +112 行 |
| `auction/cpp/auction_lob_builder.cpp` | **独立实现**，靠移植保持一致 | 源码内 13 处标注「移植自 datacheck_final」 |

`vorder_sim` 那份的差异是**纯附加、不改重建语义**的（已核对）：

- `setEventHook()` — 每条记录处理完触发的**只读**观察者回调，供虚拟订单追踪器旁挂；
- `setLiteMode()` — 跳过 `orderTable_`/`tradeTable_` 的构造（`getCurrentLob`/`calcPosition`/
  派生记录/距离比例等只为输出记录服务的重活），vsim 不用这两张表；
- `bestBid()` / `bestAsk()` — 聚合盘口只读访问器，供 uncross 与最优价查询。

**因此两份的重建规则目前是一致的。**但一旦本文件再修一处规则（尤其是下方
「未解决」列表里的第 10/12 条），必须同步检查另外两处，否则三个工具会给出不同的盘口。

---

## datacheck 用法

### 1. TL 主源重建 LOB + TL vs Flow 事件对比（推荐）

TL 数据重建 LOB，同时与 Flow 做事件对比，输出 H5 格式，只跑 SH A股（`--sym-prefix 6`）：

```bash
DATE=20260611
./build/datacheck \
  --all --sym-prefix 6 \
  --date ${DATE} --market sh \
  --primary-type tl --primary-path /home/yhzhou/TLData/${DATE}/mdl_4_24_0.csv \
  --compare-type flow --compare-path /home/yhzhou/Flow/${DATE} \
  --output-dir /home/yhzhou/dc_out/${DATE} \
  --gen-lob --output-format h5 \
  --lob-dir /home/yhzhou/dc_out/${DATE}/lob_h5 \
  --workers 40
```

生成的 `lob_h5/` 再与 TL 官方 LOB（`Lob_new/`）对比：

```bash
./build/lob_compare \
  --dir-a /home/yhzhou/dc_out/${DATE}/lob_h5 \
  --dir-b /home/yhzhou/Lob_new/${DATE} \
  --file-prefix-b sh \
  --sysid-mod-b 1000000000000 \
  --label-a dc --label-b tl \
  --date ${DATE} \
  --output /home/yhzhou/dc_out/${DATE}/cmp_dc_tl_lob.md \
  --workers 40
```

> `--sysid-mod-b 1000000000000`：TL 的 sysid 编码为 `Channel × 10¹² + 交易所订单号`，取模后与 dc 的原始订单号对齐。

### 1b. 深圳市场（SZ）：TL 主源重建 LOB + TL vs Flow 事件对比

深圳行情拆成两个文件（委托 `mdl_6_33` + 成交/撤单 `mdl_6_36`），需要 `--primary-path2` 指定成交文件：

```bash
DATE=20260611
./build/datacheck \
  --all --date ${DATE} --market sz \
  --primary-type tl --primary-path /home/yhzhou/TLData/${DATE}/mdl_6_33_0.csv \
  --primary-path2 /home/yhzhou/TLData/${DATE}/mdl_6_36_0.csv \
  --compare-type flow --compare-path /home/yhzhou/Flow/${DATE} \
  --output-dir /home/yhzhou/dc_out/${DATE} \
  --gen-lob --output-format h5 \
  --workers 40
```

跟 TL 官方 LOB 对比（SZ 同样需要 `--sysid-mod-b`）：

```bash
./build/lob_compare \
  --dir-a /home/yhzhou/dc_out/${DATE}/lob_sz \
  --dir-b /home/yhzhou/Lob_new/${DATE} \
  --file-prefix-b sz \
  --sysid-mod-b 1000000000000 \
  --label-a dc --label-b tl \
  --date ${DATE} \
  --output /home/yhzhou/dc_out/${DATE}/cmp_dc_tl_lob_sz.md \
  --workers 40
```

### 2. Flow 主源重建 LOB（无 TL 数据时）

LOB builder 数据源无关，Flow 同样可作主源。省略 `--compare-*` 只做 LOB 重建：

```bash
DATE=20260611
./build/datacheck \
  --all --sym-prefix 6 \
  --date ${DATE} --market sh \
  --primary-type flow --primary-path /home/yhzhou/Flow/${DATE} \
  --output-dir /home/yhzhou/dc_out/${DATE} \
  --gen-lob --output-format h5 \
  --lob-dir /home/yhzhou/dc_out/${DATE}/lob_flow_h5 \
  --workers 40
```

### 3. 仅事件对比（不生成 LOB）

```bash
./build/datacheck \
  --all --date 20260611 \
  --primary-type tl --primary-path /home/yhzhou/TLData/20260611/mdl_4_24_0.csv \
  --compare-type flow --compare-path /home/yhzhou/Flow/20260611 \
  --output-dir /home/yhzhou/dc_out/20260611 \
  --workers 40
```

### 4. 单只股票调试

```bash
./build/datacheck \
  --symbol 600000 --date 20260611 \
  --primary-type tl --primary-path /home/yhzhou/TLData/20260611/mdl_4_24_0.csv \
  --compare-type flow --compare-path /home/yhzhou/Flow/20260611 \
  --gen-lob --output-format h5 \
  --lob-dir /tmp/lob_test
```

---

## datacheck 参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `--symbol SYM` | 单只股票 | — |
| `--symbols A,B,...` | 多只股票（逗号分隔） | — |
| `--all` | 自动扫描所有股票（需要 TL 数据源） | — |
| `--sym-prefix P` | 只保留以 P 开头的股票代码（如 `6` 只跑 6xxxxx A股） | — |
| `--date YYYYMMDD` | 交易日期 | — |
| `--market sh\|sz` | 市场 | `sh` |
| `--primary-type flow\|tl` | 主源类型（同时用于 LOB 重建） | `flow` |
| `--primary-path PATH` | 主源路径（SZ+tl 时为委托文件 `mdl_6_33`） | 必填 |
| `--primary-path2 PATH` | 深圳主源成交文件 `mdl_6_36`（`--market sz --primary-type tl` 时必填） | — |
| `--compare-type flow\|tl` | 比对源类型 | `tl` |
| `--compare-path PATH` | 比对源路径（省略则跳过事件对比，需配合 `--gen-lob`） | 可选 |
| `--compare-path2 PATH` | 深圳比对源成交文件（`--market sz --compare-type tl` 时必填） | — |
| `--output-dir DIR` | 事件对比报告目录（生成 `_summary_{date}_{market}.md`） | — |
| `--gen-lob` | 启用 LOB 重建（用主源数据） | false |
| `--lob-dir DIR` | LOB 输出目录 | `output-dir/lob_{market}` |
| `--lob-start HH:MM` | LOB 时间段起点 | 09:15 |
| `--lob-end HH:MM` | LOB 时间段终点 | 15:00 |
| `--output-format csv\|h5\|both` | LOB 输出格式 | `csv` |
| `--workers N` | 并行线程数 | 40 |
| `--min-src-events N` | 过滤主源事件不足 N 条的股票 | 0 |

---

## lob_compare 用法

对比两个 LOB 目录，自动识别 CSV（`{sym}_order.csv`）和 H5（`{sym}.h5`）格式。

```bash
./build/lob_compare \
  --dir-a /home/yhzhou/dc_out/20260611/lob_h5 \
  --dir-b /home/yhzhou/Lob_new/20260611 \
  --file-prefix-b sh \
  --sysid-mod-b 1000000000000 \
  --label-a dc --label-b tl \
  --date 20260611 \
  --output /home/yhzhou/dc_out/20260611/cmp_dc_tl_lob.md \
  --workers 40
```

### lob_compare 参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `--dir-a PATH` | LOB 目录 A | 必填 |
| `--dir-b PATH` | LOB 目录 B | 必填 |
| `--label-a STR` | 报告中 A 的标签 | `A` |
| `--label-b STR` | 报告中 B 的标签 | `B` |
| `--file-prefix-b PFX` | dir-b 文件名前缀（如 `sh`），发现时剥去，读取时补回 | — |
| `--sysid-offset-b N` | dir-b sysid 减去固定偏移 N | 0 |
| `--sysid-mod-b N` | dir-b sysid 取模 N（如 `1000000000000` 剥去 TL Channel 前缀） | 0 |
| `--date YYYYMMDD` | 报告标题中的日期 | — |
| `--output FILE` | 输出 Markdown 报告路径 | `lob_compare_{date}.md` |
| `--workers N` | 并行线程数 | 40 |
| `--top N` | 报告中列出差异最大的前 N 只股票 | 50 |

---

## 输出结构

```
dc_out/{date}/
├── _summary_{date}_sh.md       # SH 事件对比汇总
├── _summary_{date}_sz.md       # SZ 事件对比汇总
├── lob_sh/                     # SH LOB 输出（默认目录）
│   └── {sym}.h5
├── lob_sz/                     # SZ LOB 输出
│   └── {sym}.h5                # 含 dataset: "order" 和 "trade_1500"
└── cmp_dc_tl_lob.md            # lob_compare 报告
```

### H5 格式说明

每个 `{sym}.h5` 包含两个根级 dataset（compound type，gzip deflate(1) 压缩，chunk=10000）：

| Dataset | 关键字段 |
|---|---|
| `order` | `time`, `sysid`, `ordertype`, `direction`, `price`, `volume`, `tradevolume`, `bid_position`, `ask_position`, `type`, `bid_levels`, `ask_levels`, `bdp1~10`, `bdv1~10`(double), `akp1~10`, `akv1~10`(double) |
| `trade_1500` | `sysid`, `wt_volume`, `trans_vlm`, `vwap`, `act_vlm`, `pas_vlm`, ... |

Python 读取示例：

```python
import h5py, pandas as pd
with h5py.File('600000.h5', 'r') as f:
    order = pd.DataFrame(f['order'][:])
    trade = pd.DataFrame(f['trade_1500'][:])
```

---

## LOB 重建逻辑

- **排序**：SH 按 `BizIndex`（seqNo，全局有序）；Flow 按 `appSeq`；SZ 委托/成交按 `ApplSeqNum`（同一 Channel 内共享全局递增序号，已验证连续无重复）
- **SH 委托/成交顺序修复**：TL 对立即成交的主动单先撮合后回报委托，导致委托记录的 `BizIndex` 可能晚于其首笔成交记录。`LobBuilder::build()` 在排序前先把每笔委托的 seqNo 提前到其首笔成交之前（`resequenceSHOrderBeforeTrade`），保证 `wt` 始终在 `zb` 前出现，盘口扣减顺序正确。Flow 同理（appSeq 存在相同问题）。
- **市价单方案B（已按 Fix6 收窄）**：`priceType == '1'`（市价/对手方最优）的委托没有真实限价。仅当对手盘为空（价格无法解析为真实值）时才只放入 `allOrders_` 供成交扣减、不挂盘口；对手盘非空时（展示价已解析为到达时对手盘最优价，是真实可用价格）按普通激进单走"先吃后挂"（`waitEnqueue`），残量正确挂入簿中并被后续成交继续扣减。详见 Fix6。
- **SH 委托量补全（入簿量与展示量分离，见 Fix5）**：SH 立即成交主动单的委托记录 `A_qty` 只记录了挂住部分（可能小于实际下单量）。`build()` 在输出 `wt` 行的**展示量**时取 `max(A_qty, 成交量+撤单量之和)`（`orderFilled_`），让委托量守恒；但**入簿量**（传给 `handleOrder` 的量）对"先吃后挂"单（`resequenceSHOrderBeforeTrade` 提前过 seqNo 的单）必须用原始 `A_qty`（挂住量），否则会把已经算进 `A_qty` 净额的"先吃"部分重复计入。配合 `handleTrade` 对这类单跳过其原始 A 序号之前的成交扣减（`shOrderOrigSeq_`），避免双重扣减。
- **Wait Queue**：连续竞价期间，买价 ≥ 卖一 / 卖价 ≤ 买一时进等待队列；出队条件为严格不等式（`< 卖一` / `> 买一`），与入队条件互补不重叠，避免打平对手价的单被提前整笔放回盘口。
- **委托分类/位置用插入前盘口**：`calcType()`/`calcPosition()` 在 `handleOrder()` 把新订单插进买卖盘之前调用，避免订单把自己算进当前盘口。
- **`handleTrade` 量夹紧**：成交扣减时用 `min(trade_vol, order_remaining)`，避免订单量变负、`bestBidList_`/`bestAskList_` 残留量清不掉（实测：涨停价撤回后盘口长期卡在涨停价的根因）。
- **Derived Orders**：仅 SH 需要——市价单无显式委托记录，从成交反推补建虚拟委托；SZ 委托/成交/撤单都有显式记录，不存在这个问题。
- **15:00:01 收尾行**：LOB 重建结束后自动追加一行时间为 `15:00:01.000` 的末行快照，方便直接比对收盘盘口状态。

---

## 已知问题与修复记录

### 已修复

1. **委托分类字段用了"插入后"的盘口状态算（影响 SH + SZ，已修复）**
   `calcPosition()`/`calcType()` 必须在 `handleOrder()` 之前调用。原来顺序颠倒，导致订单永远分类不到 type=4（价差内/改善盘口），只落到 type=5。修复后 SH Top 50 差异股票"分类错"总和从 1,624,150 降到 468,744（降幅 71%）。

2. **SZ 委托/成交排序改用真实 `ApplSeqNum`（影响 SZ，已修复）**
   深圳委托（mdl_6_33）和成交/撤单（mdl_6_36）是两个独立文件，同一 Channel 内两个文件的 `ApplSeqNum` 共享全局递增序号（已验证：合并后 1..N 无重复无空洞），直接用 `ApplSeqNum` 当排序键，不再靠时间戳猜顺序。

3. **SZ 等待队列出队条件边界重叠（影响 SZ，已修复，影响最大）**
   入队条件含等于（`ip >= 卖一`），出队也含等于，导致打平对手价的单进队后被提前整笔放回盘口，盘口分叉且不自纠。改为严格不等式（`ip < 卖一` / `ip > 买一`）后，000636 分类不一致率从 55% 降到 0.4%，成交记录差异全市场从 2881/2881 降到 0/2881。

4. **SH 委托/成交 seqNo 顺序修复（已修复）**
   TL SH 对立即成交的主动单先撮合后回报委托，委托记录的 BizIndex 可能晚于其首笔成交。`resequenceSHOrderBeforeTrade()` 在排序前把这类委托的 seqNo 提前到首笔成交之前。Flow 数据中 appSeq 存在同样问题，同逻辑处理。

5. **市价单以保护价挂进盘口（已修复，2026-07-06 由 Fix6 进一步收窄）**
   `priceType == '1'` 的市价/对手方最优单保护价（市价卖≈1.0）被当成限价挂入盘口，产生虚假价位。改为方案B：市价单只进 `allOrders_` 供成交扣减，不进等待队列、不进 `bestBidList_`/`bestAskList_`。这也是 SZ 未解决问题 6a（dc 卖一 = 1.000）的根因修复。
   **后续发现方案B过于绝对**（见下方第 9 条 Fix6）：市价单未成交残量在真实市场中会继续挂单排队，方案B"一律不挂簿"导致这部分残量的后续成交被 `handleTrade` 错误倒扣，现已改为仅在对手盘为空时才不挂簿。

6. **SH 立即成交主动单委托量偏小（已修复）**
   SH 交易所对立即成交的主动单 `A_qty` 只记录了挂住部分，可能小于实际下单量。`build()` 输出 `wt` 行时取 `max(A_qty, 成交量+撤单量之和)`（`orderFilled_`），让委托量守恒；`handleOrder` 仍用原始 `A_qty` 不影响订单簿。

7. **`handleTrade` 成交量超出订单剩余量时整段跳过扣减（已修复）**
   原逻辑 `if (order->volume < volume) return` 会在成交量超出剩余量时跳过 `bestBidList_`/`bestAskList_` 的更新，导致价格档位永久卡死（实测：涨停价撤回后盘口长期卡在涨停价）。改为 `min(vol, remaining)` 夹紧后始终执行扣减。

8. **SH「先吃后挂」单双重扣减（2026-07-06 已修复，对应 `lob_diff_summary.md`(已归档至 `_archive/`) Fix5）**
   上交所对「委托价越过对手盘、立即部分成交」的主动单，逐笔数据里 A（委托）记录排在其首笔成交之后，且 **A_qty 记的是挂住量**（已净掉 A 之前的立即成交部分），不是原始下单量。`resequenceSHOrderBeforeTrade` 把 A 提前到成交之前后，若 `handleTrade` 再把这些"先吃"部分从 A_qty 里扣一遍，会造成双重扣减（实测 20260611 sh688981 127.18 价位少 1612 股）。
   修复：`resequenceSHOrderBeforeTrade` 额外记录每笔单原始（未提前）的 seqNo（`shOrderOrigSeq_`）；`handleTrade` 对 `原始seqNo < 该单原始A序号` 的成交跳过对该单的主动方扣减（对手方照常扣）；入簿量固定用原始 `A_qty`（挂住量），展示量仍是 `max(A_qty, orderFilled_)` 的补全值，两者用途分离（见上方"SH 委托量补全"条目）。

9. **SZ 市价单残量被后续成交"倒扣"导致价位虚假掏空（2026-07-06 已修复，对应 `lob_diff_summary.md`(已归档至 `_archive/`) Fix6）**
   SZ 市价单（`priceType == '1'`）未完全成交的残量，在真实市场中会继续挂单排队、被后续到达的对手单陆续吃掉（而非"落地即完结"）。旧版"方案B"对市价单一律不挂盘口，但 `handleTrade` 处理这类残量的后续成交时仍会对 `bestBidList_`/`bestAskList_` 做减法——这笔量从未被加进去过，导致该价位被凭空多扣，可能扣到 0 触发 erase（价位从簿中"消失"），连累后续多个 3 秒桶的十档盘口比真实偏少。
   修复：`priceType == '1'` 时，只有对手盘为空（无法解析出真实价格）才继续走"不挂簿"；对手盘非空（价格已解析为到达时对手盘最优价，真实可用）时按普通激进单走"先吃后挂"，与其它订单同一套逻辑。SH 因 `priceType` 恒为 `'2'`（TL-SH 无该字段，硬编码限价），此分支从不触发，无回归风险。

### 未解决（已定位根因）

10. **SH derived order 价格/分类错误（暂无法修复）**
    SH 市价单从成交反推的虚拟委托价格用的是成交价，而 TL 生产系统有原始限价，导致价格和 type 不一致。影响范围已收敛：有显式委托记录的订单价格 100% 一致，价格/分类错误全部来自 derived order。

11. **SH + SZ 收盘集合竞价方向歧义（影响极小）**
    收盘时刻（`15:00:01.000`）成交的主动方判断与 TL 不一致，每只受影响股票固定多出 1 个 sysid，不扩散。

12. **SZ 末行 LOB 快照差异：子类 6b 收盘集合竞价残留（约 324 只，待修复）**
    14:57~15:00 收盘集合竞价期间到来的买单被当连续竞价处理，进 `bidWaitQueue_`；收盘集合竞价成交（15:00:00.000）对等待队列里的买单部分成交后，剩余量留在等待队列，EOD 触发出队时被放回盘口，成为末行多出的 dc 买一。
    待修复方案：检测到首个 `TransactTime=15:00:00.000` 的成交记录时，先批量释放 `bidWaitQueue_` 再执行成交扣减；或把 14:57~15:00 识别为收盘集合竞价、直接入盘口。

---

## 性能参考（20260611，40 workers）

| 模式 | 耗时 |
|---|---|
| SH：TL 主源，全天 LOB，H5 deflate(1)，2312 只 A股 | ~17 分钟 |
| SH：Flow 主源，全天 LOB，H5 输出 | ~11 分钟 |
| SH：lob_compare（2312 只，H5 vs H5） | ~10 分钟 |
| SZ：TL 主源（两个文件），全天 LOB，H5 deflate(1)，4068 只 | ~30 分钟 |
| SZ：lob_compare（2881 只，H5 vs H5） | ~12 分钟 |
