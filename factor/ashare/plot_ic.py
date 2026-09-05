#!/usr/bin/env python3
"""
IC 可视化：
  1. IC 衰减曲线（各因子在不同预测周期的 IC 均值）
  2. imb1 的逐时间点 IC 序列（盘中 IC 分布）
"""

import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import os
import sys
from _paths import res_dir, plot_dir

# ─── 1. IC 衰减曲线 ────────────────────────────────────────────────────────────

def plot_ic_decay(ic_dir, pdir):
    decay = pd.read_csv(f'{ic_dir}/ic_decay.csv', index_col=0)
    horizons = [int(c.replace('t+', '')) for c in decay.columns]

    fig, axes = plt.subplots(1, 2, figsize=(14, 5))

    # 左：绝对 IC 值
    ax = axes[0]
    for factor in decay.index:
        vals = decay.loc[factor].values
        ax.plot(horizons, vals, marker='o', label=factor, linewidth=1.8)
    ax.axhline(0, color='black', linewidth=0.8, linestyle='--')
    ax.set_xlabel('Forward horizon (minutes)')
    ax.set_ylabel('Rank IC mean')
    ax.set_title('IC Decay — Absolute')
    ax.set_xticks(horizons)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    # 右：相对衰减（归一化到 t+1 = 100%）
    ax = axes[1]
    decay_norm = decay.div(decay['t+1'].abs(), axis=0) * 100
    for factor in decay_norm.index:
        vals = decay_norm.loc[factor].values
        ax.plot(horizons, vals, marker='o', label=factor, linewidth=1.8)
    ax.axhline(0,   color='black', linewidth=0.8, linestyle='--')
    ax.axhline(50,  color='gray',  linewidth=0.6, linestyle=':')
    ax.set_xlabel('Forward horizon (minutes)')
    ax.set_ylabel('IC retained (%)')
    ax.set_title('IC Decay — Normalized to t+1')
    ax.set_xticks(horizons)
    ax.yaxis.set_major_formatter(mticker.PercentFormatter())
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    fig.tight_layout()
    out = f'{pdir}/ic_decay.png'
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"Saved: {out}")

# ─── 2. imb1 IC 序列（盘中分布）──────────────────────────────────────────────

def plot_ic_series(ic_dir, pdir):
    ic = pd.read_csv(f'{ic_dir}/ic_series.csv', index_col='time')

    # 只画强因子
    strong = ['imb1', 'wmid_dev', 'bid_slope', 'ask_slope']
    ic_strong = ic[strong]

    fig, axes = plt.subplots(2, 2, figsize=(14, 8), sharex=True)
    axes = axes.flatten()

    # 找午休开始位置（11:30:00 是上午最后一格）
    times = list(ic_strong.index)
    lunch_x = times.index('11:30:00') if '11:30:00' in times else None

    for i, col in enumerate(strong):
        ax = axes[i]
        vals = ic_strong[col]
        x    = range(len(vals))

        # 柱状图，正负不同色
        colors = ['#d62728' if v > 0 else '#1f77b4' for v in vals.fillna(0)]
        ax.bar(x, vals.fillna(0), color=colors, alpha=0.7, width=0.8)
        ax.axhline(0, color='black', linewidth=0.8)

        mean_ic = vals.mean()
        ax.axhline(mean_ic, color='orange', linewidth=1.2, linestyle='--',
                   label=f'mean={mean_ic:.3f}')

        # 午休分隔线
        if lunch_x is not None:
            ax.axvline(lunch_x + 0.5, color='gray', linewidth=1.2,
                       linestyle='--', alpha=0.8)
            ymin, ymax = ax.get_ylim()
            ax.text(lunch_x + 0.7, ymax * 0.92, 'Lunch\n11:30',
                    fontsize=7, color='gray', va='top')

        ax.set_title(col, fontsize=11)
        ax.set_ylabel('Rank IC')
        ax.legend(fontsize=8)
        ax.grid(alpha=0.2)

        # x 轴：只显示整点时间标签（每30格一个）
        tick_pos  = list(range(0, len(vals), 30))
        tick_labs = [vals.index[p] for p in tick_pos]
        ax.set_xticks(tick_pos)
        ax.set_xticklabels(tick_labs, rotation=30, fontsize=7)

    fig.suptitle('Intraday IC Series (t+1 forward return)', fontsize=13)
    fig.tight_layout()
    out = f'{pdir}/ic_series.png'
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"Saved: {out}")

# ─── 3. IC 衰减热力图 ─────────────────────────────────────────────────────────

def plot_ic_heatmap(ic_dir, pdir):
    decay = pd.read_csv(f'{ic_dir}/ic_decay.csv', index_col=0)

    fig, ax = plt.subplots(figsize=(9, 4))
    data = decay.values
    im = ax.imshow(data, aspect='auto', cmap='RdYlGn',
                   vmin=-0.2, vmax=0.2)
    plt.colorbar(im, ax=ax, label='Rank IC')

    ax.set_xticks(range(len(decay.columns)))
    ax.set_xticklabels(decay.columns)
    ax.set_yticks(range(len(decay.index)))
    ax.set_yticklabels(decay.index)

    for i in range(len(decay.index)):
        for j in range(len(decay.columns)):
            ax.text(j, i, f'{data[i,j]:.3f}', ha='center', va='center',
                    fontsize=9, color='black')

    ax.set_title('IC Decay Heatmap')
    fig.tight_layout()
    out = f'{pdir}/ic_heatmap.png'
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"Saved: {out}")

if __name__ == '__main__':
    date   = sys.argv[1] if len(sys.argv) > 1 else '20260611'
    ic_dir = res_dir(date)
    pdir   = plot_dir(date)
    plot_ic_decay(ic_dir, pdir)
    plot_ic_series(ic_dir, pdir)
    plot_ic_heatmap(ic_dir, pdir)
    print("All plots saved to", pdir)
