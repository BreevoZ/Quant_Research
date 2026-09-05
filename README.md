# Quant Research

A 股高频数据基础设施与 L3 订单流因子研究。两条链路：一条把交易所逐笔原始数据还原成可信的
盘口面板，一条在这份面板上做因子挖掘与策略回测。

```
数据链路   原始数据 ──► Flow 统一格式 ──► LOB 重建 ──► 3 秒降频 ──► clean panel
                        tltoflow         datacheck      build_tick3s
                                         auction(生产/竞价)
                                              │
                                              └──► vorder_sim(在历史盘口上做反事实成交推演)

研究链路   clean panel ──► factor / label ──► IC / ICIR ──► 候选 alpha ──► backtest
                                        factor(早期)  ·  l3_factor(主线)
```

## 项目

| 目录 | 语言 | 定位 |
|---|---|---|
| [`tltoflow/`](tltoflow/) | C++ | 通联 TL 原始数据转 Flow 统一格式。整条链路的上游 |
| [`datacheck/`](datacheck/) | C++ | LOB 重建加双源比对。`lob_builder` 的**基准实现** |
| [`auction/`](auction/) | C++ | 面向生产的集合竞价 LOB Builder，独有逐事件虚拟撮合快照 |
| [`build_tick3s/`](build_tick3s/) | C++ | 逐笔 LOB 降频到 3 秒切片，约 215 列 |
| [`vorder_sim/`](vorder_sim/) | C++ | 虚拟订单排队/成交模拟器，不改真实盘口做反事实推演 |
| [`l3_factor/`](l3_factor/) | Python | A 股 L3 订单流因子研究与策略回测**（主线）** |
| [`factor/`](factor/) | Python | 早期因子探索与评估框架验证，已被 `l3_factor` 取代 |
| [`massive_down/`](massive_down/) | Python | 海外市场数据拉取，美股/期权/期货/指数/外汇/虚拟币 |

延伸阅读：[`l3_factor/FACTOR_BOOK.md`](l3_factor/FACTOR_BOOK.md) 因子手册、
[`l3_factor/FEATURES.md`](l3_factor/FEATURES.md) 特征字典、
[`datacheck/沪深数据与规则差异.md`](datacheck/沪深数据与规则差异.md) 市场规则差异。

## 新机器上从零开始

以 WSL 为例，macOS 把第一步换成 `brew install hdf5 cmake` 即可。

```bash
# 1. 系统依赖
sudo apt update && sudo apt install -y build-essential cmake libhdf5-dev python3-venv

# 2. Python 环境。需要 3.11 以上，qr/paths.py 用标准库 tomllib 读配置
python3 -m venv .venv && source .venv/bin/activate
pip install -e ".[ml,explore,massive]"      # 只要核心依赖就 pip install -e .

# 3. 告诉代码数据在哪
cp qr.example.toml qr.toml                  # 然后改里面的路径

# 4. 构建 C++，并用自包含测试确认环境没问题
cmake -S vorder_sim -B vorder_sim/build && cmake --build vorder_sim/build -j8
ctest --test-dir vorder_sim/build --output-on-failure     # 应为 2/2 通过
```

第 4 步的 843 条断言不需要任何外部数据，是验证工具链的最快方式。通过了就说明
编译器、HDF5、CMake 都对。

数据盘挂在别处时不必改配置文件，环境变量优先级更高：

```bash
QR_DATA=/mnt/d/quant/l3data python l3_factor/runday.py 20260707
```

跑完这几步之后，任何脚本在任何工作目录下调用都一样，
`python l3_factor/factors/factor_id.py` 和先 cd 进去再跑等价。

## 改代码前必读的四条硬约定

1. **盘口快照是 pre-event 的。** 每行记录的是这笔事件**发生前**的盘口状态。`datacheck`、
   `auction`、`vorder_sim` 的输出都遵守这条，`build_tick3s` 依赖它。如果 builder 改成
   post-event，`build_tick3s` 会再 shift 一次，双重偏移且**静默不报错**。
2. **`lob_builder` 有三份实现。** `datacheck` 是基准，`vorder_sim` 是拷贝加只读扩展
   （`EventHook` 与 lite 模式），`auction` 是独立实现带移植注释。在一处改规则必须三处同步检查。
3. **SH 原始逐笔是「坏」的。** 委托量只记挂住量、约 41% 成交无委托记录、竞价成交方向标 N。
   做 SH 要用 `datacheck` 补全后的逐事件数据，不能用生数据。SZ 原始数据干净可直接用。
4. **SH 竞价成交方向是未知，不是二值。** 走三路映射，口径基准在 `tltoflow`。

## 构建

C++ 项目各自独立，都用 CMake：

```bash
cmake -S datacheck -B datacheck/build && cmake --build datacheck/build -j8
```

`build_tick3s` 另有 `Makefile` 与 `build.sh` 两套入口，日常开发用 `make`。
四个 C++ 项目在 macOS 与 Linux 上都能构建。

`vorder_sim` 带两层自包含测试，挂在 ctest 上，不需要任何外部数据：

```bash
cmake -S vorder_sim -B vorder_sim/build && cmake --build vorder_sim/build -j8
ctest --test-dir vorder_sim/build --output-on-failure
```

其余项目的比对脚本都依赖真实行情数据，无法离线运行。
第三方依赖在 [`third_party/`](third_party/)，HDF5 与 clickhouse 桩头各一份，
三个 C++ 项目共用。平台差异见 [`third_party/README.md`](third_party/README.md)。

### 路径

数据路径不写在代码里，一律由 [`qr/paths.py`](qr/paths.py) 解析。
顺序是环境变量 `QR_<KEY>`、仓库根的 `qr.toml`、内置默认值，相对路径锚定仓库根。
可配置的键见 [`qr.example.toml`](qr.example.toml)。

### 跨平台

目标环境是 macOS 与 WSL，两边都是 POSIX，`l3_factor/bt/` 下的 bash 批跑脚本直接可用。

代码里不留绝对路径，一律走 `qr/paths.py`。单日流水线 `l3_factor/runday.py` 用标准库
解压和查磁盘余量，不依赖 `unzip` 或 GNU 专有的 `df --output`，所以 mac 上也能跑。

C++ 的 HDF5 优先找系统安装，macOS 用 `brew install hdf5`，WSL 用
`apt install libhdf5-dev`。`third_party/hdf5` 只是找不到时的回退。

## 仓库约定

- **数据不进仓库。** 原始数据、H5、parquet、批跑日志都由 [`.gitignore`](.gitignore) 挡在外面，
  文件仍留在工作目录里。
- **路径不进代码。** 一律走 [`qr/paths.py`](qr/paths.py)，本机配置写在 `qr.toml`，该文件不进仓库。
- **密钥不进仓库。** `massive_down/KEYS.txt` 已被忽略，参照 README 自行准备。
- `_archive/` 是历史文档与旧归档，不纳入版本控制。

## 已知缺口

- `auction/cpp/run.sh` 调用 `merge_action_parquet.py`，该文件不存在，生产流程第 3 步跑不通。
- `build_tick3s` 走 parquet 路线需要 `merge_3s_parquet.py`，同样不存在。
- C++ 侧还有 9 处服务器绝对路径（`/home/sharedriver1/public/...`、`/home/TLData`），
  编译进了 `auction`、`build_tick3s` 的默认值和几个 `run.sh`。Python 侧已全部外置。
