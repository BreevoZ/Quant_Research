# l3_factor — A股 L3 订单流因子研究与策略回测

[A-share overview / A 股入口](../README.md) · [Repository / 全仓](../../README.md) · [Setup / 环境](../../docs/SETUP.md)

## English quick start

Main A-share L3 research: event features, IC diagnostics, model combinations and execution experiments. Full runs require local market data; historical reports retain their original Chinese text.

Commands below run from the repository root, using conda `mx` for Python:

```bash
conda activate mx
python ashare/l3_factor/runday.py --help
python ashare/l3_factor/eval/eval_multiday.py --help
```

See the [A-share guide](../README.md) for data contracts, paths and research limitations.
The detailed Chinese implementation reference follows. Its component-local commands assume
`cd ashare/l3_factor` unless they explicitly start from the repository root.

## 中文技术说明

下文保留原项目技术细节；项目内相对命令以 `ashare/l3_factor` 为工作目录，仓库根命令另行标明。

从**逐笔委托流**(order-flow, L3)挖预测因子,并一路做到真实排队回测与执行择时验证。
起点是"从订单号入手"的一族特征——**降频即消失**的信息(降频后只剩价量)。

> **文档导航**
> - 本页 = 项目地图 + 怎么跑 + 当前结论摘要
> - [FACTOR_BOOK.md](FACTOR_BOOK.md) — **因子手册(自动生成)**,每因子一张卡:长期证据/衰减/尾部 alpha
> - [FEATURES.md](FEATURES.md) — 特征字典(唯一登记处) + 全量证据总表
> - [docs/](docs/) — 阶段性报告快照:`REPORT_20260707`(单日评估,历史)、`REPORT_S123`(策略回测)、`REPORT_EXEC_TIMING`(执行择时)

## 为什么是订单流而不是价量

价量类因子噪音大、要降频才有稳定信号(参见 `factor/` 项目里 crypto 分钟反转的负结论)。
订单流类相反:信号藏在**事件之间的关系**(谁挂了多大、谁撤了、排在谁前面),**只在事件级存在**,
一降频就死。这也是这条线的护城河——尤其配合对 A股 L3 数据缺陷的理解(见「数据」)。

---

## 目录结构

```
l3_factor/
├── runday.py         单日流水线(解压 → precache → 抽特征 → 删 CSV), 跨平台
│
├── factors/          特征抽取器
├── eval/             因子评估
├── backtest/         策略回测 + 执行择时(含 strategies/ 策略插件)
├── ml/               机器学习组合器 + 序列模型
├── explore/          跨学科探索(声化/节律/天文安慰剂/指数择时)
├── archive/          已取代或已证伪的历史代码,不再维护
│
├── docs/             阶段性报告快照
├── bt/               评估与回测产物(csv/png/log,120M)
└── data/             数据(4.1T,见下)
```

> **脚本在任何工作目录下调用都一样。** 数据路径统一由仓库根的 `qr/paths.py` 解析
> (环境变量 → `qr.toml` → 默认值),不再相对当前工作目录;
> `CachedRec` 缓存读取搬到了 `qr/l3cache.py`,用 `from qr import l3cache` 引入。
>
> 先在仓库根装一次:`pip install -e ".[ml,explore]"`,然后复制 `qr.example.toml`
> 成 `qr.toml` 指向你本机的数据目录。

### factors/ — 特征抽取

| 文件 | 作用 |
|---|---|
| `factor_id.py` | **主力抽取器**(13 特征 = 订单号系 8 + 流量系 5),126 天面板由它产出 |
| `factor_uniq.py` | 第三代结构性特征(9 因子:拆单链/主动方集中度/残量行为/追价/年龄结构/补墙) |
| `agg_events.py` | 点过程**公共预处理**:订单号合并同笔主动单 → 只留主动方 → 毫秒去重 |
| `hawkes.py` | Hawkes 自激发过程拟合(滚动窗口,因果) |
| `factor_pointproc.py` | 点过程因子(基于上面两个) |
| `bblocks_probe.py` | Bayesian Blocks 探针(**已弃用**:全局最优天生非因果) |

### eval/ — 因子评估

| 文件 | 作用 |
|---|---|
| `eval_multiday.py` | 多日日度 IC 聚合(日间 t + 同号%;特征列自动发现) |
| `ic_horizon.py` | IC 期限结构(skip 进场,h=1~60 分钟) |
| `gen_factor_book.py` | 因子手册生成器 → `python eval/gen_factor_book.py > FACTOR_BOOK.md` |
| `eval_pointproc.py` | 点过程因子专用评估 |

### backtest/ — 回测与执行择时

