# Massive downloader · Massive 数据下载器

[Repository / 全仓](../../README.md) · [US equities / 美股](../../us_equity/README.md) ·
[Setup / 环境](../../docs/SETUP.md)

A shared local archive downloader for Massive/Polygon data. It supports bulk S3 Flat Files and
small-scope REST retrieval across stocks, options, indices, futures, FX and crypto. Actual dataset
availability depends on the provider and subscription. This directory contains ingestion, not strategies.

共享的 Massive/Polygon 本地归档下载器，支持 S3 Flat Files 批量下载和小范围 REST 获取，
覆盖股票、期权、指数、期货、外汇和虚拟币；具体数据可用性取决于服务端与订阅权限。
本目录只负责数据接入，不放策略。

## Install and inspect · 安装与查看

From the repository root / 在仓库根执行：

```bash
conda activate mx
python -m pip install -e ".[massive]"
python data_sources/massive/massive_data_downloader.py catalog
python data_sources/massive/massive_data_downloader.py flatfiles --help
python data_sources/massive/massive_data_downloader.py rest --help
```

The CLI is unchanged from the former `massive_down` directory. `catalog` describes script capabilities,
not guaranteed account access; some generic REST trades/quotes combinations are provider-dependent.

CLI 与原 `massive_down` 不变。`catalog` 描述脚本能力，不保证账户权限；
部分通用 REST trades/quotes 组合仍取决于服务端支持。

## Credentials · 密钥

Keep credentials in the ignored `data_sources/massive/KEYS.txt`. Always pass its path explicitly when
running from the repository root; the CLI's bare default `KEYS.txt` is relative to the current directory.
Never commit this file. The parser recognizes two entitlement groups:

密钥放在被忽略的 `data_sources/massive/KEYS.txt`。从仓库根调用时显式传 `--keys`；
CLI 默认的裸文件名 `KEYS.txt` 相对于当前工作目录。不要提交密钥。解析器识别两组权限：

```text
REST API
Option / Stocks
<stocks/options REST API key>

Index / Future / Currency / Crypto
<other-assets REST API key>

FLATFILES
Option / Stocks
<stocks/options access key>
<stocks/options secret key>

Index / Future / Currency / Crypto
<other-assets access key>
<other-assets secret key>
```

## US minute starting point · 美股分钟起点

```bash
python data_sources/massive/massive_data_downloader.py flatfiles \
  --keys data_sources/massive/KEYS.txt \
  --asset stocks --data minute \
  --start-date 2026-06-15 --end-date 2026-06-18 \
  --output-dir data/massive/unified --workers 4
python -m us_equity run
```

This matches the default US experiment and input path. The default downloader output directory is
still `downloads/unified` relative to the working directory; the explicit argument above is the
repository convention. Flat Files are downloaded as gzip then extracted; `--keep-gz` retains both.

这与默认美股实验及输入路径一致。下载器自身默认输出仍是当前目录下的 `downloads/unified`，
上面的显式参数才是本仓库约定。Flat Files 下载 gzip 后解压；`--keep-gz` 会同时保留两份。

The US reader rejects duplicate CSV/gzip copies in one input tree. Its default workflow uses only
the extracted CSV. `--data all` excludes quotes unless `--include-quotes` is given; quote datasets
can be large, so request them explicitly for a defined research scope.

美股读取器会拒绝同一输入目录内重复的 CSV/gzip 数据，默认流程只保留解压 CSV。
`--data all` 默认排除 quotes，除非加 `--include-quotes`；报价数据可能很大，应按明确研究范围请求。

## REST samples · REST 小样本

```bash
python data_sources/massive/massive_data_downloader.py rest \
  --keys data_sources/massive/KEYS.txt \
  --asset stocks --data minute --tickers AAPL,MSFT,SPY \
  --date 2026-06-18 --output-dir data/massive/unified
```

REST outputs one file per ticker by default. `--combined-output` merges per-date outputs;
`--from-flatfiles` discovers tickers from local Flat Files and enables combined mode.
`--max-pages` and `--max-tickers` truncate data and are intended for tests.

REST 默认每个 ticker 一个文件；`--combined-output` 合并为按日输出；
`--from-flatfiles` 从本地 Flat File 发现 ticker 并启用 combined 模式。
`--max-pages` 和 `--max-tickers` 会截断数据，只适合测试。

## Source boundaries · 来源口径

| Source / 来源 | Minute timestamp / 分钟时间 | Stock price basis / 股票价格口径 |
|---|---|---|
| Flat Files | `window_start`, nanoseconds / 纳秒 | Unadjusted / 未复权 |
| Current REST downloader | `t` or renamed `window_start`, milliseconds / 毫秒 | `adjusted=true`, split-adjusted / 拆股复权 |

Do not combine these sources without reconciling units and adjustment basis. The downloader does not
perform research normalization. The [US reader](../../us_equity/README.md) has separate source profiles.
Stock trades/quotes are not a full order-level book and cannot reconstruct L3 queue positions.

单位和复权口径未统一前，不要拼接两种来源。下载器不负责研究清洗；美股读取器有独立来源配置。
股票 trades/quotes 不是完整逐订单簿，不能据此重建 L3 排队位置。

The [detailed reference](MASSIVE_DATA_DOWNLOADER_REFERENCE.md) retains the original Chinese parameter
and field documentation, with an English orientation at the top. Downloaded data, account credentials
and failed-download logs belong outside version control.

[详细参考](MASSIVE_DATA_DOWNLOADER_REFERENCE.md)保留原中文参数和字段说明，顶部增加英文导读。
下载数据、密钥与失败日志不进入版本库。
