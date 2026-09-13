#!/usr/bin/env python3
"""
分组回测 + 因子组合：
  1. 对 imb1、bid_slope、ask_slope 分别做5分位分组回测
  2. 构造等权合成因子，同样做分组回测
  3. 输出各组累计收益曲线和分组收益统计
"""

import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os
import sys
from pathlib import Path
from _paths import feat_dir, res_dir, plot_dir, gbt_dir

SINGLE_FACTORS = ['imb1', 'bid_slope', 'ask_slope']
N_GROUPS = 5

# ─── 加载面板 ─────────────────────────────────────────────────────────────────

def load_panel(feature_dir, max_stocks=None):
    files = sorted(Path(feature_dir).glob('*.csv'))
    if max_stocks:
        files = files[:max_stocks]
    print(f"Loading {len(files)} stocks...")

    cols_needed = SINGLE_FACTORS + ['mid']
    panels = {col: {} for col in cols_needed}
    ref_index = None

    for fp in files:
        try:
            df = pd.read_csv(fp, index_col='time', usecols=['time'] + cols_needed)
        except Exception:
            continue
        if ref_index is None:
            ref_index = df.index
        elif not df.index.equals(ref_index):
            continue
        for col in cols_needed:
            panels[col][fp.stem] = df[col]

    result = {k: pd.DataFrame(v, index=ref_index) for k, v in panels.items()}
    print(f"Loaded {result['mid'].shape[1]} stocks, {len(ref_index)} time points")
    return result

# ─── 合成因子 ─────────────────────────────────────────────────────────────────

def make_composite(panels: dict) -> pd.DataFrame:
    """
    等权合成：把 imb1、bid_slope、ask_slope 分别截面 rank 后等权相加。
    ask_slope 反向（IC 为负），所以取负。
    """
    r_imb1      =  panels['imb1'].rank(axis=1)
    r_bid_slope =  panels['bid_slope'].rank(axis=1)
    r_ask_slope = -panels['ask_slope'].rank(axis=1)   # 反向

    # 只在三个都有值时才合成
    composite = (r_imb1 + r_bid_slope + r_ask_slope) / 3
    composite[r_imb1.isna() | r_bid_slope.isna() | r_ask_slope.isna()] = np.nan
    return composite

# ─── 分组回测核心 ─────────────────────────────────────────────────────────────

def group_backtest(factor: pd.DataFrame, mid: pd.DataFrame,
                   n_groups: int = 5) -> dict:
    """
    每个时间点按因子值分成 n_groups 组，计算下一分钟各组平均收益。
    返回：
      group_ret  : DataFrame (time × group)，各组每分钟平均收益
      cum_ret    : DataFrame (time × group)，累计收益
      summary    : DataFrame，各组年化、总收益、胜率等统计
    """
    # 前向1分钟收益：(mid[t+1] - mid[t]) / mid[t]
    # 13:01:00 跨午休，对应 11:30:00 的前向收益也是跨午休的，两处都置 NaN
    ret1 = mid.pct_change()
    ret1.loc['13:01:00'] = np.nan
    fwd_ret = ret1.shift(-1)

    # 截面分位数分组：qcut 对每行（每个时间点）按因子值分组
    T = factor.shape[0]
    group_labels = list(range(1, n_groups + 1))
    group_ret = pd.DataFrame(index=factor.index, columns=group_labels, dtype=float)

    for ti in range(T):
        f_row  = factor.iloc[ti]
        r_row  = fwd_ret.iloc[ti]
        valid  = f_row.notna() & r_row.notna()
        if valid.sum() < n_groups * 3:
            continue
        try:
            groups = pd.qcut(f_row[valid], n_groups, labels=group_labels)
        except ValueError:
            continue
        for g in group_labels:
            mask = groups == g
            if mask.sum() > 0:
                group_ret.loc[factor.index[ti], g] = r_row[valid][mask].mean()

    cum_ret = (1 + group_ret.fillna(0)).cumprod() - 1

    # 统计汇总
    summary_rows = []
    for g in group_labels:
        ret = group_ret[g].dropna()
        total_ret = (1 + ret).prod() - 1
        summary_rows.append({
            'group':       g,
            'mean_ret':    ret.mean(),
            'total_ret':   total_ret,
            'win_rate':    (ret > 0).mean(),
            'sharpe':      ret.mean() / ret.std() * np.sqrt(240) if ret.std() > 0 else np.nan,
            'n':           len(ret),
        })
    summary = pd.DataFrame(summary_rows).set_index('group')

    # 多空组合（第5组 - 第1组）
    ls_ret = group_ret[n_groups] - group_ret[1]
    ls_total = (1 + ls_ret.fillna(0)).prod() - 1
    ls_sharpe = ls_ret.mean() / ls_ret.std() * np.sqrt(240) if ls_ret.std() > 0 else np.nan
    summary.loc['L-S'] = {
        'mean_ret':  ls_ret.mean(),
        'total_ret': ls_total,
        'win_rate':  (ls_ret > 0).mean(),
        'sharpe':    ls_sharpe,
        'n':         ls_ret.notna().sum(),
    }

    return {'group_ret': group_ret, 'cum_ret': cum_ret, 'summary': summary,
            'ls_ret': ls_ret}

