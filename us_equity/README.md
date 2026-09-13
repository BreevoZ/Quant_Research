# US equities · 美股研究

[Repository / 全仓](../README.md) · [A-shares / A 股](../ashare/README.md) ·
[Setup / 环境](../docs/SETUP.md) · [English](#english) · [中文](#中文)

## English

This is the US-equity research starting point: Massive minute bars → data QA → causal features →
delayed return labels → cross-sectional diagnostics. It is not yet an executable trading backtest.
The fixed 12-stock example universe is for pipeline validation, not a historical investable universe.

### Run the offline demo

From the repository root, reuse the `mx` conda environment:

```bash
conda activate mx
python -m pip install -e ".[us]"
python -m us_equity demo
python -m unittest discover -s us_equity/tests -v
```

No credentials are needed. The demo uses 12 synthetic `SYNxx` symbols and three historical sessions:
2025-03-07 and 2025-03-10 around the daylight-saving transition, plus the 2025-11-28 half-day.
Each day deliberately contains a missing minute and an extra premarket bar. Reports and manifests
are marked synthetic and cannot support claims about investment performance.

Outputs go to `data/us_equity/demo_<UTC time>/`. Use `--output /path/to/new_directory` to choose a
new destination. Existing output directories are never overwritten. Python can also be invoked as
`conda run -n mx python ...` without activating a shell.

### Use real Massive minute data

The shared downloader lives in [data_sources/massive](../data_sources/massive/README.md).
From the repository root:

```bash
python -m pip install -e ".[us,massive]"
python data_sources/massive/massive_data_downloader.py flatfiles \
  --keys data_sources/massive/KEYS.txt \
  --asset stocks --data minute \
  --start-date 2026-06-15 --end-date 2026-06-18 \
  --output-dir data/massive/unified --workers 4
python -m us_equity run
```

The default [experiment configuration](experiments/minute_baseline.toml) contains dates, symbols,
lookbacks, horizons, skip duration and minimum cross-section size. Flat Files download the full
market; the research reader filters to the configured symbols. Edit dates and tickers in that file.

If data already exists elsewhere:

```bash
python -m us_equity run \
  --config us_equity/experiments/minute_baseline.toml \
  --input /path/to/flatfiles/stocks/minute_aggs_v1
```

Alternatively, set `us_minute` and `us_research` in repository-root `qr.toml`, or use
`QR_US_MINUTE` / `QR_US_RESEARCH`. Relative configured paths are repository-relative;
explicit CLI paths are relative to the calling directory. See [Setup](../docs/SETUP.md).

### Source contracts

| `source` | Input | Timestamp unit | Price basis |
|---|---|---|---|
| `flatfiles` | `us_stocks_sip/minute_aggs_v1` | Nanosecond epoch | Unadjusted |
| `rest_split_adjusted` | Current repository downloader's REST minute CSVs | Millisecond epoch | Split-adjusted |

REST short fields (`ticker,t,o,h,l,c,v`) and combined long fields are both accepted. To use REST,
copy the experiment configuration, set `source = "rest_split_adjusted"`, point `--input` to either
`rest/stocks/minute_aggs` or `rest_combined/stocks/minute_aggs`, and run a separate experiment.

Sources are not mixed or silently adjusted. `adjusted=true` in the current downloader means split
adjustment, not dividend-inclusive total return. CSV prices cannot prove their adjustment basis;
`source` is the caller's declaration. The reader checks units, dates and duplicates.
All current features and labels stay within a session. Cross-day research will require corporate
actions and explicit raw/adjusted price management.

Files are discovered recursively as `YYYY-MM-DD.csv[.gz]` or `YYYY-MM-DD_<ticker>.csv[.gz]`.
Processing is session-by-session, with chunked CSV reads. Every XNYS session in the selected range must
have input files. Checks cover the selected symbols:

- Required ticker, integer timestamp and OHLCV fields; finite positive prices, nonnegative volume,
  valid high/low bounds and correct New York date/minute alignment.
- Duplicate ticker/minute pairs are rejected, including a retained `.csv.gz` plus its extracted `.csv`.
- Regular-session bar starts are in `[open, close)`, including half-days; pre/postmarket bars are excluded.
- Every configured ticker receives a full clock grid. Missing bars stay NaN; zero-volume prices are unusable.
- Missing bars may mean no qualifying trades, a halt or incomplete files; the reader does not distinguish them.

The XNYS calendar defines the regular cash-equity session, not individual-symbol halts or venue-specific
auction rules. Its package version is recorded in each manifest.

### Features and labels

| Feature | Definition |
|---|---|
| `ret_5m`, `ret_15m`, `ret_60m` | Continuous intraday close returns; lookbacks are configurable |
| `rv_30m` | Square root of the sum of 30 one-minute squared log returns; not annualized |
| `volume_ratio_30m` | Current volume / prior 30-minute average, excluding the current bar |
| `range_pct` | Current `(high-low)/close` |

Features have an explicit registry. Missing history invalidates the affected window; prices are never
forward-filled and history is never borrowed across sessions. No industry-neutral residual or real VWAP
is claimed from these inputs.

For a signal bar `[09:30,09:31)`, `available_at` is 09:31. With `skip_minutes=1`, leave a whole minute
and use the 09:32 open as the entry proxy. A 15-minute label exits at the 09:47 open:

```text
label = open[t + 1 + skip + horizon] / open[t + 1 + skip] - 1
```

`t` is the bar-start minute. Labels do not cross the close; missing bars in the required future interval
invalidate them. Minute open is a price proxy, not an executable fill. Final historical bars also do
not necessarily equal their first real-time publication.

### Outputs and interpretation

| File | Contents |
|---|---|
| `manifest.json` | Configuration, price/time contract, input/code SHA256, dependency versions, run status |
| `qa.json` | Daily row counts, out-of-session rows and missing minutes per symbol |
| `panels/YYYY-MM-DD.parquet` | Bars, availability time, features and labels |
| `bucket_ic.csv` | Sampled cross-sectional IC and high-minus-low return spread |
| `daily_ic.csv` | Equal-weight daily diagnostics |
| `summary.csv`, `report.md` | Across-day summary and readable report |

Only use runs with `manifest.status == "complete"`; failures retain their error and a failed status.
Each horizon is sampled on a non-overlapping grid anchored to the session open. Different horizons
and different days can still be correlated. Rank IC uses the available feature/label pairs and the
configured minimum cross-section size. Tied factor ranks stay together. Valid samples can differ by
feature, so check `n` before comparisons.

Feature direction is not flipped after seeing results. The high-minus-low spread is the difference
between group mean returns in basis points, not portfolio net PnL. Spreads, slippage, impact, borrow,
commissions and position accounting are not modeled. No strategy equity curve, Sharpe or independent-day
t-statistic is reported.

### Next research steps

1. Validate real samples and build point-in-time security identity, listing/delisting and corporate-action data.
2. Lock a hypothesis and selection/test windows; investigate industry/market-adjusted short returns with volume.
3. Add trades/quotes for a small universe to study spreads, trade pressure and price recovery.
4. Add cash/position accounting and executable costs before borrowing or passive-fill assumptions.

The following Chinese section contains the same workflow and technical scope.

## 中文

第一条可运行链路：

```text
Massive minute CSV / CSV.gz
    → 来源口径校验 + XNYS 常规交易日历 + 每股分钟覆盖率
    → 逐日完整分钟网格（缺失保留 NaN）
    → 因果特征 + 延迟 open-to-open 标签
    → 非重叠截面 Rank IC / 高低组收益差
    → Parquet 面板 + CSV 诊断 + Markdown 报告 + manifest
```

这是研究初始化，尚未包含真实成交回测。股票池明确固定为少量股票，仅用于验收管道；
没有历史股票池、行业中性化、公司行动归档、借券或持仓账本，不能据此认定策略可交易。
A 股研究集中在 `ashare/`，两个市场共用 `qr/` 和 `data_sources/`。

### 1. 先跑无需密钥的 demo

在仓库根执行：

```bash
conda activate mx
pip install -e ".[us]"
python -m us_equity demo
```

默认写入 `data/us_equity/demo_<UTC时间>/`，命令会打印报告路径。
也可以用 `--output /path/to/new_demo` 指定一个尚不存在的目录。

demo 使用 12 个 `SYNxx` 合成标的，固定随机种子，并运行三个历史交易日：

- `2025-03-07`：夏令时前，14:30 UTC 开盘，390 分钟。
- `2025-03-10`：夏令时后，13:30 UTC 开盘，390 分钟。
- `2025-11-28`：半日市，纽约时间 13:00 收盘，210 分钟。

每一天故意删除一根分钟 K 线，并加入一根盘前 K 线，以验证缺失处理和时段过滤。
所有 demo 报告与 manifest 都标记 `synthetic`；结果仅验证软件运行。

运行语义测试：

```bash
python -m unittest discover -s us_equity/tests -v
```

### 2. 接入 Massive 真实分钟数据

现有下载器仍是唯一下载入口。安装下载依赖并把数据落在约定位置：

```bash
pip install -e ".[us,massive]"
python data_sources/massive/massive_data_downloader.py flatfiles \
  --keys data_sources/massive/KEYS.txt \
  --asset stocks --data minute \
  --start-date 2026-06-15 --end-date 2026-06-18 \
  --output-dir data/massive/unified --workers 4

python -m us_equity run
```

这会读取默认配置 [`experiments/minute_baseline.toml`](experiments/minute_baseline.toml)。
日期是一个固定的初始化样本周，可以直接编辑配置。
Flat Files 下载的是当天全市场文件；读取阶段才按配置中的股票列表过滤。
不要为了初始化同时下载 options、trades 或 quotes 全量数据。

若数据已在别处，用 CLI 指定 **minute_aggs_v1 目录**：

```bash
python -m us_equity run \
  --config us_equity/experiments/minute_baseline.toml \
  --input /path/to/flatfiles/stocks/minute_aggs_v1
```

或者在仓库根 `qr.toml` 的 `[paths]` 段设置：

```toml
us_minute = "/path/to/flatfiles/stocks/minute_aggs_v1"
us_research = "data/us_equity"
```

也支持 `QR_US_MINUTE` / `QR_US_RESEARCH`。配置中的相对路径锚定仓库根，
CLI 的显式相对路径按调用目录解释。每次默认创建一个新的 `run_<UTC时间>` 目录。
显式 `--output` 也必须是新目录，以免旧面板与新报告混用。

### 3. 数据契约

| 配置 `source` | 来源 | 时间单位 | 价格口径 |
|---|---|---|---|
| `flatfiles` | Massive `us_stocks_sip/minute_aggs_v1` | 纳秒 epoch | 未复权 |
| `rest_split_adjusted` | 当前仓库下载器的 REST minute 输出 | 毫秒 epoch | 拆股复权 |

REST 模式同时支持 `ticker,t,o,h,l,c,v` 和 combined 模式的长字段名。
若使用 REST，复制实验配置并将 `source` 改为 `rest_split_adjusted`，再将 `--input`
指向 `rest/stocks/minute_aggs` 或 `rest_combined/stocks/minute_aggs`，单独跑一个实验。

**本版不做复权换算、不自动推断口径、不合并两种来源。** 同名字段不能保证同一语义。
当前下载器的 `adjusted=true` 指拆股复权，不等同于含分红的总收益复权。
输入 CSV 不携带完整复权元数据，`source` 是调用者对目录来源的明确声明；
程序能拦截单位错误和重复行，不能从价格本身证明复权状态。

所有特征和标签严格限制在同一常规交易日，因此本版不计算隔夜收益，也不把拆股前后价格串起来。
后续做跨日特征、价格筛选和总收益时，必须补公司行动与原始/调整价格管理。

输入按文件名 `YYYY-MM-DD.csv[.gz]` 或 `YYYY-MM-DD_<ticker>.csv[.gz]` 递归发现。
选定区间缺任何一个 XNYS 交易日文件会报错；没有任一配置股票的常规时段数据也会报错。
按日读取，CSV 分块过滤股票；不会一次把多年份原始数据载入内存。

对选定股票执行以下检查：

- 必须有 ticker、时间、OHLCV；时间戳是整数，属于文件对应的纽约日期，并按分钟对齐。
- 价格为正，成交量非负，数值有限，OHLC 高低关系正确。
- `ticker + ts` 重复直接拒绝，包括重复下载的 `.csv` / `.csv.gz`。
- 常规分钟采用 `[session_open, session_close)`，排除盘前、盘后及收盘时刻开始的 bar。
- 每个配置股票每天有完整时钟网格；缺失分钟保留 NaN，零成交量 bar 不参与价格计算。
- 缺失不能自动等同于数据损坏：也可能是没有合格成交或停牌，需后续结合行情状态判断。

日历使用 `exchange_calendars` 的 XNYS，记录依赖版本；它处理常规现金股票时段，
不描述单股停牌或不同场所的竞价规则。

### 4. 特征与标签口径

特征明确注册在 [`features.py`](features.py)，评估不会把任意数值列自动当成因子。

| 特征 | 计算 |
|---|---|
| `ret_5m / ret_15m / ret_60m` | 同日连续分钟 close 收益；窗口可配置 |
| `rv_30m` | 过去 30 个一分钟 log return 平方和的平方根，不年化 |
| `volume_ratio_30m` | 当前成交量 / **不含当前分钟**的前 30 分钟平均量 |
| `range_pct` | 当前 `(high-low)/close` |

任何所需历史分钟缺失，相关滚动特征保持 NaN；不前向填充，不跨日借用数据。
暂不加入未经行业/市场调整的“残差收益”命名，也不从 OHLC 伪造真实 VWAP。

时间轴示例，`skip_minutes=1`、`horizon=15`：

```text
09:30–09:31   特征所属分钟 [t, t+1)
09:31        available_at：该分钟收口，最早可使用特征
09:31–09:32   留出一整分钟
09:32        entry_proxy_open
09:47        exit proxy open
label = open[09:47] / open[09:32] - 1
```

一般公式为 `open[t+1+skip+h] / open[t+1+skip] - 1`，以 **bar 开始时间** 为 t。
标签不会跨收盘，进出与中间区间缺失时保持 NaN。分钟 open 是该窗口的价格代理，
并非保证能在该时刻以该价成交；最终历史 bar 也不等同于实时首次发布版本。

### 5. 输出与评估

```text
run_<UTC时间>/
  manifest.json          # 来源、配置快照、输入 SHA256、代码 SHA256、依赖版本、完成状态
  qa.json                # 每日行数、时段外数据、逐股票缺失分钟
  panels/YYYY-MM-DD.parquet
  bucket_ic.csv          # 每个采样截面的 IC 与高低组收益差
  daily_ic.csv           # 当日各采样截面等权平均
  summary.csv            # 再按交易日等权汇总
  report.md              # 结果、数据质量和限制
```

只有 `manifest.status == "complete"` 的实验可使用；异常会记录 `failed` 和错误信息。
默认 15/30/60 分钟标签，各自按持有期长度采样，起点锚定开盘。
同一持有期内持仓窗口不重叠，不同持有期和不同日期之间仍可能相关。

每个特征与标签按两者都有值的股票计算截面 Rank IC；至少 `min_cross_section` 个标的。
高低组采用平均排名，并保留并列值，不按 ticker 人为拆散。不同特征的有效样本可能不同，
对比时须检查 `bucket_ic.csv` 的 `n`。特征方向不按结果自动翻转。

报告里的“高低组价差”是两组平均收益之差，**没有扣成本，也不是资金曲线**。
本版不输出策略净值、Sharpe、独立日假设下的 t 值或优化后的组合。

### 6. 接下来做什么

1. **数据层**：验证真实样本，补历史股票身份、上市/退市状态、公司行动与流动性股票池。
2. **第一项研究**：行业/市场调整后的短期收益 × 异常成交量，提前锁定选择窗与测试窗。
3. **微观结构**：给小股票池加入 trades/quotes，研究价差、成交压力与价格恢复。
4. **交易验证**：建立完整的持仓/现金账本、主动成交与成本模型，再考虑借券与被动成交。

达到相应数据条件后再新增模块；本版不放空的 backtest 或 L3 模拟器壳子。

参考口径：[Massive Flat Files](https://massive.com/docs/flat-files/stocks/overview)、
[REST 聚合与拆股复权](https://massive.com/docs/rest/stocks/aggregates/custom-bars)、
[交易日历文档](https://github.com/gerrymanoim/exchange_calendars)。
