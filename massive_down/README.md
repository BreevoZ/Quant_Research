# Massive Data Downloader

一个用于下载 Massive/Polygon 市场数据的本地归档工具。

核心脚本：

```text
massive_data_downloader.py
```

详细接口和字段说明见：

```text
MASSIVE_DATA_DOWNLOADER_REFERENCE.md
```

## 1. 这个项目能做什么

这个项目主要解决两类问题：

1. 批量下载历史数据  
   使用 Flat Files，适合全市场、多年份、回测和本地数据仓库。

2. 小范围拉取 REST 数据  
   使用 REST API，适合拉某几个 ticker、补洞、验证、查近期/当天数据。

目前脚本支持：

| 资产类别 | Flat Files | REST |
|---|---:|---:|
| US Stocks | 支持 | 支持 |
| US Options | 支持 | 支持 |
| US Indices | 支持 | 支持 |
| Futures | 支持 | 支持 |
| Forex | 支持 | 支持 |
| Crypto | 支持 | 支持 |

支持的数据类型包括：

- `day`
- `minute`
- `session`，主要用于 futures Flat Files
- `trades`
- `quotes`

注意：`quotes` 通常非常大，脚本在 `--data all` 时默认排除 quote。需要 quote 时要显式传 `--data quotes` 或 `--include-quotes`。

## 2. 环境要求

建议使用 Python 3.10+。当前开发环境使用的是 Python 3.12。

安装依赖：

```powershell
pip install -r requirements.txt
```

如果 Windows 上 `python` 命令不可用，可以使用你的 Python 绝对路径，例如：

```powershell
& "C:\Users\MX\AppData\Local\Programs\Python\Python312\python.exe" -m pip install -r requirements.txt
```

## 3. 密钥文件

脚本默认读取当前目录下的：

```text
KEYS.txt
```

当前项目里可以保留真实密钥，但如果以后上传 GitHub 或发给不可信的人，建议先替换成模板或单独私下发送。

脚本识别两组权限：

| 分组 | 覆盖资产 |
|---|---|
| Option / Stocks | stocks, options |
| Index / Future / Currency / Crypto | indices, futures, forex, crypto |

`KEYS.txt` 的结构大致如下：

```text
REST API
Option / Stocks
<stocks/options REST API key>

Index / Future / Currency / Crypto
<indices/futures/forex/crypto REST API key>

FLATFILES
Option / Stocks
<stocks/options flatfiles access key>
<stocks/options flatfiles secret key>

Index / Future / Currency / Crypto
<indices/futures/forex/crypto flatfiles access key>
<indices/futures/forex/crypto flatfiles secret key>
```

## 4. 快速开始

先看脚本支持哪些类别：

```powershell
python massive_data_downloader.py catalog
```

下载某一天所有 Flat Files，默认排除 quote：

```powershell
python massive_data_downloader.py flatfiles `
  --asset all `
  --data all `
  --date 2026-06-18 `
  --workers 8
```

下载某个日期区间的 crypto 分钟线：

```powershell
python massive_data_downloader.py flatfiles `
  --asset crypto `
  --data minute `
  --start-date 2023-01-01 `
  --end-date 2026-06-24 `
  --workers 8
```

用 REST 拉几个 ticker 的分钟线：

```powershell
python massive_data_downloader.py rest `
  --asset stocks `
  --data minute `
  --tickers AAPL,MSFT,SPY `
  --date 2026-06-18
```

先从本地 Flat File 读取 ticker universe，再用 REST 拉数据并合并：

```powershell
python massive_data_downloader.py rest `
  --asset stocks `
  --data minute `
  --date 2026-06-18 `
  --from-flatfiles `
  --workers 12 `
  --request-delay 0.03
```

## 5. 推荐工作流

### 5.1 历史研究 / 回测

推荐：

```text
Flat Files -> 本地 CSV -> 清洗/转换 -> 回测/分析
```

例子：

```powershell
python massive_data_downloader.py flatfiles `
  --asset stocks `
  --data minute `
  --start-date 2023-01-01 `
  --end-date 2026-06-24 `
  --workers 8
```