| 文件 | 作用 |
|---|---|
| `vgen.py` | 「策略 + 当日面板 → vsim 订单」通用框架:触发调度/防前视/订单编排/meta 配对 |
| `strategies/*.py` | 策略插件(PARAMS/COLS/on_bucket 三件套,~15 行一个):`eat_age_rev`/`trd_imb_rev`/`combo_rev` |
| `vloop.py` | **不动点迭代**:依赖自身成交的逻辑(平仓量=实际成交/达量撤单)= 开环回放迭代到收敛(实测两轮) |
| `pnl.py` | 底仓 T+0 配对结算(佣金 2bp 双边 + 印花 5bp 卖出;进出不齐记底仓吸收) |
| `exec_timing.py` | **执行择时对照实验**(TWAP / 随机权重 / 信号择时 三组) |
| `phase_sweep.py` | 参数相位扫描 |
| `plot_pnl.py` / `plot_h1_pnl.py` / `plot_compare.py` | 盈亏追踪图 / 分布 / 逆向选择诊断 |

**回测流程**:`backtest/vgen.py --strategy S --panel P` → (`backtest/vloop.py`) → `vsim --orders ...` → `backtest/pnl.py` → `backtest/plot_pnl.py`

### ml/ — 机器学习

| 文件 | 作用 |
|---|---|
| `ml_prep.py` | 样本准备(每分钟截面 rank 归一) |
| `ml_combine.py` | **三层对照**:最强单因子 / Ridge 线性 / 梯度提升树,按月扩窗 walk-forward |
| `ml_placebo.py` | 安慰剂:截面内打乱训练标签重跑 |
| `ml_interact.py` | H 统计量扫两两交互 + 二维部分依赖网格 |
| `seq_data.py` / `seq_model.py` / `nn_gru.py` | 序列模型直接读原始事件流(**纯 numpy 手写 GRU**,含梯度检验) |

### explore/ — 跨学科探索

| 文件 | 作用 |
|---|---|
| `sonify.py` | 订单流**声化**:一天逐笔 → 30 秒立体声(五声音阶/涨跌调式/成交密度控疏密) |
| `market_rhythm.py` | 全市场日内节律 FFT(逐秒序列,去 U 型基线) |
| `raw_tick_ls.py` | 单股逐笔 Lomb-Scargle(真不规则采样,打乱间隔做 null) |
| `plot_rhythm.py` | 节律出图 |
| `market_hr.py` / `by_cluster_ic.py` | 市场"赫罗图"聚类 + 分族验证因子 IC |
| `index_ts.py` / `index_strat.py` | L3 因子聚合为市场状态 → 指数择时 |
| `tess_fetch.py` / `tess_lc.py` / `tess_placebo.py` | **截面管道阴性校准**(TESS 恒星光变,零因果由时间箭头保证) |
| `agn_placebo.py` | 时序管道阴性校准(ZTF 类星体光变) |
| `read_tick3s.py` | 读 3 秒切片数据 |

---

## 数据

- **来源**:`vorder_sim` 全市场 flow 二进制缓存(`CachedRec`,逐事件、非降频)。`qr/l3cache.py` 负责读 + 校验。
- **静默误读防护**:`l3cache.load()` 校验 MAGIC + parserVersion + 记录尺寸,任一不符**当场报错**
  (缓存被新解析器重生成 → 语义/布局可能变 → 拒读,而不是读出错位垃圾)。
- **只用 SZ**:SZ 原始逐笔干净(委托记录完整、`ApplSeqNum` 权威)。
  **SH 原始逐笔是坏的**(委托量=净掉的挂住量、41% 成交无委托记录、方向标 N),
  做 SH 要改用 `datacheck_final` 的**补全后**逐事件数据(LobBuilder 回放),不能用生数据。
- **数据形态**:用**原始逐笔事件流**,不是重建盘口状态(后者会抹掉单笔量/号/撤单)。

### data/ 里有什么(4.1T)

| 目录 | 大小 | 内容 |
|---|---|---|
| `cache/` | 4.0T | 逐日二进制缓存,126 天(2026H1) + 90 天(2023-2025 样本外) |
| `panels/` | 39G | `factor_id` 13 列特征面板,按日一个 CSV |
| `panels_uniq/` | 15G | `factor_uniq` 9 列面板 |
| `ml/` | 5.4G | 机器学习样本(parquet) |
| `seq/` | 789M | 序列模型样本(npz) |
| `seq_old/` | 2.7G | ⚠️ 旧版序列样本(事件数窗口 K=64,已被时间窗版本取代)**可删** |
| `agn/`, `index_min_2026h1.csv`, `market_ts.csv` | — | 安慰剂与指数数据 |

---

## 怎么跑

```bash
# From repository root / 从仓库根开始
cd ashare/l3_factor          # bt/ 下的批跑脚本会自己切到这里, 不必先 cd

# 1. 单日流水线(解压 SZ 双文件 → vsim precache → 抽特征 → 删 CSV)
#    幂等: 面板已存在则整天跳过; 原子解压; 磁盘 <300GB 刹车
python runday.py 20260707      # 在任何目录下调用都可以

# 2. 多日评估
python eval/eval_multiday.py --days 20260707,20260708 --dump bt/daily_ic.csv
python eval/ic_horizon.py    --panels data/panels
python eval/gen_factor_book.py > FACTOR_BOOK.md    # 数据更新后重跑

# 3. 策略回测
python backtest/vgen.py --strategy backtest/strategies/eat_age_rev.py \
       --panel data/panels/panel_id_20260707.csv --outdir bt/out --tag h1_20260707
python backtest/vloop.py --orders bt/out/orders_h1_20260707.csv --meta bt/out/meta_h1_20260707.csv
python backtest/pnl.py   --res bt/out/res_h1_20260707.csv --meta bt/out/meta_h1_20260707.csv

# 4. 执行择时对照实验
python backtest/exec_timing.py --days 20 --universe 60 --lam 0.5

# 批量: bt/ 下有 runner(nightchain.sh / overnight.sh / h1day.sh / s123day.sh),同样从根目录调用
```