# ─── 画图 ─────────────────────────────────────────────────────────────────────

def _add_lunch_line(ax, n_total, lunch_idx, label_y_frac=0.92):
    """在时间序列图上画午休分隔线（11:30 之后）"""
    if lunch_idx is None:
        return
    ax.axvline(lunch_idx + 0.5, color='gray', linewidth=1.2,
               linestyle='--', alpha=0.8)
    ymin, ymax = ax.get_ylim()
    y_range = ymax - ymin
    ax.text(lunch_idx + 1, ymin + y_range * label_y_frac,
            'Lunch\n11:30', fontsize=7, color='gray', va='top')


def plot_group_results(results: dict, title: str, filename: str, pdir: str):
    fig, axes = plt.subplots(1, 3, figsize=(16, 5))

    cum = results['cum_ret']
    n_total = len(cum)
    # 240 格时间序列中 11:30:00 是第 120 格（上午 09:31→11:30 共 120 格，index=119）
    lunch_idx = 119  # 0-based index of 11:30:00

    # 左：各组累计收益曲线
    ax = axes[0]
    colors = plt.cm.RdYlGn(np.linspace(0.1, 0.9, N_GROUPS))
    for i, g in enumerate(cum.columns):
        ax.plot(cum[g].values * 100, label=f'G{g}', color=colors[i], linewidth=1.5)
    _add_lunch_line(ax, n_total, lunch_idx)
    ax.set_title('Cumulative Return by Group (%)')
    ax.set_xlabel('Time (minute)')
    ax.set_ylabel('Cumulative return (%)')
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    # 中：各组平均收益（条形图，无时间轴，不加午休线）
    ax = axes[1]
    summary = results['summary'].loc[1:N_GROUPS]
    mean_rets = summary['mean_ret'] * 100
    bar_colors = plt.cm.RdYlGn(np.linspace(0.1, 0.9, N_GROUPS))
    bars = ax.bar(range(1, N_GROUPS + 1), mean_rets, color=bar_colors)
    ax.axhline(0, color='black', linewidth=0.8)
    ax.set_title('Mean Return per Minute by Group (%)')
    ax.set_xlabel('Group (1=lowest factor, 5=highest)')
    ax.set_ylabel('Mean return (%)')
    for bar, v in zip(bars, mean_rets):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                f'{v:.4f}%', ha='center', va='bottom', fontsize=8)
    ax.grid(alpha=0.3, axis='y')

    # 右：多空组合累计收益
    ax = axes[2]
    ls_cum = (1 + results['ls_ret'].fillna(0)).cumprod() - 1
    ax.plot(ls_cum.values * 100, color='navy', linewidth=1.5)
    ax.axhline(0, color='black', linewidth=0.8, linestyle='--')
    _add_lunch_line(ax, n_total, lunch_idx)
    ls_total = results['summary'].loc['L-S', 'total_ret']
    ls_sharpe = results['summary'].loc['L-S', 'sharpe']
    ax.set_title(f'Long-Short Cumulative Return\n'
                 f'Total={ls_total*100:.2f}%  Sharpe={ls_sharpe:.2f}')
    ax.set_xlabel('Time (minute)')
    ax.set_ylabel('Cumulative return (%)')
    ax.grid(alpha=0.3)

    fig.suptitle(title, fontsize=13, y=1.01)
    fig.tight_layout()
    out = f'{pdir}/{filename}'
    fig.savefig(out, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"Saved: {out}")

# ─── 主流程 ──────────────────────────────────────────────────────────────────

def main():
    date       = sys.argv[1] if len(sys.argv) > 1 else '20260611'
    max_stocks = int(sys.argv[2]) if len(sys.argv) > 2 else None
    gbt        = gbt_dir(date)
    pdir       = plot_dir(date)

    panels = load_panel(feat_dir(date), max_stocks)
    mid    = panels['mid']

    all_factors = {col: panels[col] for col in SINGLE_FACTORS}
    all_factors['composite'] = make_composite(panels)

    print("\n" + "="*60)
    for name, factor in all_factors.items():
        print(f"\nRunning group backtest: {name} ...")
        res = group_backtest(factor, mid, N_GROUPS)

        res['summary'].to_csv(f'{gbt}/{name}_summary.csv', float_format='%.6f')

        print(f"  {'Group':<6} {'MeanRet%':>10} {'TotalRet%':>11} {'WinRate':>9} {'Sharpe':>8}")
        print(f"  {'-'*48}")
        for g, row in res['summary'].iterrows():
            print(f"  {str(g):<6} {row['mean_ret']*100:>10.4f} "
                  f"{row['total_ret']*100:>11.3f} "
                  f"{row['win_rate']:>9.3f} {row['sharpe']:>8.2f}")

        plot_group_results(res, f'Group Backtest [{date}]: {name}',
                           f'group_bt_{name}.png', pdir)

if __name__ == '__main__':
    main()
