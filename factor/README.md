# factor — 早期因子探索与评估框架验证

> ⚠️ **本项目已被 [`l3_factor`](../l3_factor/) 取代，不再维护。**
> 保留原因：这里跑通的评估口径（skip 检验、池化 vs 逐日 IC、分档回测、扣费判断）是后续
> A 股 L3 因子研究的方法论底稿，几条负结论也在这里第一次被建立。新的因子工作请去 `l3_factor`。

两条互不相干的研究线，此前平铺在同一目录里，现已拆开：

```
factor/
├── ashare/            A 股 LOB 盘口因子（基础档位特征 + IC 评估框架）
├── crypto/            海外分钟数据因子（虚拟币，用于验证评估流程本身）
├── features/          ashare 产物：LOB 特征表（按日期分目录，482M）
├── ic/                ashare 产物：IC 汇总
├── results/           ashare 产物：IC 序列 / 衰减 / 分档回测 / 图
├── crypto_features/   crypto 产物：分钟因子表（1272 个日期目录，2.7G）
└── crypto_ic/         crypto 产物：IC 明细
```

---

## 一、ashare/ — A 股 LOB 盘口因子

从 `Lob_new/` 的重建 H5 里按分钟抽盘口特征，再跑截面 Rank IC。用的是盘口前几档信息，
属于**较基础的 LOB 因子**——L3 层的撤单率、队列位置、订单激进度、订单生命周期等特征
在本项目里没有开发，那部分是 `l3_factor` 的内容。

| 文件 | 作用 |
|---|---|
| `_paths.py` | 共用路径配置（`LOB_ROOT` / `FEAT_ROOT` / `RES_ROOT`，均为绝对路径） |
| `lob_features.py` | 特征抽取：H5 → 每股一个 CSV（行=分钟，列=特征）。中间价、价差、相对价差、加权中间价及其偏离、多档盘口不平衡、订单流不平衡、买卖盘深度斜率等 |
| `ic_analysis.py` | 截面 Rank IC 评估 |
| `ic_decay.py` | IC 随持有期的衰减 |
| `factor_corr.py` | 因子间相关矩阵（排除"换皮因子"） |
| `group_backtest.py` | 分档（分位数）回测 |
| `plot_ic.py` | IC 序列 / 热力图 / 衰减曲线出图 |

跑法（`_paths.py` 与各脚本同目录，直接执行即可）：

```bash
cd /home/yhzhou/factor
python ashare/lob_features.py 20260611     # 先抽特征
python ashare/ic_analysis.py   20260611    # 再评估
python ashare/plot_ic.py       20260611    # 出图
```

**已跑过的日期**：`20251205`、`20260611`（见 `features/` 与 `results/`）。

---

## 二、crypto/ — 海外分钟数据因子（评估流程演练）

用 Massive 的虚拟币分钟数据（2023–2026，9 个主流币）做一轮完整的
"因子构建 → IC → 污染排查 → 口径比较 → 回测 → 交易意义判断"。
**目的不是找可交易策略，而是检验评估流程本身是否可靠。**

| 文件 | 作用 |
|---|---|
| `crypto_features.py` | 从分钟 OHLCV 算 18 个基础因子：多周期收益、已实现波动率、量比、VWAP 偏离、价格位置、Amihud 非流动性等。输出格式对齐 A 股 features |
| `crypto_ic_analysis.py` | 逐日 Rank IC |
| `crypto_ic_clean.py` | **污染排查**：跳过一根 K 线隔离共享端点，剥离 bid-ask bounce |
| `crypto_ic_lagprofile.py` | 不同 lag 下的 IC 曲线 |
| `crypto_backtest.py` | 截面市场中性多空回测 |
| `crypto_backtest_ts.py` | 时序多空回测 |
| `crypto_diag.py` | 数据健康度诊断 |

```bash
cd /home/yhzhou/factor
python crypto/crypto_features.py 2024-01 2024-03   # 按月区间算特征
python crypto/crypto_ic_clean.py                   # 污染排查（关键一步）
```

数据源：`/home/yhzhou/massive_data/FLATFILE/Crypto_MIN/{year}/{mm}/{date}.csv`

---

## 三、本项目得到的结论（负结论，但建立了后续所有检验的模板）

1. **漂亮的 IC 可能来自微观结构噪音**。部分短周期因子逐日 Rank IC 看着很强，
   **跳过一根 K 线**隔离共享端点后预测力大幅消失 → 来源是 bid-ask bounce 而非真实 alpha。
   这个"skip 检验"后来在 `l3_factor` 里固化为因子准入的**必过项，且从未失手**。
2. **逐日 IC 会因日内自相关高估因子强度**，须与池化 IC、分档均值、非重叠采样一起判断。
3. **IC 只反映秩相关，不反映收益幅度、不含成本**，必须配回测和扣费盈亏平衡分析。
4. **市场中性化会改变因子暴露**，某些市场级反转信号中性化后方向会变。
5. 最终判断：该类**分钟级反转信号在扣费后不具备交易价值**——即使零手续费下表现也不理想。

---

## 环境

Python 3.11+。依赖见仓库根 `pyproject.toml`，装一次即可：`pip install -e .`。
路径来自 `qr/paths.py`，复制仓库根的 `qr.example.toml` 成 `qr.toml` 后配置。
