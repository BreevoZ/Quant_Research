# Massive data reference · 数据参考

[Downloader quick start / 下载器入口](README.md) · [US research / 美股研究](../../us_equity/README.md)

## English orientation

This file preserves the original Chinese CLI/field reference. Use the bilingual downloader README
for installation, credentials and runnable repository-root commands. Script file names and flags are
unchanged; the directory is now `data_sources/massive/`.

- `flatfiles`: daily market-wide S3 gzip CSV downloads, optionally retaining the compressed file.
- `rest`: per-ticker requests, optional date-level merging, pagination and retry handling.
- `catalog`: script dataset combinations; actual availability depends on account permissions/provider support.
- Stock minute Flat Files use nanosecond `window_start` and unadjusted prices.
- The current REST stock aggregate code requests split-adjusted prices and uses millisecond `t`;
  combined outputs rename `t` to `window_start` without changing its unit.
- Stock trades contain trade IDs, prices/sizes, conditions and participant/SIP timestamps, not order IDs.
- Stock quotes provide best bid/ask prices and sizes, not an order-level queue.
- `--max-pages` / `--max-tickers` intentionally truncate; they are not full-history research settings.

The original reference follows. Its schema tables describe the recorded sample files and downloader
implementation; they are not an account entitlement guarantee.

## 中文原始参考

# `massive_data_downloader.py` 使用与数据字段说明

本文档按当前脚本 `massive_data_downloader.py` 的实际逻辑整理，更新时间：2026-07-08。

字段部分主要来自两处：

- 当前项目里已经下载下来的 CSV 表头；
- 对 `quotes_v1` 远端 Flat File 只读取 gzip 文件开头得到的表头，没有下载完整 quote 文件。

注意：REST 返回字段可能随 Massive/Polygon 服务端版本、权限、市场类别变化而增加字段。脚本会把 REST 返回的额外字段自动写入 CSV。

## 1. 脚本支持的三类命令

### 1.1 `catalog`

打印脚本支持的资产类别和数据类型。

```powershell
python massive_data_downloader.py catalog
```

说明：当前脚本里的 `print_catalog()` 文案有一处偏宽松：它写着 indices/forex 支持 trades/quotes，但实际 `--data all` 只会给 indices/forex 拉 `day,minute`。如果显式传 `--data quotes`，脚本会走通用 `/v3/quotes/{ticker}`，但是否有数据取决于服务端和权限。

### 1.2 `flatfiles`

走 Massive Flat Files，适合批量历史下载、全市场数据、回测数据归档。

Flat File 远端对象模板：

```text
https://files.massive.com
bucket: flatfiles
key: {prefix}/{data_type}/YYYY/MM/YYYY-MM-DD.csv.gz
```

例子：

```text
us_stocks_sip/minute_aggs_v1/2026/06/2026-06-18.csv.gz
global_crypto/trades_v1/2026/06/2026-06-18.csv.gz
us_futures_cme/session_aggs_v1/2026/06/2026-06-18.csv.gz
```

脚本会把 `.csv.gz` 下载后解压为 `.csv`。默认不保留 gzip；如果要保留压缩包，加 `--keep-gz`。

### 1.3 `rest`

走 REST API，适合小范围查询、补洞、验证、最新/近期数据、ticker universe 或 reference 查询。

REST 有两种输出模式：

1. 默认 per-ticker 模式：一个 ticker 一个 CSV；
2. combined 模式：先按 ticker 拉，再合并成一个日期级 CSV，结构更像 Flat File。

当使用：

```powershell
--from-flatfiles
```

脚本会自动启用 combined 模式：它会先从本地 Flat File 读取 ticker 列表，再用 REST 逐个 ticker 拉数据，最后合并。

## 2. 通用参数

