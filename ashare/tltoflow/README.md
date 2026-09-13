# tltoflow — 通联 TL 原始数据 → Flow 统一格式转换器

[A-share overview / A 股入口](../README.md) · [Repository / 全仓](../../README.md) · [Setup / 环境](../../docs/SETUP.md)

## English quick start

Convert TL source records to unified Flow CSV. Exchange-specific normalization belongs here.

Commands below run from the repository root, using conda `mx` for Python:

```bash
cmake -S ashare/tltoflow -B ashare/tltoflow/build
cmake --build ashare/tltoflow/build -j4
```

See the [A-share guide](../README.md) for data contracts, paths and research limitations.
The detailed Chinese implementation reference follows. Its component-local commands assume
`cd ashare/tltoflow` unless they explicitly start from the repository root.

## 中文技术说明

下文保留原项目技术细节；项目内相对命令以 `ashare/tltoflow` 为工作目录，仓库根命令另行标明。

把通联(TongLian)TLData 的原始逐笔 CSV 转成 **Flow 统一格式** CSV,并同步产出 Instruments XML。
单文件 C++ 实现(`tl_to_flow_csv.cpp`, 1043 行),无第三方依赖。

## 它在整条链路里的位置

```
TLData 原始逐笔 ──┐
                  ├─► tltoflow (本项目) ──► Flow 格式 CSV ──┬─► datacheck_final  (LOB 重建 + 双源比对)
其他原始数据源 ───┘                                          ├─► vorder_sim       (虚拟订单模拟)
                                                             ├─► build_tick3s     (3 秒降频)
                                                             └─► l3_factor        (L3 因子)
```

**Flow 不是一个独立数据源,而是一套统一格式**:不同原始数据源(TLData 是其中之一)
经各自的轻量转换脚本落到同一套字段结构上,下游的 LOB 重建/跨源比对就可以用同一套逻辑处理,
不必为每个数据源单独写一遍委托/撤单/成交处理。**本项目就是 TLData 那一支转换脚本。**

> 这条"各源转 Flow → 共用一套 Flow LOB Builder"的路径,比"为每个数据源单写一套 Builder"
> 更具扩展性,是更值得长期投入的方案。后者(如 `datacheck_final` 的 TL 直读路径)主要作为
> **对照与验证手段**——用来确认"原始数据 → Flow → LOB"这条主链路本身可信。
> 两条路径的产出理论上应完全一致,数据源对比脚本本质上就是在验证这一点。

## 输出字段

```
tradeDirection, priceType, bidId, askId, price, volume, turnover, appSeq, ...
```

排序键默认 `appseq` = `channel * 1e12 + rawSeq`。

---

## 用法

```bash
# 最简: 单日
./build/tl_to_flow_csv -d 20260611

# 生产实际用法
./build/tl_to_flow_csv -d 20260611 \
    --sort-key     appseq \
    --sort-tmp-dir /tmp \
    --missing-policy fail \
    --number-format  compact \
    --sh-turnover        raw \
    --sh-trade-dir-mode  zero \
    --sz-trade-turnover  raw \
    --sz-cancel-turnover raw \
    --sz-ordtype-map 49:1,85:U,default:2 \
    --sh-price-type  0
```

输入输出根目录不写死:默认取 `qr.toml` 的 `tl_zip` 与 `flow_root`,可用 `QR_TL_ZIP` /
`QR_FLOW_ROOT` 覆盖,也可以用 `--tl-root` / `--output-root` 显式指定。
`./build/tl_to_flow_csv --help` 会打印出当前实际生效的默认值。

### 批量:`run.sh`

```bash
./run.sh 20260611                 # 单日
./run.sh 20260601 20260630        # 日期区间
```

环境变量可覆盖:`BIN` / `TL_ROOT` / `OUTPUT_ROOT` / `SORT_TMP_DIR` / `SYMBOL_PREFIX_FILTER`
(默认 `SH:6,SZ:0|3`)。日期之后的额外参数会**透传**给 `tl_to_flow_csv`。

