#!/usr/bin/env python3
"""
LOB 特征提取：从 Lob_new/ 的 H5 文件中提取截面因子特征
输出：每支股票一个 CSV，行是分钟级时间点，列是特征
"""

import h5py
import numpy as np
import pandas as pd
import os
import sys
from pathlib import Path
from qr import paths

# ─── 时间常量 ─────────────────────────────────────────────────────────────────

# 用一个固定日期做 datetime，方便 resample 对齐整分钟
_EPOCH = pd.Timestamp('2000-01-01')

def _td_to_dt(td_index):
    """timedelta index → datetime index（加上固定 epoch）"""
    return _EPOCH + td_index

def _dt_to_str(dt_index):
    """datetime index → HH:MM:SS 字符串"""
    return dt_index.strftime('%H:%M:%S')

# 连续竞价分钟格点（右端，即窗口 [t-1min, t] 的 t）
AM = pd.date_range('2000-01-01 09:31', '2000-01-01 11:30', freq='1min')
PM = pd.date_range('2000-01-01 13:01', '2000-01-01 15:00', freq='1min')
MINUTE_GRID = AM.append(PM)  # DatetimeIndex, 240 个点

# ─── 读取 H5 → DataFrame ─────────────────────────────────────────────────────

def load_h5(path: str) -> pd.DataFrame:
    with h5py.File(path, 'r') as f:
        data = f['order'][:]

    df = pd.DataFrame({
        'time':      data['time'].astype(str),
        'ordertype': data['ordertype'].astype(str),
        'direction': data['direction'].astype(str),
        'price':     data['price'].astype(float),
        'volume':    data['volume'].astype(float),
    })
    for i in range(1, 11):
        df[f'bdp{i}'] = data[f'bdp{i}'].astype(float)
        df[f'bdv{i}'] = data[f'bdv{i}'].astype(float)
        df[f'akp{i}'] = data[f'akp{i}'].astype(float)
        df[f'akv{i}'] = data[f'akv{i}'].astype(float)

    # 时间字符串 → datetime：把 HH:MM:SS.mmm 转成 epoch + timedelta（全向量化）
    raw = data['time']
    arr = np.frombuffer(raw.tobytes(), dtype=np.uint8).reshape(len(raw), 16)
    H  = (arr[:, 0].astype(np.int64) - 48) * 10 + (arr[:, 1].astype(np.int64) - 48)
    M  = (arr[:, 3].astype(np.int64) - 48) * 10 + (arr[:, 4].astype(np.int64) - 48)
    S  = (arr[:, 6].astype(np.int64) - 48) * 10 + (arr[:, 7].astype(np.int64) - 48)
    ms = ((arr[:, 9].astype(np.int64) - 48) * 100 +
          (arr[:, 10].astype(np.int64) - 48) * 10 +
          (arr[:, 11].astype(np.int64) - 48))
    ns = (H * 3600 + M * 60 + S) * 10**9 + ms * 10**6
    df['dt'] = pd.to_datetime(ns + _EPOCH.value, unit='ns')
    return df

# ─── 盘口快照采样 ─────────────────────────────────────────────────────────────

def sample_snapshots(df: pd.DataFrame) -> pd.DataFrame:
    """
    取每分钟窗口 (t-1min, t] 内最后一条记录作为盘口快照。
    返回以 MINUTE_GRID 为 index 的 DataFrame，缺失时用前值填充。
    """
    lob_cols = (
        [f'bdp{i}' for i in range(1, 11)] +
        [f'bdv{i}' for i in range(1, 11)] +
        [f'akp{i}' for i in range(1, 11)] +
        [f'akv{i}' for i in range(1, 11)] +
        ['price']
    )
    # 只用 wt（委托单）记录作为盘口快照：
    # zb（成交）记录的 akp1/akv1 存的是"刚撮合的对手方"而非真实挂单，会造成 bid==ask 的假象
    wt_only = df[df['ordertype'] == 'wt']
    snap = (wt_only.set_index('dt')[lob_cols]
              .resample('1min', label='right', closed='right', origin='epoch')
              .last()
              .reindex(MINUTE_GRID)
              .ffill())
    return snap

# ─── 特征计算 ─────────────────────────────────────────────────────────────────