| 参数 | 含义 |
|---|---|
| `--keys KEYS.txt` | 密钥文件路径，默认 `KEYS.txt`。脚本不会打印密钥。 |
| `--asset` | 资产类别：`stocks,options,indices,futures,forex,crypto,all`，支持逗号分隔。 |
| `--exclude-assets` | 排除某些资产，例如 `--exclude-assets options`。 |
| `--data` | 数据类型。Flat Files: `day,minute,session,trades,quotes,all`；REST: `day,minute,trades,quotes,all`。 |
| `--date` | 单日下载，格式 `YYYY-MM-DD`。 |
| `--start-date` / `--end-date` | 日期区间下载。 |
| `--output-dir` | 输出根目录，默认 `downloads/unified`。 |
| `--workers` | 并发 worker 数，默认 `4`。Flat File 可适当提高，REST 过高容易触发限速。 |
| `--request-delay` | 每个任务提交/HTTP 请求前等待的秒数，用来平滑限速。 |
| `--retries` | HTTP/网络失败重试次数，默认 `7`。 |
| `--retry-backoff` | 指数退避基准秒数，默认 `1.0`。 |
| `--print-every` | 大任务进度打印频率。 |
| `--overwrite` | 已存在文件也重新下载/重建。 |
| `--include-quotes` | 当 `--data all` 时包含 quote。默认排除 quote，因为 quote 通常非常大。 |

## 3. `KEYS.txt` 结构

脚本只识别两组权限：

| 脚本内部组 | 覆盖资产 |
|---|---|
| `stocks_options` | `stocks`, `options` |
| `other` | `indices`, `futures`, `forex`, `crypto` |

`KEYS.txt` 需要包含 REST API key 和 Flat Files access/secret key。示意结构如下，不要把真实 key 写进文档或聊天里：

```text
REST API
Option / Stocks
<你的 stocks/options REST API key>

Index / Future / Currency / Crypto
<你的 indices/futures/forex/crypto REST API key>

FLATFILES
Option / Stocks
<你的 stocks/options flatfiles access key>
<你的 stocks/options flatfiles secret key>

Index / Future / Currency / Crypto
<你的 indices/futures/forex/crypto flatfiles access key>
<你的 indices/futures/forex/crypto flatfiles secret key>
```

脚本的解析逻辑比较宽松：

- 行里同时包含 `OPTION` 和 `STOCK`，会识别成 `stocks_options`；
- 行里包含 `INDEX`、`FUTUR` 或 `CUR`，会识别成 `other`；
- `REST API` 段下面每组读一行 key；
- `FLATFILES` 段下面每组读两行：第一行 access key，第二行 secret key。

## 4. REST 专用参数

| 参数 | 含义 |
|---|---|
| `--tickers AAPL,MSFT` | 显式指定 ticker。crypto/forex/indices 会自动补 `X:`、`C:`、`I:` 前缀。 |
| `--all-tickers` | 通过 REST reference endpoint 发现 ticker。可能比较慢。 |
| `--from-flatfiles` | 用本地 Flat File 的 ticker 列作为 REST ticker universe。需要先下载 Flat File。 |
| `--combined-output` | REST 输出合并成一个日期级 CSV。`--from-flatfiles` 会自动打开它。 |
| `--keep-parts` | 保留 REST combined 临时分片文件。 |
| `--max-tickers N` | 限制 ticker 数量，适合测试。 |
| `--max-pages N` | 限制 trades/quotes 翻页页数，只适合测试，不适合全量。 |
| `--allow-options-from-flatfiles` | 允许用 options Flat File 里的所有合约 ticker 拉 REST。非常大，默认保护性跳过。 |

## 5. 支持的资产类别与数据类型

### 5.1 Flat Files catalog

