#!/usr/bin/env python3
"""
Crypto Time-Series IC Analysis

对每个币种，计算各因子与未来k分钟收益的 Spearman Rank IC。
时间序列IC：在单币的时间维度上做相关，不跨品种。

指标：
  - 每日IC：每天1440分钟内算IC → IC均值/std → ICIR
  - IC Decay：不同horizon下IC的衰减

输出：
  <crypto_ic>/ic_daily.csv    每日IC流水（date, coin, factor, horizon, ic）
  <crypto_ic>/ic_summary.csv  汇总（mean, std, ICIR, t-stat）
  crypto_ic 是 qr.toml 里的键，默认 data/factor/crypto_ic
"""

import os, glob, sys
import numpy as np
import pandas as pd
from qr import paths

FEAT_ROOT = str(paths.get('crypto_feat'))
OUT_ROOT  = str(paths.get('crypto_ic'))
os.makedirs(OUT_ROOT, exist_ok=True)

# 跳过 BNB（只有2026年3月起，样本太少）
COINS = ['btc', 'eth', 'sol', 'xrp', 'ada', 'doge', 'link', 'ltc', 'avax']

FACTOR_COLS = [
    'ret_1m', 'ret_5m', 'ret_15m', 'ret_1h', 'ret_4h', 'ret_1d',
    'rv_1h', 'rv_4h', 'rv_1d',
    'range_pct', 'parkinson',
    'vol_ratio_1h', 'vol_ratio_1d', 'avg_trade_sz',
    'vwap_dev', 'price_pos_4h', 'vol_accel', 'amihud',
]

# (horizon_label, shift_lag, source_ret_col)
# fwd return at t = source_ret_col at t+lag  (因为 ret_kh[i] = (close[i]/close[i-k])-1)
HORIZONS = [
    ('1m',   1,    'ret_1m'),
    ('5m',   5,    'ret_5m'),
    ('15m',  15,   'ret_15m'),
    ('1h',   60,   'ret_1h'),
    ('4h',   240,  'ret_4h'),
    ('1d',   1440, 'ret_1d'),
]


def load_coin(coin: str) -> pd.DataFrame:
    files = sorted(glob.glob(f'{FEAT_ROOT}/*/{coin}.csv'))
    if not files:
        return pd.DataFrame()
    dfs = []
    for f in files:
        date = os.path.basename(os.path.dirname(f))  # 20230102
        try:
            df = pd.read_csv(f, index_col='time')
            df['date'] = date
            dfs.append(df)
        except Exception:
            pass
    if not dfs:
        return pd.DataFrame()
    full = pd.concat(dfs, ignore_index=True)
    return full


def compute_daily_ic(coin: str) -> pd.DataFrame:
    df = load_coin(coin)
    if df.empty:
        print(f'  {coin}: no data')
        return pd.DataFrame()

    factors = [c for c in FACTOR_COLS if c in df.columns]
    n_rows = len(df)

    # 在全序列上做 shift（crypto 24/7，跨日连续，不需要断开）
    fwd_dict = {}
    for hz_label, lag, ret_col in HORIZONS:
        if ret_col in df.columns:
            fwd_dict[hz_label] = df[ret_col].shift(-lag).values

    rows = []
    dates = df.groupby('date').groups

    for date, idx in dates.items():
        idx = list(idx)
        sub = df.iloc[idx]
        fac_data = sub[factors]

        for hz_label in fwd_dict:
            fwd = fwd_dict[hz_label][idx]
            valid_fwd = np.isfinite(fwd)

            if valid_fwd.sum() < 30:
                continue

            fac_valid = fac_data[valid_fwd]
            fwd_valid_s = pd.Series(fwd[valid_fwd], index=fac_valid.index)

            if len(fac_valid) < 30:
                continue

            # Rank 前向收益
            fwd_ranked = fwd_valid_s.rank(method='average')

            # 对每个因子做 rank，然后与 fwd_ranked 做 Pearson（= Spearman）
            # corrwith 向量化计算所有因子
            fac_ranked = fac_valid.rank(method='average', na_option='keep')
            ic_vec = fac_ranked.corrwith(fwd_ranked, method='pearson')

            for fac, ic in ic_vec.items():
                if pd.notna(ic) and np.isfinite(ic):
                    rows.append({
                        'date':    date,
                        'coin':    coin,
                        'factor':  fac,
                        'horizon': hz_label,
                        'ic':      round(ic, 5),
                    })

    print(f'  {coin}: {n_rows:,} rows, {len(dates)} days → {len(rows):,} IC values')
    return pd.DataFrame(rows)


def main():
    all_daily = []

    print('=== Crypto Time-Series IC Analysis ===')
    for coin in COINS:
        df_ic = compute_daily_ic(coin)
        if not df_ic.empty:
            all_daily.append(df_ic)

    if not all_daily:
        print('No data.')
        return

    df_daily = pd.concat(all_daily, ignore_index=True)
    df_daily.to_csv(f'{OUT_ROOT}/ic_daily.csv', index=False, float_format='%.5f')
    print(f'\nSaved ic_daily.csv  ({len(df_daily):,} rows)')

    # ── Summary: mean IC, std, ICIR, t-stat ──────────────────────────────────
    stats = (df_daily.groupby(['coin', 'factor', 'horizon'])['ic']
             .agg(ic_mean='mean', ic_std='std', n_days='count')
             .reset_index())
    stats['icir']   = stats['ic_mean'] / stats['ic_std']
    stats['t_stat'] = stats['ic_mean'] / (stats['ic_std'] / np.sqrt(stats['n_days']))

    stats.sort_values('icir', key=abs, ascending=False, inplace=True)
    stats.to_csv(f'{OUT_ROOT}/ic_summary.csv', index=False, float_format='%.4f')
    print(f'Saved ic_summary.csv  ({len(stats):,} rows)')

    # ── 打印 Top 因子 (1h horizon) ─────────────────────────────────────────────
    print('\n─── Top 20 factors by |ICIR|, horizon=1h ───')
    top_1h = (stats[stats['horizon'] == '1h']
              .sort_values('icir', key=abs, ascending=False)
              .head(20))
    print(top_1h[['coin','factor','ic_mean','ic_std','icir','t_stat','n_days']].to_string(index=False))

    # ── IC Decay 表（各因子在不同 horizon 下的平均IC）─────────────────────────
    print('\n─── IC Decay (avg across all coins, top-10 factors at 1h) ───')
    avg_ic = (df_daily.groupby(['factor', 'horizon'])['ic']
              .mean().reset_index())
    decay = avg_ic.pivot(index='factor', columns='horizon', values='ic')
    hz_order = [h for h, _, _ in HORIZONS if h in decay.columns]
    decay = decay[hz_order]
    top_facs = decay['1h'].abs().sort_values(ascending=False).head(10).index
    print(decay.loc[top_facs].to_string(float_format='%.4f'))

    # ── 按年份分段（bull/bear breakdown）──────────────────────────────────────
    print('\n─── Annual IC breakdown (avg |IC|, horizon=1h) ───')
    df_1h = df_daily[df_daily['horizon'] == '1h'].copy()
    df_1h['year'] = df_1h['date'].str[:4]
    annual = (df_1h.groupby(['year', 'factor'])['ic']
              .mean().abs().unstack('factor'))
    # top factors by overall |IC|
    top_facs_1h = decay['1h'].abs().sort_values(ascending=False).head(8).index
    print(annual[[f for f in top_facs_1h if f in annual.columns]].to_string(float_format='%.4f'))

    print(f'\nDone → {OUT_ROOT}/')


if __name__ == '__main__':
    main()