### 全部选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `-d, --date DATE` | — | 交易日期(必需) |
| `--input-dir DIR` / `--tl-root DIR` | `qr.toml` 的 `tl_zip` | 输入目录 / 根目录 |
| `--output-file FILE` / `--output-root DIR` | `qr.toml` 的 `flow_root` | 输出文件 / 根目录 |
| `-m, --market SH\|SZ` | 全部 | 市场过滤 |
| `-s, --symbol SYMBOL` | 全部 | 代码过滤,如 `600396.SH` |
| `--symbol-prefix-filter FILTER` | — | 前缀过滤,如 `SH:6,SZ:0\|3` |
| `--start-time` / `--end-time HH:MM:SS.mmm` | 全天 | 交易所时间闭区间 |
| `--sort-key appseq\|time` | `appseq` | 输出排序键 |
| `--sort-tmp-dir DIR` | — | `sort -T` 用的临时目录 |
| `--keep-temp` | 否 | 保留 `<output>.unsorted.tmp` |
| `--missing-policy fail\|skip` | `fail` | 输入缺失时的处理 |
| `--number-format compact\|fixed6\|raw` | `compact` | 价格/成交额文本格式 |
| `--sz-ordtype-map MAP` | `49:1,85:U,default:2` | SZ OrdType → priceType 映射 |
| `--sh-price-type VALUE` | `0` | SH 输出的 priceType |
| `--sh-turnover raw\|pxqty\|zero` | `raw` | SH 成交额口径 |
| `--sh-trade-dir-mode zero\|side` | `zero` | **SH 成交方向口径(见下方口径说明)** |
| `--sz-trade-turnover raw\|pxqty\|zero` | `raw` | SZ 成交额口径 |
| `--sz-cancel-turnover raw\|pxqty\|zero` | `raw` | SZ 撤单额口径 |
| `--output-instruments-xml PATH` | `<output-root>/<DATE>.xml` | Instruments XML 输出路径 |
| `--instruments-template FILE` | — | 模板,保留除 `InstrumentID`/`ExchangeID` 外的全部属性 |

### Instruments XML

CSV 生成后,转换器会按**实际输出的标的集合**写一份 Instruments XML。
用 `--instruments-template ./Instruments.xml` 可保留模板中的其余属性。

---

## ⚠️ 字段口径说明:SH 集合竞价成交的方向标志

上海市场**集合竞价撮合的成交,在原始数据里主动方向标志为 "N"(中性,表示无主动方)**。

本转换器忠实地把这类成交标为**"未知"**(`--sh-trade-dir-mode zero`,默认)。

这一点曾与 `vorder_sim` 的 TLData 解析器不一致——后者用的是**二值规则**,把所有非买方主动的
成交都归为卖方主动,等于**给这些本无主动方的竞价成交编造了方向**。虽然这些成交全部落在
集合竞价窗口、绝大多数情况下不影响最终结果,但已构造用例证明它确实会在特定场景下改变成交归因
(一笔挂在竞价清算价、于连续段插入的订单,其前方队列估计会读到这些竞价成交)。

**处理结果**:已把 `vorder_sim` 的 TLData 解析器改为与本转换器一致的**三路映射(买/卖/未知)**,
两源结果随即完全统一。**本项目的口径是基准,改动前请确认下游是否同步。**

> 更一般的教训:重建结果一致并不代表底层字段定义一致,必须回到原始事件逐字段确认。

---

## 构建

```bash
cmake -S . -B build
cmake --build build -j4
```

无需第三方库；CMake 自动接入共享 `qr/` 头文件。产物为 `build/tl_to_flow_csv`，与 `run.sh` 默认路径一致。

## 文件

| 文件 | 说明 |
|---|---|
| `tl_to_flow_csv.cpp` | 全部实现(1043 行) |
| `run.sh` | 日期区间批量封装,支持环境变量覆盖与参数透传 |
| `Instruments.xml` | XML 模板(1.9M),供 `--instruments-template` 使用 |
| `tl_to_flow_csv` | 编译产物(与源码同目录;`run.sh` 默认 `BIN=$SCRIPT_DIR/tl_to_flow_csv`) |
| `CMakeLists.txt` | 备用构建 |