| 资产 | 远端 prefix | 本地目录类别 | 支持数据 |
|---|---|---|---|
| stocks | `us_stocks_sip` | `stocks` | `day_aggs_v1`, `minute_aggs_v1`, `trades_v1`, `quotes_v1` |
| options | `us_options_opra` | `options` | `day_aggs_v1`, `minute_aggs_v1`, `trades_v1`, `quotes_v1` |
| indices | `us_indices` | `indices` | `day_aggs_v1`, `minute_aggs_v1` |
| futures CME | `us_futures_cme` | `futures/cme` | `session_aggs_v1`, `minute_aggs_v1`, `trades_v1`, `quotes_v1` |
| futures CBOT | `us_futures_cbot` | `futures/cbot` | `session_aggs_v1`, `minute_aggs_v1`, `trades_v1`, `quotes_v1` |
| futures COMEX | `us_futures_comex` | `futures/comex` | `session_aggs_v1`, `minute_aggs_v1`, `trades_v1`, `quotes_v1` |
| futures NYMEX | `us_futures_nymex` | `futures/nymex` | `session_aggs_v1`, `minute_aggs_v1`, `trades_v1`, `quotes_v1` |
| forex | `global_forex` | `forex` | `day_aggs_v1`, `minute_aggs_v1`, `quotes_v1` |
| crypto | `global_crypto` | `crypto` | `day_aggs_v1`, `minute_aggs_v1`, `trades_v1` |

当 `--data all` 时，脚本默认排除所有 `quotes_v1`。要拉 quote 必须显式：

```powershell
--data quotes
```

或者：

```powershell
--data all --include-quotes
```

### 5.2 REST catalog：`--data all` 的实际行为

| 资产 | `--data all` 实际包含 | REST endpoint |
|---|---|---|
| stocks | `day`, `minute`, `trades` | day/minute 用 `/v2/aggs...`，trades 用 `/v3/trades...` |
| options | `day`, `minute`, `trades` | day/minute 用 `/v2/aggs...`，trades 用 `/v3/trades...` |
| indices | `day`, `minute` | `/v2/aggs...` |
| futures | `day`, `minute`, `trades` | aggregates 用 `/futures/v1/aggs...`，trades 用 `/futures/v1/trades...` |
| forex | `day`, `minute` | `/v2/aggs...` |
| crypto | `day`, `minute`, `trades` | day/minute 用 `/v2/aggs...`，trades 用 `/v3/trades...` |

补充：

- REST futures 的 `day` 在脚本里对应 `resolution=1session`，输出目录叫 `session_aggs`。
- REST futures quotes 没有实现；如果请求 futures quotes，脚本会报错。
- 非 futures 资产如果显式传 `--data quotes`，脚本会调用 `/v3/quotes/{ticker}`；是否有结果取决于数据商接口和你的权限。

## 6. REST endpoint 模板

### 6.1 Reference ticker 查询

stocks/options/indices/forex/crypto：

```text
GET /v3/reference/tickers
params:
  market={stocks|options|indices|fx|crypto}
  active=true
  date=YYYY-MM-DD
  limit=100
  sort=ticker
  order=asc
```

futures：

```text
GET /futures/v1/contracts
params:
  date=YYYY-MM-DD
  active=true
  limit=100
  sort=ticker.asc
```

### 6.2 Stocks grouped daily

当 REST 拉 stocks day，并且没有传 `--tickers` / `--all-tickers` 时，脚本会使用 grouped endpoint，一次拉全市场日线：

```text
GET /v2/aggs/grouped/locale/us/market/stocks/YYYY-MM-DD
params:
  adjusted=true
```

### 6.3 通用 aggregate：stocks/options/indices/forex/crypto

```text
GET /v2/aggs/ticker/{ticker}/range/1/{day|minute}/YYYY-MM-DD/YYYY-MM-DD
params:
  adjusted=true
  sort=asc
  limit=50000
```

### 6.4 通用 trades/quotes：非 futures

```text
GET /v3/trades/{ticker}
GET /v3/quotes/{ticker}
params:
  timestamp=YYYY-MM-DD
  sort=timestamp
  order=asc
  limit=50000
```

这些接口会翻页读取 `next_url`，直到没有下一页，或者达到 `--max-pages`。

### 6.5 Futures aggregates

```text
GET /futures/v1/aggs/{ticker}
params:
  resolution=1session 或 1min
  window_start.gte=YYYY-MM-DD
  window_start.lt=下一天 YYYY-MM-DD
  sort=window_start.asc
  limit=50000
```

