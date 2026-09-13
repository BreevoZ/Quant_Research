"""Cross-sectional diagnostics, sampled on non-overlapping horizon grids."""

import numpy as np
import pandas as pd

from .config import Experiment
from .features import feature_names

BUCKET_COLUMNS = ["session", "ts", "feature", "horizon", "n", "rank_ic", "tail_spread_bp"]


def evaluate_session(panel: pd.DataFrame, experiment: Experiment) -> pd.DataFrame:
    rows = []
    for horizon in experiment.horizons:
        # Same phase for every stock, anchored to regular-session open.
        sampled = panel[(panel.minute + 1) % horizon == 0]
        label = f"fwd_{horizon}m"
        for ts, cross in sampled.groupby("ts", sort=True):
            for feature in feature_names(experiment.lookbacks):
                pair = cross[[feature, label]].dropna()
                if (len(pair) < experiment.min_cross_section
                        or pair[feature].nunique() < experiment.quantiles
                        or pair[label].nunique() < 2):
                    continue
                ranks = pair[feature].rank(method="average", pct=True)
                # Ties remain together; no arbitrary ticker-order tie breaking.
                lower = pair.loc[ranks <= 1 / experiment.quantiles, label]
                upper = pair.loc[ranks > 1 - 1 / experiment.quantiles, label]
                if lower.empty or upper.empty:
                    continue
                rows.append({"session": cross.session.iloc[0], "ts": ts,
                             "feature": feature, "horizon": horizon, "n": len(pair),
                             "rank_ic": ranks.corr(pair[label].rank()),
                             "tail_spread_bp": (upper.mean() - lower.mean()) * 10_000})
    return pd.DataFrame(rows, columns=BUCKET_COLUMNS)


def summarize(buckets: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    keys = ["feature", "horizon"]
    daily = buckets.groupby(["session", *keys], as_index=False).agg(
        rank_ic=("rank_ic", "mean"), tail_spread_bp=("tail_spread_bp", "mean"),
        buckets=("rank_ic", "count"), mean_n=("n", "mean"),
    )
    summary = daily.groupby(keys, as_index=False).agg(
        days=("session", "count"), mean_daily_ic=("rank_ic", "mean"),
        std_daily_ic=("rank_ic", "std"), positive_ic_days=("rank_ic", lambda x: (x > 0).mean()),
        mean_daily_tail_spread_bp=("tail_spread_bp", "mean"),
    )
    # Keep the original feature direction; do not flip based on evaluation results.
    return daily, summary.replace([np.inf, -np.inf], np.nan)


def render_report(summary: pd.DataFrame, qa: list[dict], synthetic: bool) -> str:
    tag = ("Synthetic demo / 合成数据 DEMO — pipeline validation only / 仅验证管道" if synthetic
           else "Fixed-universe exploration / 固定股票池探索 — no independent holdout / 未做独立样本外验证")
    lines = [f"# US equity minute baseline：{tag}", "",
             f"Sessions / 交易日: {len(qa)}; feature × horizon diagnostics / 特征 × 持有期诊断: {len(summary)}.", "",
             "Signals use completed bars; labels are delayed open-to-open price proxies, not executable fills.",
             "信号使用已收口分钟；标签为收口后留出至少一分钟的 open-to-open 价格代理。",
             "Each horizon is sampled without overlapping holding windows, then averaged equally by day; horizons remain correlated.",
             "每个持有期按自身长度非重叠采样，再逐日等权汇总；不同持有期仍相关。",
             "High-minus-low group returns are in basis points and are not portfolio net PnL.",
             "高低组价差 = 高特征组收益 − 低特征组收益（bp），不是组合净收益。",
             "Spreads, slippage, impact, borrow, commissions and fills are not modeled; no strategy Sharpe is reported.",
             "未建模买卖价差、滑点、冲击、借券、佣金或真实成交，不报告策略 Sharpe。",
             "Feature signs stay fixed. No independent-day t-statistic is assumed.",
             "原始因子方向固定，不根据本次结果翻转；不把日度样本当成必然独立来计算 t 值。", "",
             "| Feature / 特征 | Horizon min / 持有分钟 | Days / 天数 | Mean daily IC / 日均 IC | Daily IC std / 标准差 | High-low bp / 高低组差 |",
             "|---|---:|---:|---:|---:|---:|"]
    for row in summary.itertuples(index=False):
        std = "—" if pd.isna(row.std_daily_ic) else f"{row.std_daily_ic:.4f}"
        lines.append(f"| {row.feature} | {row.horizon} | {row.days} | {row.mean_daily_ic:.4f} "
                     f"| {std} | {row.mean_daily_tail_spread_bp:.3f} |")
    if summary.empty:
        lines.append("\nInsufficient valid cross-sections; check symbols, coverage and windows. / 没有足够有效截面，请检查股票数、覆盖与窗口。")
    lines += ["", "## Data quality · 数据质量", "",
              "| Date / 日期 | Minutes per symbol / 应有分钟 | Regular rows / 常规行数 | Missing / 缺失分钟 | Outside session / 时段外 | Zero volume / 零量行数 |",
              "|---|---:|---:|---:|---:|---:|"]
    for day in qa:
        lines.append(f"| {day['session']} | {day['expected_minutes_per_ticker']} "
                     f"| {day['regular_rows']} | {sum(day['missing_minutes'].values())} "
                     f"| {day['outside_regular_rows']} | {day['zero_volume_rows']} |")
    lines += ["", "Missing minutes stay empty without compressing time; zero-volume prices are excluded.",
              "缺失分钟不填价、不压缩时间；零成交量价格不参与特征/标签。",
              "Missing bars may reflect no qualifying trades, halts or incomplete files; this does not establish executability.",
              "缺失可能来自无合格成交、停牌或文件缺损，本版不能区分；不能据此证明可成交。",
              "QA covers configured symbols only; per-symbol missing counts are in qa.json.",
              "完整性检查只覆盖配置股票池，逐股票缺失数在 qa.json。", "",
              "## Next steps · 下一步", "", "1. Build point-in-time universes and corporate actions. / 建立历史股票池与公司行动档案。",
              "2. Lock hypotheses and selection windows; reserve an untouched test period. / 固定假设与选择窗，留出独立测试期。",
              "3. Add trades/quotes, executable costs and position accounting. / 接入逐笔报价、可执行成本与持仓账本。", ""]
    return "\n".join(lines)
