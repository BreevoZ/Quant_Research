# Earlier A-share factors · 早期 A 股因子

[A-shares / A 股入口](../README.md) · [Main L3 research / L3 主线](../l3_factor/README.md)

This legacy project extracts minute-level features from reconstructed H5 order books and evaluates
cross-sectional Rank IC, decay, correlation and quantile returns. New research belongs in `l3_factor`.
The former crypto experiments have moved to [archive/crypto_minute](../../archive/crypto_minute/README.md).

本项目从重建 H5 盘口提取分钟特征，评估截面 Rank IC、衰减、相关性与分组收益。
新研究使用 `l3_factor`；原 crypto 实验已移到独立归档目录。

| Script / 脚本 | Purpose / 用途 |
|---|---|
| `lob_features.py` | H5 → features / 盘口特征提取 |
| `ic_analysis.py` | Cross-sectional Rank IC / 截面 Rank IC |
| `ic_decay.py` | Horizon decay / 期限衰减 |
| `factor_corr.py` | Feature correlation / 因子相关性 |
| `group_backtest.py` | Quantile return diagnostics / 分组收益诊断 |
| `plot_ic.py` | IC plots / IC 绘图 |

From the repository root, with actual data configured in `qr.toml` / 在仓库根执行，需先配置真实数据：

```bash
conda activate mx
python ashare/factor/lob_features.py 20260611
python ashare/factor/ic_analysis.py 20260611
python ashare/factor/plot_ic.py 20260611
```

Paths use `lob_root`, `feat_root` and `res_root`. The original combined project notes are preserved
as [Chinese historical notes](../../archive/factor_research_notes.zh.md).

路径键为 `lob_root`、`feat_root`、`res_root`；原混合项目说明保留为中文历史记录。
