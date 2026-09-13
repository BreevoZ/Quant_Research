#!/usr/bin/env python3
"""
衰减曲线实验：区分 "skip-1 杀 bounce" vs "真信号因位移衰减"

对每个因子，固定目标为"1分钟收益"，只移动它离因子的距离 d：
    target_d[t] = log(close[t+d+1] / close[t+d])   # 始终是1分钟窗口
    factor = ret_1m[t] 等

= 因子与"d分钟后那一分钟收益"的 Spearman = 类自相关函数。

判读：
  - Bounce（微观结构假象）→ d=0 尖峰，d>=1 悬崖式掉到~0
  - 真实反转信号          → 随 d 缓慢衰减（缓坡），d=1 仍显著
"""
import os, glob
import numpy as np
import pandas as pd
from qr import paths

FEAT_ROOT = str(paths.get('crypto_feat'))
COINS = ['btc', 'eth', 'sol', 'xrp', 'ada', 'doge', 'link', 'ltc', 'avax']
FACTORS = ['ret_1m', 'vwap_dev', 'ret_5m', 'ret_15m', 'ret_1d']
DISPLACE = [0, 1, 2, 3, 5, 10]   # 目标离因子多少分钟


def load_coin(coin):
    files = sorted(glob.glob(f'{FEAT_ROOT}/*/{coin}.csv'))
    dfs = []
    for f in files:
        date = os.path.basename(os.path.dirname(f))
        try:
            df = pd.read_csv(f, index_col='time'); df['date'] = date; dfs.append(df)
        except Exception:
            pass
    return pd.concat(dfs, ignore_index=True) if dfs else pd.DataFrame()


def daily_ic(fac_df, fwd):
    valid = np.isfinite(fwd.values)
    if valid.sum() < 30:
        return pd.Series(dtype=float)
    y = fwd[valid].rank()
    fr = fac_df[valid].rank(na_option='keep')
    return fr.corrwith(y, method='pearson')


def main():
    # 累加器: {(factor,d): [daily ics across coins]}
    acc = {(f, d): [] for f in FACTORS for d in DISPLACE}

    for coin in COINS:
        df = load_coin(coin)
        if df.empty:
            continue
        facs = [f for f in FACTORS if f in df.columns]
        lr = np.log1p(df['ret_1m'].fillna(0.0))
        S = lr.cumsum().values
        n = len(S)

        # 每个 d 一个目标列：1分钟收益 = S[t+d+1]-S[t+d]
        for d in DISPLACE:
            # target_d[t] = S[t+d+1] - S[t+d]  (1分钟收益，位于 t+d 处)
            tgt = np.full(n, np.nan)
            hi = n - d - 1
            if hi > 0:
                tgt[:hi] = S[d+1:d+1+hi] - S[d:d+hi]
            df['_t'] = tgt
            for date, blk in df.groupby('date', sort=False):
                ic = daily_ic(blk[facs], blk['_t'])
                for f in facs:
                    v = ic.get(f, np.nan)
                    if pd.notna(v) and np.isfinite(v):
                        acc[(f, d)].append(v)
        print(f'  {coin} done')

    # 汇总成表：行=因子，列=d
    print('\n=== 衰减曲线：因子 vs "d分钟后的1分钟收益" 的平均IC ===')
    print('（d=0 含共享端点；d>=1 已隔离 close[t]）\n')
    header = 'factor      ' + ''.join(f'  d={d:<5}' for d in DISPLACE)
    print(header)
    print('-' * len(header))
    rows = {}
    for f in FACTORS:
        vals = []
        for d in DISPLACE:
            arr = np.array(acc[(f, d)])
            vals.append(arr.mean() if len(arr) else np.nan)
        rows[f] = vals
        cells = ''.join(f'  {v:+7.4f}' for v in vals)
        print(f'{f:<10}{cells}')

    # 诊断：d=0 相对 d=1 的"尖峰" vs d=1 之后的"坡度"
    print('\n=== 诊断 ===')
    for f in FACTORS:
        v = rows[f]
        spike = v[0] - v[1]                       # d0 比 d1 多出来的（=bounce尖峰）
        plateau = v[1]                            # d=1 隔离后的水平（=真信号起点）
        slope = v[1] - v[3] if len(v) > 3 else np.nan  # d1->d3 的缓慢衰减
        kind = 'BOUNCE(悬崖)' if abs(plateau) < 0.3*abs(v[0]) else '真信号(缓坡)'
        print(f'  {f:<10} 尖峰(d0-d1)={spike:+.4f}  隔离后水平(d1)={plateau:+.4f}  '
              f'd1→d3坡度={slope:+.4f}  → {kind}')


if __name__ == '__main__':
    main()