### 6.6 Futures trades

```text
GET /futures/v1/trades/{ticker}
params:
  session_end_date=YYYY-MM-DD
  limit=50000
```

## 7. 输出目录结构

### 7.1 Flat Files

普通资产：

```text
downloads/unified/flatfiles/{asset}/{data_type}/YYYY/MM/YYYY-MM-DD.csv
```

例子：

```text
downloads/unified/flatfiles/stocks/minute_aggs_v1/2026/06/2026-06-18.csv
downloads/unified/flatfiles/crypto/trades_v1/2026/06/2026-06-18.csv
```

futures 多一层交易所/市场分组：

```text
downloads/unified/flatfiles/futures/{cme|cbot|comex|nymex}/{data_type}/YYYY/MM/YYYY-MM-DD.csv
```

例子：

```text
downloads/unified/flatfiles/futures/cme/session_aggs_v1/2026/06/2026-06-18.csv
```

### 7.2 REST per-ticker

```text
downloads/unified/rest/{asset}/{data_type}/YYYY/MM/YYYY-MM-DD_{ticker}.csv
```

例子：

```text
downloads/unified/rest/crypto/minute_aggs/2026/06/2026-06-18_X_BTCUSD.csv
```

### 7.3 REST combined

```text
downloads/unified/rest_combined/{asset}/{data_label}/YYYY/MM/YYYY-MM-DD.csv
```

例子：

```text
downloads/unified/rest_combined/stocks/day_aggs/2026/06/2026-06-18.csv
downloads/unified/rest_combined/futures/session_aggs/2026/06/2026-06-18.csv
```

临时 per-ticker part：

```text
downloads/unified/_parts/rest_combined/{asset}/{data_label}/YYYY/MM/YYYY-MM-DD/{ticker}.csv
```

默认合并完成后会删除 part；加 `--keep-parts` 可以保留。

## 8. REST ticker universe 如何确定

REST 需要知道要拉哪些 ticker。脚本按下面优先级确定 ticker 列表：

1. `--tickers`：用户显式指定；
2. `--from-flatfiles`：从本地 Flat File 的 `ticker` 列读取；
3. `--all-tickers`：调用 REST reference endpoint 查询；
4. 默认样例 ticker。

默认样例 ticker：

| 资产 | 默认 ticker |
|---|---|
| stocks | `AAPL`, `MSFT`, `SPY`, `QQQ` |
| options | `O:A260618C00120000`, `O:A260618P00125000`, `O:A260717C00130000` |
| indices | `I:SPX`, `I:NDX`, `I:DJI`, `I:RUT` |
| futures | `ESM6`, `NQM6`, `CLN6`, `GCQ6` |
| forex | `C:EURUSD`, `C:USDJPY`, `C:GBPUSD`, `C:AUDUSD`, `C:USDCAD`, `C:USDCHF`, `C:NZDUSD`, `C:EURJPY`, `C:USDCNY`, `C:USDCNH` |
| crypto | `X:BTCUSD`, `X:ETHUSD`, `X:SOLUSD`, `X:XRPUSD`, `X:DOGEUSD`, `X:ADAUSD`, `X:BNBUSD`, `X:AVAXUSD`, `X:LINKUSD`, `X:LTCUSD` |

`--from-flatfiles` 的 ticker 来源：

| 资产 | 优先 ticker 来源 |
|---|---|
| stocks/options/indices/forex/crypto | 优先读取本地 `day_aggs_v1` 当天文件的 `ticker` 列 |
| futures | 优先读取四个 venue 的 `session_aggs_v1`，没有则回退到 `minute_aggs_v1` |

## 9. 字段说明：通用概念

### 9.1 时间字段

