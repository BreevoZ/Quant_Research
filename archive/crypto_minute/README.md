# Crypto minute experiments · Crypto 分钟历史实验

[Archive / 归档入口](../README.md) · [Repository / 全仓](../../README.md)

These scripts used Massive cryptocurrency minute data to test the research evaluation process:
features → Rank IC → lag/skip diagnostics → cross-sectional or time-series backtests.
They are preserved as methodological history, not as a validated trading strategy.

这些脚本曾用 Massive 虚拟币分钟数据验证研究评估流程：特征 → Rank IC → lag/skip 排查 →
截面或时序回测。保留它们用于方法论回溯，不代表已验证的交易策略。

The historical finding was that apparent minute reversal often weakened after separating shared
price endpoints and accounting for bid/ask bounce and costs. See the
[original Chinese notes](../factor_research_notes.zh.md).

历史发现是：分离共享价格端点、考虑买卖价跳动与成本后，表面的分钟反转经常大幅减弱。
详细结论见原中文记录。

Paths remain `crypto_data`, `crypto_feat`, `crypto_ic` in `qr.toml` / 路径键保持不变：

```bash
conda activate mx
python archive/crypto_minute/crypto_features.py 2024-01 2024-03
python archive/crypto_minute/crypto_ic_clean.py
```

Commands require the original datasets. The directory move does not change model assumptions.

命令需要原数据集；目录迁移不改变模型假设。
