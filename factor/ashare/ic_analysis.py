#!/usr/bin/env python3
"""
IC 分析：用 t 时刻的因子值预测 t+1 时刻的 ret1（前向1分钟中间价收益率）
输出：
  ic_series.csv  - 每个时间点的截面 Rank IC（每列一个因子）
  ic_summary.csv - 各因子 IC 均值、IR、胜率、t-stat
"""

import pandas as pd
import numpy as np
import os
import sys
from pathlib import Path
from _paths import feat_dir, res_dir

FACTOR_COLS = [
    'mid', 'spread', 'rel_spread', 'wmid', 'wmid_dev',
    'imb1', 'imb2', 'imb3', 'imb5',
    'ofi', 'bid_slope', 'ask_slope', 'bid1_vol', 'ask1_vol',
]

# ─── 加载所有股票，构建面板 ───────────────────────────────────────────────────

def load_panel(feature_dir: str, max_stocks: int = None) -> dict:
    """
    返回:
      panels['ret1_fwd'] : DataFrame (time × stock)，前向1分钟收益率
      panels[factor]     : DataFrame (time × stock)，各因子值
    """
    files = sorted(Path(feature_dir).glob('*.csv'))
    if max_stocks:
        files = files[:max_stocks]

    print(f"Loading {len(files)} stocks...")

    panels = {col: {} for col in FACTOR_COLS + ['ret1_fwd']}
    ref_index = None

    for fp in files:
        try:
            df = pd.read_csv(fp, index_col='time')
        except Exception as e:
            print(f"  skip {fp.name}: {e}", file=sys.stderr)
            continue

        if ref_index is None:
            ref_index = df.index
        elif not df.index.equals(ref_index):
            continue  # 时间格点不对齐，跳过

        # 清洗：13:01:00 的 ret1 是跨午休的90分钟收益，置 NaN
        if '13:01:00' in df.index:
            df.loc['13:01:00', 'ret1'] = np.nan

        # 前向收益：ret1_fwd[t] = ret1[t+1]（13:01:00 已 NaN，11:30:00 的前向也自动 NaN）
        ret1_fwd = df['ret1'].shift(-1)
        panels['ret1_fwd'][fp.stem] = ret1_fwd

        for col in FACTOR_COLS:
            if col in df.columns:
                panels[col][fp.stem] = df[col]

    # dict of Series → DataFrame (time × stock)
    result = {k: pd.DataFrame(v, index=ref_index) for k, v in panels.items()}
    n_stocks = result['ret1_fwd'].shape[1]
    print(f"Loaded {n_stocks} stocks, {len(ref_index)} time points")
    return result

# ─── Rank IC（Spearman）────────────────────────────────────────────────────────

def compute_rank_ic(factor_panel: pd.DataFrame, ret1_fwd: pd.DataFrame) -> pd.Series:
    """
    逐行（每个时间点）计算截面 Spearman 相关系数。
    factor_panel, ret1_fwd: 均为 (time × stock) DataFrame
    返回 IC 序列 (time,)
    """
    # rank 沿 axis=1（跨股票截面 rank），min_periods=10 过滤样本太少的时间点
    f_rank = factor_panel.rank(axis=1)
    y_rank = ret1_fwd.rank(axis=1)
    # corrwith(axis=1) 对每一行计算两个 DataFrame 行之间的相关系数
    ic = f_rank.corrwith(y_rank, axis=1, method='pearson')
    # 有效股票数 < 10 的时间点置 NaN
    n_valid = factor_panel.notna() & ret1_fwd.notna()
    ic[n_valid.sum(axis=1) < 10] = np.nan
    return ic

# ─── 汇总统计 ────────────────────────────────────────────────────────────────

def summarize(ic: pd.Series) -> pd.Series:
    valid = ic.dropna()
    if len(valid) < 5:
        return pd.Series(dict(mean=np.nan, std=np.nan, ir=np.nan,
                               win_rate=np.nan, t_stat=np.nan, n=0))
    m  = valid.mean()
    s  = valid.std()
    return pd.Series({
        'mean':     m,
        'std':      s,
        'ir':       m / s if s > 0 else np.nan,
        'win_rate': (valid > 0).mean(),
        't_stat':   m / (s / np.sqrt(len(valid))) if s > 0 else np.nan,
        'n':        len(valid),
    })

# ─── 主流程 ──────────────────────────────────────────────────────────────────

def main():
    date       = sys.argv[1] if len(sys.argv) > 1 else '20260611'
    max_stocks = int(sys.argv[2]) if len(sys.argv) > 2 else None
    out_dir    = res_dir(date)
    os.makedirs(out_dir, exist_ok=True)

    panels   = load_panel(feat_dir(date), max_stocks)
    ret1_fwd = panels['ret1_fwd']

    print("Computing Rank IC...")
    ic_dict = {}
    for col in FACTOR_COLS:
        ic_dict[col] = compute_rank_ic(panels[col], ret1_fwd)

    ic_df      = pd.DataFrame(ic_dict)
    summary_df = ic_df.apply(summarize).T

    ic_df.to_csv(os.path.join(out_dir, 'ic_series.csv'), float_format='%.6f')
    summary_df.to_csv(os.path.join(out_dir, 'ic_summary.csv'), float_format='%.4f')

    print(f"\nIC Summary [{date}] (predicting ret1 at t+1):")
    print(summary_df[['mean', 'ir', 'win_rate', 't_stat']].to_string(float_format='{:.4f}'.format))
    print(f"\nSaved to {out_dir}/")

if __name__ == '__main__':
    main()
