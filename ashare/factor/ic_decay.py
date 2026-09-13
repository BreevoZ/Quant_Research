#!/usr/bin/env python3
"""
IC 衰减分析：对 k=1,2,3,5,10,20 分钟前向累计收益，分别计算 Rank IC
衰减曲线越平缓，信号持续性越好；衰减越快，说明是纯高频信号。

前向 k 分钟收益：ret_k[t] = (mid[t+k] - mid[t]) / mid[t]
"""

import pandas as pd
import numpy as np
import os
import sys
from pathlib import Path
from _paths import feat_dir, res_dir

HORIZONS = [1, 2, 3, 5, 10, 20]

FACTOR_COLS = [
    'wmid_dev', 'imb1', 'imb2', 'imb5',
    'ofi', 'bid_slope', 'ask_slope',
]

# ─── 加载面板 ─────────────────────────────────────────────────────────────────

def load_panel(feature_dir: str, max_stocks: int = None) -> dict:
    """返回 panels['mid'] 和 panels[factor]，均为 (time × stock) DataFrame"""
    files = sorted(Path(feature_dir).glob('*.csv'))
    if max_stocks:
        files = files[:max_stocks]

    print(f"Loading {len(files)} stocks...")

    cols_needed = FACTOR_COLS + ['mid']
    panels = {col: {} for col in cols_needed}
    ref_index = None

    for fp in files:
        try:
            df = pd.read_csv(fp, index_col='time', usecols=['time'] + cols_needed)
        except Exception as e:
            print(f"  skip {fp.name}: {e}", file=sys.stderr)
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

# ─── 前向 k 分钟累计收益 ──────────────────────────────────────────────────────

def forward_return(mid: pd.DataFrame, k: int) -> pd.DataFrame:
    """ret_k[t] = (mid[t+k] - mid[t]) / mid[t]
    跨午休的窗口（t 在上午末 k 格内）置 NaN，避免90分钟午休被当成 k 分钟收益。
    """
    mid_fwd = mid.shift(-k)
    ret = (mid_fwd - mid) / mid
    # 找出下午第一个有效时间点索引位置，往前 k 行全部置 NaN
    idx = mid.index.tolist()
    try:
        pm_start = idx.index('13:01:00')
        for i in range(max(0, pm_start - k), pm_start):
            ret.loc[idx[i]] = np.nan
    except ValueError:
        pass
    return ret

# ─── Rank IC ─────────────────────────────────────────────────────────────────

def rank_ic(factor: pd.DataFrame, fwd_ret: pd.DataFrame) -> pd.Series:
    f_rank = factor.rank(axis=1)
    y_rank = fwd_ret.rank(axis=1)
    ic = f_rank.corrwith(y_rank, axis=1, method='pearson')
    n_valid = factor.notna() & fwd_ret.notna()
    ic[n_valid.sum(axis=1) < 10] = np.nan
    return ic

def ic_mean(ic: pd.Series) -> float:
    return ic.dropna().mean()

# ─── 主流程 ──────────────────────────────────────────────────────────────────

def main():
    date       = sys.argv[1] if len(sys.argv) > 1 else '20260611'
    max_stocks = int(sys.argv[2]) if len(sys.argv) > 2 else None
    out_dir    = res_dir(date)
    os.makedirs(out_dir, exist_ok=True)

    panels = load_panel(feat_dir(date), max_stocks)
    mid    = panels['mid']

    # 预计算所有 horizon 的前向收益
    fwd_rets = {k: forward_return(mid, k) for k in HORIZONS}

    # 对每个因子、每个 horizon 计算 IC 均值
    results = {}  # factor → {k: ic_mean}
    for col in FACTOR_COLS:
        factor = panels[col]
        row = {}
        for k in HORIZONS:
            ic_series = rank_ic(factor, fwd_rets[k])
            row[k] = ic_mean(ic_series)
        results[col] = row
        print(f"  {col} done")

    decay_df = pd.DataFrame(results, index=HORIZONS).T
    decay_df.columns = [f't+{k}' for k in HORIZONS]

    # 归一化：每行除以 t+1 的值，看相对衰减速度
    decay_norm = decay_df.div(decay_df['t+1'].abs(), axis=0)

    out_path = os.path.join(out_dir, 'ic_decay.csv')
    decay_df.to_csv(out_path, float_format='%.4f')

    print(f"\n{'='*60}")
    print("IC 衰减表（绝对值）：")
    print(decay_df.to_string(float_format='{:.4f}'.format))

    print(f"\nIC 衰减表（相对 t+1 归一化，100%=完全保持）：")
    print((decay_norm * 100).to_string(float_format='{:.1f}%'.format))

    print(f"\nSaved: {out_path}")

if __name__ == '__main__':
    main()
