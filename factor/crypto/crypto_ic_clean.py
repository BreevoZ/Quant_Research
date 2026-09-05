#!/usr/bin/env python3
"""
Crypto Time-Series IC —— 干净版（聚焦分钟策略）

针对短 horizon (1m/5m/15m)：
  - 非重叠采样：horizon=k 时，每天每 k 分钟取一个点 → 相邻未来收益不重叠
  - skip-1：未来收益从 t+1 开始，隔离因子用的 close[t]，切断 bid-ask bounce 共享端点

并排对照：
  - raw   : 不跳 + 全采样（重叠）   = 旧口径，含污染
  - clean : skip-1 + 非重叠采样      = 真实可交易口径

未来收益用 ret_1m 重建对数累加（秩相关对单调变换不变）：
  fwd_clean_k[t] = log(close[t+1+k]/close[t+1]) = S[t+1+k] - S[t+1]
  fwd_raw_k[t]   = log(close[t+k]  /close[t])   = S[t+k]   - S[t]
  其中 S = cumsum(log(1+ret_1m))

输出:
  /home/yhzhou/factor/crypto_ic/ic_clean_detail.csv   每(coin,factor,horizon,variant)汇总
  控制台: raw vs clean 对照表
"""

import os, glob
import numpy as np
import pandas as pd
from qr import paths

FEAT_ROOT = str(paths.get('crypto_feat'))
OUT_ROOT  = str(paths.get('crypto_ic'))
os.makedirs(OUT_ROOT, exist_ok=True)

COINS = ['btc', 'eth', 'sol', 'xrp', 'ada', 'doge', 'link', 'ltc', 'avax']

FACTOR_COLS = [
    'ret_1m', 'ret_5m', 'ret_15m', 'ret_1h', 'ret_4h', 'ret_1d',
    'rv_1h', 'rv_4h', 'rv_1d',
    'range_pct', 'parkinson',
    'vol_ratio_1h', 'vol_ratio_1d', 'avg_trade_sz',
    'vwap_dev', 'price_pos_4h', 'vol_accel', 'amihud',
]

HORIZONS = [1, 5, 15]   # 分钟


def load_coin(coin: str) -> pd.DataFrame:
    files = sorted(glob.glob(f'{FEAT_ROOT}/*/{coin}.csv'))
    dfs = []
    for f in files:
        date = os.path.basename(os.path.dirname(f))
        try:
            df = pd.read_csv(f, index_col='time')
            df['date'] = date
            dfs.append(df)
        except Exception:
            pass
    if not dfs:
        return pd.DataFrame()
    # 文件已按日期 glob 排序，文件内 time 升序 → concat 即全序列时间序
    return pd.concat(dfs, ignore_index=True)


def daily_ic(sub_fac: pd.DataFrame, fwd: pd.Series) -> pd.Series:
    """单日：因子矩阵 vs 未来收益，返回每个因子的 Spearman IC。"""
    valid = np.isfinite(fwd.values)
    if valid.sum() < 30:
        return pd.Series(dtype=float)
    f = sub_fac[valid]
    y = fwd[valid].rank(method='average')
    fr = f.rank(method='average', na_option='keep')
    return fr.corrwith(y, method='pearson')


def process_coin(coin: str) -> pd.DataFrame:
    df = load_coin(coin)
    if df.empty:
        print(f'  {coin}: no data'); return pd.DataFrame()

    factors = [c for c in FACTOR_COLS if c in df.columns]
    lr = np.log1p(df['ret_1m'].fillna(0.0))
    S  = lr.cumsum().values
    n  = len(S)

    out_rows = []
    for k in HORIZONS:
        # 未来对数收益（对全序列 shift，跨日连续）
        fwd_raw   = np.full(n, np.nan)
        fwd_clean = np.full(n, np.nan)
        # raw: S[t+k]-S[t]
        fwd_raw[:n-k]       = S[k:] - S[:n-k]
        # clean (skip-1): S[t+1+k]-S[t+1]
        if n > k+1:
            fwd_clean[:n-k-1] = S[k+1:] - S[1:n-k]

        df['_fwd_raw']   = fwd_raw
        df['_fwd_clean'] = fwd_clean

        # 按天分组
        for variant, fwd_col, subsample in [
            ('raw',   '_fwd_raw',   1),   # 重叠：全采样
            ('clean', '_fwd_clean', k),   # 非重叠：每 k 分钟一个点
        ]:
            # 收集每日IC
            day_ics = {f: [] for f in factors}
            for date, blk in df.groupby('date', sort=False):
                if subsample > 1:
                    blk = blk.iloc[::subsample]
                ic = daily_ic(blk[factors], blk[fwd_col])
                for f in factors:
                    v = ic.get(f, np.nan)
                    if pd.notna(v) and np.isfinite(v):
                        day_ics[f].append(v)

            for f in factors:
                arr = np.array(day_ics[f])
                if len(arr) < 50:
                    continue
                m, s = arr.mean(), arr.std(ddof=1)
                out_rows.append({
                    'coin': coin, 'factor': f, 'horizon': f'{k}m',
                    'variant': variant,
                    'ic_mean': m, 'ic_std': s, 'n_days': len(arr),
                    'icir': m / s if s > 0 else np.nan,
                    't_stat': m / (s / np.sqrt(len(arr))) if s > 0 else np.nan,
                })

    df.drop(columns=['_fwd_raw', '_fwd_clean'], inplace=True, errors='ignore')
    print(f'  {coin}: done ({len(factors)} factors x {len(HORIZONS)} hz x 2 variants)')
    return pd.DataFrame(out_rows)


def main():
    print('=== Crypto Clean IC (1m/5m/15m, skip-1, non-overlap) ===')
    parts = []
    for coin in COINS:
        r = process_coin(coin)
        if not r.empty:
            parts.append(r)
    detail = pd.concat(parts, ignore_index=True)
    detail.to_csv(f'{OUT_ROOT}/ic_clean_detail.csv', index=False, float_format='%.5f')
    print(f'\nSaved ic_clean_detail.csv ({len(detail):,} rows)\n')

    # 跨币聚合：对 ic_mean 取币间平均（每个币本身已是1270天均值）
    agg = (detail.groupby(['factor', 'horizon', 'variant'])
           .agg(ic_mean=('ic_mean', 'mean'),
                icir=('icir', 'mean'))
           .reset_index())

    # ── raw vs clean 对照（按 horizon）──
    for k in ['1m', '5m', '15m']:
        sub = agg[agg['horizon'] == k]
        piv_ic   = sub.pivot(index='factor', columns='variant', values='ic_mean')
        piv_icir = sub.pivot(index='factor', columns='variant', values='icir')
        tbl = pd.DataFrame({
            'IC_raw':    piv_ic.get('raw'),
            'IC_clean':  piv_ic.get('clean'),
            'ICIR_raw':  piv_icir.get('raw'),
            'ICIR_clean':piv_icir.get('clean'),
        })
        tbl['IC_shrink%'] = (1 - tbl['IC_clean'].abs() / tbl['IC_raw'].abs()) * 100
        tbl = tbl.reindex(tbl['ICIR_clean'].abs().sort_values(ascending=False).index)
        print(f'\n─── horizon = {k}  (raw vs clean, 9币平均) ───')
        print(tbl.to_string(float_format=lambda x: f'{x:7.4f}'))

    print(f'\nDone → {OUT_ROOT}/ic_clean_detail.csv')


if __name__ == '__main__':
    main()