| 字段 | 含义 |
|---|---|
| `window_start` | 聚合窗口开始时间。Flat File 里通常是纳秒 epoch；REST `/v2/aggs` 返回的 `t` 是毫秒 epoch，脚本在 combined 模式里会改名为 `window_start`。做 Flat File vs REST 对比时要特别注意单位。 |
| `timestamp` | 成交或 futures quote/trade 的事件时间，通常是纳秒 epoch。 |
| `participant_timestamp` | 交易所/参与方本地时间戳，通常是纳秒 epoch。 |
| `sip_timestamp` | SIP 收到并发布的时间戳，通常是纳秒 epoch。 |
| `trf_timestamp` | TRF 收到时间戳，暗池/OTC 场外成交可能有值。 |
| `ask_timestamp` / `bid_timestamp` | futures quote 的 ask/bid 时间戳，通常是纳秒 epoch。 |
| `session_end_date` | futures 归属交易 session 的结束日期。不是简单自然日。 |

### 9.2 聚合行情字段

| 字段 | 含义 |
|---|---|
| `ticker` | 标的代码。crypto 常见 `X:BTCUSD`，forex 常见 `C:EURUSD`，indices 常见 `I:SPX`，options 常见 `O:...`。 |
| `open` / `high` / `low` / `close` | 周期内 OHLC。 |
| `volume` | 周期内成交量。不同市场单位不同：股票/期权通常是股数/张数，crypto/forex 取决于数据商定义。 |
| `transactions` | 周期内成交笔数或参与聚合的事件数。 |
| `volume_weighted_average_price` | VWAP。REST combined 会使用这个长字段名。 |
| `vw` | REST 原始聚合字段里的 VWAP。 |
| `v` / `o` / `h` / `l` / `c` / `t` / `n` | REST 原始简写字段：volume/open/high/low/close/timestamp/transactions。 |
| `dollar_volume` | futures 聚合里的名义成交额。 |
| `settlement_price` | futures session 结算价。 |
| `resolution` | REST futures aggregate 输出，`1session` 或 `1min`。 |
| `exchange` | futures minute aggregate 中的交易所/venue ID。 |

### 9.3 成交 trades 字段

| 字段 | 含义 |
|---|---|
| `ticker` | 标的代码。 |
| `price` | 成交价。 |
| `size` | 成交数量。股票可出现碎股小数。 |
| `conditions` | 成交条件码，可能是逗号分隔或数组式字段。 |
| `correction` | 修正标志，例如正常/撤销/更正。具体编码以数据商文档为准。 |
| `exchange` | 交易所或 venue ID。 |
| `id` | 数据源内部成交 ID。并非所有资产都有。 |
| `sequence_number` | 序号。股票里常用于 SIP 排序；futures 里也有交易序号。 |
| `report_sequence` | futures 报告序号。 |
| `channel` | futures REST trades 可能返回的数据通道字段。 |
| `tape` | 美股 tape：常见 1/2/3。 |
| `trf_id` | TRF 机构 ID，场外/暗池成交可能出现。 |

### 9.4 报价 quotes 字段

| 字段 | 含义 |
|---|---|
| `ask_exchange` / `bid_exchange` | 卖一/买一所在交易所或报价来源。 |
| `ask_price` / `bid_price` | 卖一价/买一价。 |
| `ask_size` / `bid_size` | 卖一量/买一量。 |
| `conditions` | quote 条件码。 |
| `indicators` | quote 指示码/状态码。 |
| `sequence_number` | quote 序号。 |
| `timestamp` | futures quote 的主时间戳。 |
| `ask_timestamp` / `bid_timestamp` | futures ask/bid 侧时间戳。 |
| `participant_timestamp` | forex/stock quote 中可能出现的参与方时间戳。 |
| `sip_timestamp` | SIP quote 时间戳。 |
| `tape` | 美股 tape。 |
| `trf_timestamp` | TRF quote 时间戳，部分场景有值。 |

## 10. Flat File 字段表头

下面是当前脚本能拉、并按当前项目样例/远端表头确认过的 Flat File CSV 字段。

### 10.1 stocks