def compute_features(snap: pd.DataFrame, df: pd.DataFrame) -> pd.DataFrame:
    bid1 = snap['bdp1'];  ask1 = snap['akp1']
    bv1  = snap['bdv1'];  av1  = snap['akv1']
    valid = (bid1 > 0) & (ask1 > 0)

    feat = pd.DataFrame(index=snap.index)

    # ── 基础盘口 ──
    feat['mid']        = ((bid1 + ask1) / 2).where(valid)
    feat['spread']     = (ask1 - bid1).where(valid)
    feat['rel_spread'] = (feat['spread'] / feat['mid']).where(valid)

    # 加权中间价（Micro Price）
    denom = bv1 + av1
    feat['wmid']     = ((bid1 * av1 + ask1 * bv1) / denom).where(valid & (denom > 0))
    feat['wmid_dev'] = feat['wmid'] - feat['mid']

    # ── 深度不平衡 ──
    def imb(level):
        b = snap[f'bdv{level}'];  a = snap[f'akv{level}']
        has_quote = (snap[f'bdp{level}'] > 0) & (snap[f'akp{level}'] > 0)
        return ((b - a) / (b + a)).where(has_quote & ((b + a) > 0))

    feat['imb1'] = imb(1)
    feat['imb2'] = imb(2)
    feat['imb3'] = imb(3)

    bv5 = sum(snap[f'bdv{i}'] for i in range(1, 6))
    av5 = sum(snap[f'akv{i}'] for i in range(1, 6))
    feat['imb5'] = ((bv5 - av5) / (bv5 + av5)).where((bv5 + av5) > 0)

    # ── OFI（成交方向不平衡），按分钟聚合 ──
    trades = df[df['ordertype'] == 'zb'].set_index('dt').sort_index()
    vol    = trades['volume']
    buy_vol  = vol.where(trades['direction'] == 'B', 0)
    sell_vol = vol.where(trades['direction'] == 'S', 0)

    resample_kw = dict(label='right', closed='right', origin='epoch')
    buy_sum  = buy_vol.resample('1min',  **resample_kw).sum().reindex(MINUTE_GRID, fill_value=0)
    sell_sum = sell_vol.resample('1min', **resample_kw).sum().reindex(MINUTE_GRID, fill_value=0)
    tot_sum  = vol.resample('1min',      **resample_kw).sum().reindex(MINUTE_GRID, fill_value=0)

    feat['ofi'] = ((buy_sum - sell_sum) / tot_sum).where(tot_sum > 0)

    # ── 1分钟中间价收益率 ──
    # 午休边界（下午第一格）是上下午的拼接点，pct_change 会算出~90分钟的跨午休收益，置 NaN
    feat['ret1'] = feat['mid'].pct_change()
    feat.iloc[120, feat.columns.get_loc('ret1')] = np.nan  # MINUTE_GRID[120] = 13:01:00

    # ── 买卖盘深度斜率（1档 vs 5档量的比） ──
    feat['bid_slope'] = (bv1 / snap['bdv5']).where(snap['bdv5'] > 0)
    feat['ask_slope'] = (av1 / snap['akv5']).where(snap['akv5'] > 0)

    feat['bid1_vol'] = bv1
    feat['ask1_vol'] = av1

    return feat

# ─── 主流程 ──────────────────────────────────────────────────────────────────

def process_one(h5_path: str, out_dir: str) -> str:
    symbol = Path(h5_path).stem
    df     = load_h5(h5_path)
    snap   = sample_snapshots(df)
    feat   = compute_features(snap, df)

    feat.index = _dt_to_str(feat.index)
    feat.index.name = 'time'

    out_path = os.path.join(out_dir, f'{symbol}.csv')
    feat.to_csv(out_path, float_format='%.6f')
    return out_path

def _worker(args):
    h5_path, out_dir = args
    try:
        return process_one(h5_path, out_dir), None
    except Exception as e:
        return None, (h5_path, str(e))

def process_date(date: str, lob_root: str, out_root: str,
                 max_stocks: int = None, workers: int = 8):
    import multiprocessing as mp

    lob_dir = os.path.join(lob_root, date)
    out_dir = os.path.join(out_root, date)
    os.makedirs(out_dir, exist_ok=True)

    h5_files = sorted(Path(lob_dir).glob('*.h5'))
    if max_stocks:
        h5_files = h5_files[:max_stocks]

    total = len(h5_files)
    print(f"Processing {total} stocks for {date} → {out_dir}  (workers={workers})")

    args = [(str(p), out_dir) for p in h5_files]
    done = 0; errors = []
    with mp.Pool(workers) as pool:
        for out, err in pool.imap_unordered(_worker, args, chunksize=4):
            done += 1
            if err:
                errors.append(err)
                print(f"  ERROR {Path(err[0]).name}: {err[1]}", file=sys.stderr)
            elif done % 500 == 0 or done == 1:
                print(f"  [{done}/{total}] {out}")

    print(f"Done. {done-len(errors)}/{total} ok, {len(errors)} errors.")

if __name__ == '__main__':
    LOB_ROOT = str(paths.get('lob_root'))
    OUT_ROOT = str(paths.get('feat_root'))
    date       = sys.argv[1] if len(sys.argv) > 1 else '20260611'
    max_stocks = int(sys.argv[2]) if len(sys.argv) > 2 else None
    process_date(date, LOB_ROOT, OUT_ROOT, max_stocks)
