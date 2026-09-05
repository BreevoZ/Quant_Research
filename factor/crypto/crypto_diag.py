#!/usr/bin/env python3
"""
诊断：为什么 ret_1d 的 IC 强(负)，策略却零费用亏钱？

按 ret_1d 分10档，看每档【未来15分钟收益的均值】(PnL关心) vs 秩(IC关心)。
分两种未来收益：
  raw      : 原始（含市场）
  demean   : 每期跨币去均值（市场中性，回测用的口径）
若 raw 单调反转、demean 变平/翻转 → 反转是市场级的，中性化杀掉了。
若两者均值都不单调（尽管秩相关为负）→ 秩≠幅度，大波动是动量。
"""
import os, glob
import numpy as np
import pandas as pd
from qr import paths

FEAT_ROOT = str(paths.get('crypto_feat'))
COINS = ['btc', 'eth', 'sol', 'xrp', 'ada', 'doge', 'link', 'ltc', 'avax']
HOLD = 15


def load_coin(coin):
    files = sorted(glob.glob(f'{FEAT_ROOT}/*/{coin}.csv'))
    dfs = []
    for f in files:
        date = os.path.basename(os.path.dirname(f))
        try:
            df = pd.read_csv(f, usecols=['time', 'ret_1d', 'ret_1m']); df['date'] = date
            dfs.append(df)
        except Exception:
            pass
    return pd.concat(dfs, ignore_index=True) if dfs else pd.DataFrame()


def main():
    recs = []
    for coin in COINS:
        df = load_coin(coin)
        if df.empty:
            continue
        mins = df['time'].str.slice(0, 2).astype(int) * 60 + df['time'].str.slice(3, 5).astype(int)
        S = np.log1p(df['ret_1m'].fillna(0.0)).cumsum().values
        n = len(S); fwd = np.full(n, np.nan); hi = n - HOLD - 1
        if hi > 0:
            fwd[:hi] = S[HOLD+1:HOLD+1+hi] - S[1:1+hi]
        gts = df['date'].values + df['time'].values
        grid = (mins.values % HOLD == 0); idx = np.where(grid)[0]
        recs.append(pd.DataFrame({
            'gts': gts[idx], 'coin': coin,
            'sig': df['ret_1d'].values[idx],
            'fwd': np.expm1(fwd[idx]),
        }))
    d = pd.concat(recs, ignore_index=True).dropna(subset=['sig', 'fwd'])

    # 每期跨币去均值
    d['fwd_dm'] = d['fwd'] - d.groupby('gts')['fwd'].transform('mean')

    # 池化 Spearman（与IC脚本核对符号）= rank 后 pearson，避开 scipy
    sr = d['sig'].rank()
    ic_raw = sr.corr(d['fwd'].rank())
    ic_dm  = sr.corr(d['fwd_dm'].rank())
    print(f'池化 Spearman IC:  raw={ic_raw:+.4f}   demean={ic_dm:+.4f}\n')

    # 10档
    d['q'] = pd.qcut(d['sig'], 10, labels=False, duplicates='drop')
    g = d.groupby('q').agg(
        sig_mean=('sig', 'mean'),
        fwd_raw_bp=('fwd', lambda x: x.mean() * 1e4),
        fwd_dm_bp=('fwd_dm', lambda x: x.mean() * 1e4),
        n=('fwd', 'size'),
    )
    print('档(0=ret_1d最低/最超卖 → 9=最高/最超买)')
    print(f'{"q":>3} {"ret_1d均值":>11} {"未来raw(bp)":>13} {"未来demean(bp)":>15} {"样本":>10}')
    for q, r in g.iterrows():
        print(f'{int(q):>3} {r.sig_mean:>+11.4f} {r.fwd_raw_bp:>+13.2f} '
              f'{r.fwd_dm_bp:>+15.2f} {int(r.n):>10,}')

    # 多空价差（最超卖Q0 - 最超买Q9）：反转应为正
    ls_raw = g.loc[0, 'fwd_raw_bp'] - g.loc[g.index.max(), 'fwd_raw_bp']
    ls_dm  = g.loc[0, 'fwd_dm_bp']  - g.loc[g.index.max(), 'fwd_dm_bp']
    print(f'\n多空价差 Q0-Q9 (反转为正):  raw={ls_raw:+.2f}bp   demean={ls_dm:+.2f}bp  /每15min')

    # 单调性：用秩相关看档均值是否单调
    mono_raw = pd.Series(g['fwd_raw_bp'].values).rank().corr(pd.Series(g.index).rank())
    mono_dm  = pd.Series(g['fwd_dm_bp'].values).rank().corr(pd.Series(g.index).rank())
    print(f'档均值单调性(档号 vs 未来均值, 反转应为负): raw={mono_raw:+.2f}  demean={mono_dm:+.2f}')


if __name__ == '__main__':
    main()
