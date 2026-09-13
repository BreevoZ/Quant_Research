#!/usr/bin/env python3
"""
因子相关性分析：计算各因子之间的截面 Spearman 相关系数
逻辑：在每个时间点 t，把所有股票的两个因子值做 rank 相关；对全天 240 个时间点取均值。
相关性高（>0.7）说明两个因子在衡量同一件事，组合后增益有限。
"""

import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os
import sys
from pathlib import Path
from _paths import feat_dir, res_dir, plot_dir

FACTOR_COLS = [
    'wmid_dev', 'imb1', 'imb2', 'imb3', 'imb5',
    'ofi', 'bid_slope', 'ask_slope', 'bid1_vol', 'ask1_vol',
]

# ─── 加载面板 ─────────────────────────────────────────────────────────────────

def load_panel(feature_dir, max_stocks=None):
    files = sorted(Path(feature_dir).glob('*.csv'))
    if max_stocks:
        files = files[:max_stocks]
    print(f"Loading {len(files)} stocks...")

    panels = {col: {} for col in FACTOR_COLS}
    ref_index = None
    for fp in files:
        try:
            df = pd.read_csv(fp, index_col='time', usecols=['time'] + FACTOR_COLS)
        except Exception as e:
            continue
        if ref_index is None:
            ref_index = df.index
        elif not df.index.equals(ref_index):
            continue
        for col in FACTOR_COLS:
            panels[col][fp.stem] = df[col]

    result = {k: pd.DataFrame(v, index=ref_index) for k, v in panels.items()}
    print(f"Loaded {next(iter(result.values())).shape[1]} stocks")
    return result

# ─── 截面平均 Spearman 相关矩阵 ───────────────────────────────────────────────

def factor_corr_matrix(panels: dict) -> pd.DataFrame:
    """
    对每个时间点，把各因子的截面 rank 序列两两做 Pearson 相关（等价于 Spearman）；
    再对全天时间点取均值，得到因子相关矩阵。
    """
    factors = list(panels.keys())
    n = len(factors)

    # 先把所有面板 rank（axis=1 = 截面 rank）
    ranked = {col: panels[col].rank(axis=1) for col in factors}

    # 逐时间点计算所有因子对的相关系数，累加后平均
    T = next(iter(panels.values())).shape[0]
    corr_sum  = np.zeros((n, n))
    corr_cnt  = np.zeros((n, n))

    for ti in range(T):
        # 把这个时间点所有因子的截面 rank 拼成一个 DataFrame（股票 × 因子）
        cols_at_t = pd.DataFrame(
            {col: ranked[col].iloc[ti] for col in factors}
        ).dropna()

        if len(cols_at_t) < 10:
            continue

        c = cols_at_t.corr(method='pearson').values
        valid = ~np.isnan(c)
        corr_sum[valid] += c[valid]
        corr_cnt[valid] += 1

    corr_avg = np.where(corr_cnt > 0, corr_sum / corr_cnt, np.nan)
    return pd.DataFrame(corr_avg, index=factors, columns=factors)

# ─── 画图 ─────────────────────────────────────────────────────────────────────

def plot_corr_heatmap(corr_df: pd.DataFrame, pdir: str):
    fig, ax = plt.subplots(figsize=(9, 7))
    data = corr_df.values

    im = ax.imshow(data, cmap='RdBu_r', vmin=-1, vmax=1, aspect='auto')
    plt.colorbar(im, ax=ax, label='Spearman Correlation')

    labels = corr_df.index.tolist()
    ax.set_xticks(range(len(labels))); ax.set_xticklabels(labels, rotation=35, ha='right')
    ax.set_yticks(range(len(labels))); ax.set_yticklabels(labels)

    for i in range(len(labels)):
        for j in range(len(labels)):
            v = data[i, j]
            if not np.isnan(v):
                color = 'white' if abs(v) > 0.6 else 'black'
                ax.text(j, i, f'{v:.2f}', ha='center', va='center',
                        fontsize=8, color=color)

    ax.set_title('Factor Cross-Sectional Correlation (mean over all time points)', pad=12)
    fig.tight_layout()
    out = f'{pdir}/factor_corr.png'
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"Saved: {out}")

# ─── 主流程 ──────────────────────────────────────────────────────────────────

def main():
    date       = sys.argv[1] if len(sys.argv) > 1 else '20260611'
    max_stocks = int(sys.argv[2]) if len(sys.argv) > 2 else None
    out_dir    = res_dir(date);  os.makedirs(out_dir, exist_ok=True)
    pdir       = plot_dir(date)

    panels = load_panel(feat_dir(date), max_stocks)

    print("Computing factor correlation matrix...")
    corr_df = factor_corr_matrix(panels)

    out_csv = f'{out_dir}/factor_corr.csv'
    corr_df.to_csv(out_csv, float_format='%.3f')
    print(f"\n相关矩阵：")
    print(corr_df.to_string(float_format='{:.3f}'.format))

    plot_corr_heatmap(corr_df, pdir)
    print(f"\nSaved: {out_csv}")

if __name__ == '__main__':
    main()