| 数据 | 字段 |
|---|---|
| `day_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `minute_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `trades_v1` | `ticker`, `conditions`, `correction`, `exchange`, `id`, `participant_timestamp`, `price`, `sequence_number`, `sip_timestamp`, `size`, `tape`, `trf_id`, `trf_timestamp` |
| `quotes_v1` | `ticker`, `ask_exchange`, `ask_price`, `ask_size`, `bid_exchange`, `bid_price`, `bid_size`, `conditions`, `indicators`, `participant_timestamp`, `sequence_number`, `sip_timestamp`, `tape`, `trf_timestamp` |

### 10.2 options

| 数据 | 字段 |
|---|---|
| `day_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `minute_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `trades_v1` | `ticker`, `conditions`, `correction`, `exchange`, `price`, `sip_timestamp`, `size` |
| `quotes_v1` | `ticker`, `ask_exchange`, `ask_price`, `ask_size`, `bid_exchange`, `bid_price`, `bid_size`, `sequence_number`, `sip_timestamp` |

### 10.3 indices

| 数据 | 字段 |
|---|---|
| `day_aggs_v1` | `ticker`, `open`, `close`, `high`, `low`, `window_start` |
| `minute_aggs_v1` | `ticker`, `open`, `close`, `high`, `low`, `window_start` |

### 10.4 futures

CME/CBOT/COMEX/NYMEX 的字段结构一致。

| 数据 | 字段 |
|---|---|
| `session_aggs_v1` | `ticker`, `session_end_date`, `window_start`, `open`, `high`, `low`, `close`, `volume`, `dollar_volume`, `transactions`, `settlement_price` |
| `minute_aggs_v1` | `ticker`, `exchange`, `session_end_date`, `window_start`, `open`, `high`, `low`, `close`, `volume`, `dollar_volume`, `transactions` |
| `trades_v1` | `ticker`, `timestamp`, `sequence_number`, `report_sequence`, `price`, `size`, `correction`, `exchange`, `session_end_date` |
| `quotes_v1` | `ticker`, `timestamp`, `sequence_number`, `report_sequence`, `ask_timestamp`, `ask_price`, `ask_size`, `bid_timestamp`, `bid_price`, `bid_size`, `exchange`, `session_end_date` |

### 10.5 forex

| 数据 | 字段 |
|---|---|
| `day_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `minute_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `quotes_v1` | `ticker`, `ask_exchange`, `ask_price`, `bid_exchange`, `bid_price`, `participant_timestamp` |

### 10.6 crypto