---

## 当前结论(截至 2026-07,已含 2023-2025 样本外验证)

> 详细证据见 [FACTOR_BOOK.md](FACTOR_BOOK.md) 与 [FEATURES.md](FEATURES.md);
> 早期单日结论已归档为 [docs/REPORT_20260707.md](docs/REPORT_20260707.md),**勿再引用其数字**。

**1. 因子(三代共 22 个,每个建立四维证据档案)**

`eat_age_imb`(被吃单年龄失衡)是全项目主力:2026H1 半年 124/124 天同号、日间 t≈−51;
**2023-2025 样本外 90 天 skip 口径 −0.031(t≈−47)、100% 同号,不降反升** →
已从"2026H1 的规律"升级为**横跨三年、不同 regime 的结构性反转规律**。
`flash_cxl_imb` 则在样本外由近零变为 −0.005 且变号——**skip 检验的一票否决力再次确认**。

**2. 独立日内往返策略:已被系统性证伪**

半年 126 个交易日、28,329 笔真实排队往返:净收益 **−11.0bp/双边成交额、126 天盈利天数为零**。
成本堆栈:信号纸面 alpha 8~11bp(不随持有期增长),而费税 9bp(印花 5 不可谈)+ 出场穿价 8~14bp
+ 进场 2.3bp + 逆向选择约 5bp。三种执行方式与两项执行优化均无法翻正。
**结论:该类分钟级信号作为个股独立日内往返策略,在 A 股现行成本结构下数学上无解。**

**3. 执行择时:本阶段首个正向结果**

若交易已因其他原因发生(机构母单执行),费税与价差属沉没成本,信号不承担新增成本。
1200 个母单(20 天 × 60 股 × 买卖)上,信号择时相对 TWAP 改善执行价 **+2.83bp,日间 t=5.35**,
20 天中 18 天为正;**随机对照组落在零上(−0.06bp, t=−0.24)**,排除"非均匀分配本身有利"的混淆解释。

**4. 机器学习:定位为组合器而非发现器**

三层对照:单因子 0.0215 → Ridge 0.0306 → GBDT 0.0356(配对日间 t=29),六个月全部复现;
打乱标签后塌至 0.0003。交互诊断发现**"状态因子"**——被吃单年龄中位数等因子自身不指示方向,
但决定其他因子该如何被解读(二维网格上趋势随另一维取值反向)。
限制:增量落在排序质量而非可交易收益(尾部 alpha 仅 0.5→0.9bp)。

**5. 管道本底噪声 σ(常驻参考量)**

TESS 恒星光变阴性校准得:生产规模 N≈2800 下 **0.00034(1min)~0.00014(30min)**。
**任何因子的 IC 必须显著高于此值才有意义。**

---

## 评估协议(选择级泄漏防护)

交易级前视(策略偷看未来)之外,还有**选择级泄漏**:用未来数据的 IC 选中因子/参数,再回测过去
—— 每笔交易因果干净,但"因子在未来有效"这个条件抬高了它在过去有效的先验,回测不再是独立检验。

1. **时间箭头**:选择窗必须早于回测窗。2023-2025 三年数据未参与任何选择,是最终验证的净土
   (已回跑 90 个交易日,见上「当前结论 1」)。
2. **holdout**:划从未参与选择的整月,任何策略只允许在其上跑一次。
3. **多重检验记账**:已试验特征×标签×持有期 300+ 次,入选者显著性按试验次数打折;
   同号%/机制解释作为运气无法伪造的旁证。
4. **负结论豁免**:选择级泄漏只夸大正结果;死刑判决(半年 −11bp)可直接采信。

---

## 仍待完成

1. **历史面板回填**:2023-2025 已跑 90 天(用于因子验证),但**执行择时(+2.83bp)、指数择时、
   低频截面**三项结论仍受选择级泄漏制约,需在净土年份复验。
2. **执行择时场景敏感性**:仅测试单一场景,订单规模/时间窗/被动挂单混合方式未考察;
   λ 与钳制区间一次设定全程未调(刻意防过拟合,但也不知最优值)。
3. **低频截面线**:把 L3 特征按周/月聚合为"知情交易强度"类选股因子(此频率下换手成本自然解除)。
4. 置换检验(日期标签重排的零分布校准)列装为评估模块标准件。
5. `data/seq_old/`(2.7G)已被取代,可清理。

## 环境

Python 3.11+(`qr/paths.py` 用标准库 tomllib 读配置)。依赖见仓库根 `pyproject.toml`。
无 GPU,序列模型为纯 numpy 手写实现(外网到 PyPI 不通,装不了 torch)。