历史全量不要优先用 REST 一只只 ticker 拉。那样请求量大、速度慢，还容易触发限速。

### 5.2 当天或近期小范围数据

推荐 REST：

```powershell
python massive_data_downloader.py rest `
  --asset crypto `
  --data day,minute,trades `
  --tickers X:BTCUSD,X:ETHUSD `
  --date 2026-06-18
```

REST 适合：

- 拉几个 ticker；
- 检查当天/近期数据；
- 补某一天某个 ticker 的洞；
- 和 Flat File 做交叉验证。

### 5.3 每日增量

每天盘后或第二天拉新增 Flat File：

```powershell
python massive_data_downloader.py flatfiles `
  --asset all `
  --data day,minute,trades `
  --date 2026-06-18 `
  --workers 8
```

如果要 quote：

```powershell
python massive_data_downloader.py flatfiles `
  --asset stocks `
  --data quotes `
  --date 2026-06-18 `
  --workers 2
```

quote 文件可能非常大，建议单独跑。

## 6. 输出目录结构

默认输出根目录：

```text
downloads/unified
```

Flat Files：

```text
downloads/unified/flatfiles/{asset}/{data_type}/YYYY/MM/YYYY-MM-DD.csv
```

Futures Flat Files 多一层交易所：

```text
downloads/unified/flatfiles/futures/{cme|cbot|comex|nymex}/{data_type}/YYYY/MM/YYYY-MM-DD.csv
```

REST per-ticker：

```text
downloads/unified/rest/{asset}/{data_type}/YYYY/MM/YYYY-MM-DD_{ticker}.csv
```

REST combined：

```text
downloads/unified/rest_combined/{asset}/{data_label}/YYYY/MM/YYYY-MM-DD.csv
```

## 7. 常用参数速查

| 参数 | 说明 |
|---|---|
| `--asset` | 资产类别，支持 `stocks,options,indices,futures,forex,crypto,all` |
| `--exclude-assets` | 排除资产，例如 `--exclude-assets options` |
| `--data` | 数据类型，支持 `day,minute,session,trades,quotes,all` |
| `--date` | 单日下载 |
| `--start-date` / `--end-date` | 日期区间下载 |
| `--workers` | 并发数 |
| `--overwrite` | 覆盖已有文件 |
| `--include-quotes` | `--data all` 时包含 quote |
| `--tickers` | REST 显式指定 ticker |
| `--from-flatfiles` | REST 从本地 Flat File 读取 ticker universe |
| `--combined-output` | REST 合并输出成单个日期文件 |
| `--max-tickers` | 限制 ticker 数，用于测试 |
| `--max-pages` | 限制 trades/quotes 翻页数，用于测试 |

## 8. 注意事项

1. Flat File 是历史全量下载的主力，REST 是辅助。
2. `quotes` 数据量通常比 `trades` 更夸张，最好单独跑。
3. REST full market minute/trades 不推荐长期使用；如果目标是历史全量，请等 Flat File。
4. Options REST 全量非常大，脚本默认不允许直接用所有期权合约 ticker 拉，除非加 `--allow-options-from-flatfiles`。
5. Flat File 的 `window_start` 多数是纳秒 epoch；REST aggregate 的时间戳多数是毫秒 epoch。对比两种数据时要先统一时间单位。
6. Futures 要注意 `session_end_date`，不要简单按自然日理解。

## 9. 项目文件

```text
Massive_down/
  KEYS.txt
  massive_data_downloader.py
  MASSIVE_DATA_DOWNLOADER_REFERENCE.md
  README.md
  requirements.txt
```

其中：

- `massive_data_downloader.py`：主下载脚本；
- `MASSIVE_DATA_DOWNLOADER_REFERENCE.md`：完整接口和字段说明；
- `README.md`：项目入口说明；
- `requirements.txt`：Python 依赖；
- `KEYS.txt`：密钥文件。