| 数据 | 字段 |
|---|---|
| `day_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `minute_aggs_v1` | `ticker`, `volume`, `open`, `close`, `high`, `low`, `window_start`, `transactions` |
| `trades_v1` | `ticker`, `conditions`, `exchange`, `id`, `participant_timestamp`, `price`, `size` |

## 11. REST 输出字段

### 11.1 REST per-ticker aggregate

当不使用 `--combined-output` 时，通用 aggregate 输出保留 REST 原始简写字段，并额外加 `ticker`：

| 数据 | 字段 |
|---|---|
| `day_aggs` / `minute_aggs` | `ticker`, `t`, `o`, `h`, `l`, `c`, `v`, `vw`, `n` |

字段含义：

| 字段 | 含义 |
|---|---|
| `t` | window start，REST `/v2/aggs` 通常是毫秒 epoch。 |
| `o`, `h`, `l`, `c` | open/high/low/close。 |
| `v` | volume。 |
| `vw` | VWAP。 |
| `n` | transactions。 |

stocks grouped day 的 per-ticker 目录 `rest/stocks/grouped_day/...` 会保存原始 grouped 字段：

```text
T, o, h, l, c, v, vw, t, n
```

其中 `T` 是 ticker。

### 11.2 REST combined aggregate

combined 模式会把 aggregate 统一成更像 Flat File 的长字段名：

```text
ticker, volume, open, close, high, low, window_start, transactions, volume_weighted_average_price
```

注意：这里的 `window_start` 如果来自 `/v2/aggs`，通常是毫秒 epoch；Flat File 里的 `window_start` 通常是纳秒 epoch。

### 11.3 REST futures aggregate

REST futures aggregate 输出字段来自 `/futures/v1/aggs/{ticker}`，脚本会额外加：

```text
ticker, resolution
```

常见字段：

```text
ticker, resolution, window_start, session_end_date, open, high, low, close, volume, transactions, dollar_volume, settlement_price
```

其中：

- `resolution=1session` 对应脚本里的 `day`；
- `resolution=1min` 对应脚本里的 `minute`。

### 11.4 REST trades/quotes

REST trades/quotes 不做强字段映射：脚本会把 API 返回的 JSON 字段展开后写入 CSV，并强制第一列是 `ticker`。

非 futures trades endpoint：

```text
/v3/trades/{ticker}
```

常见字段接近：

```text
ticker, conditions, correction, exchange, id, participant_timestamp, price, sequence_number, sip_timestamp, size, tape, trf_id, trf_timestamp
```

非 futures quotes endpoint：

```text
/v3/quotes/{ticker}
```

常见字段接近：

```text
ticker, ask_exchange, ask_price, ask_size, bid_exchange, bid_price, bid_size, conditions, indicators, participant_timestamp, sequence_number, sip_timestamp, tape, trf_timestamp
```

futures trades endpoint：

```text
/futures/v1/trades/{ticker}
```

常见字段：

```text
ticker, timestamp, sequence_number, report_sequence, channel, session_end_date, price, size
```

脚本当前不支持 REST futures quotes。

## 12. 典型用法

### 12.1 查看支持范围

```powershell
python massive_data_downloader.py catalog
```

### 12.2 下载某一天所有 Flat Files，默认排除 quotes

```powershell
python massive_data_downloader.py flatfiles `
  --asset all `
  --data all `
  --date 2026-06-18 `
  --workers 8
```

### 12.3 下载 crypto 2023-2026 分钟线 Flat File

```powershell
python massive_data_downloader.py flatfiles `
  --asset crypto `
  --data minute `
  --start-date 2023-01-01 `
  --end-date 2026-06-24 `
  --workers 8
```

### 12.4 用本地 Flat File ticker universe 拉 REST，并合并成一个文件

```powershell
python massive_data_downloader.py rest `
  --asset stocks `
  --data minute `
  --date 2026-06-18 `
  --from-flatfiles `
  --workers 12 `
  --request-delay 0.03
```

输出：

```text
downloads/unified/rest_combined/stocks/minute_aggs/2026/06/2026-06-18.csv
```

### 12.5 REST 小样本调试

```powershell
python massive_data_downloader.py rest `
  --asset crypto `
  --data day,minute,trades `
  --tickers X:BTCUSD,X:ETHUSD `
  --date 2026-06-18 `
  --max-pages 1
```

### 12.6 显式下载 quote

quote 通常非常大，建议先单日、小样本测试。

Flat File：

```powershell
python massive_data_downloader.py flatfiles `
  --asset stocks `
  --data quotes `
  --date 2026-06-18 `
  --workers 2
```

REST：

```powershell
python massive_data_downloader.py rest `
  --asset stocks `
  --data quotes `
  --tickers AAPL,MSFT `
  --date 2026-06-18 `
  --max-pages 1
```

## 13. 实务建议

1. 历史全量研究优先 Flat Files，不要用 REST 一只只 ticker 拉全市场历史。
2. REST 更适合最新数据、小范围查询、补洞、验证、reference ticker、snapshot/chain 这类接口。
3. `trades` 已经很大，`quotes` 通常更大；`--data all` 默认排除 quote 是合理的。
4. 对比 Flat File 和 REST aggregate 时，先统一时间单位：Flat File 多为纳秒，REST `/v2/aggs` 多为毫秒。
5. options REST 全量特别危险：用 Flat File 的期权合约 ticker universe 可能产生几十万甚至更多请求。脚本默认需要 `--allow-options-from-flatfiles` 才会放行。
6. futures 的日期逻辑要注意 `session_end_date`，不要简单按自然日理解。

