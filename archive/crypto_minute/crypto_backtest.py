#!/usr/bin/env python3
"""
ret_1d 反转因子 —— 截面 dollar-neutral 多空回测

口径（真实可成交）：
  - close[t] 出信号 ret_1d[t]
  - t+1 进场（skip-1，隔离 close[t] 的 bounce）
  - 持有 HOLD 分钟，下个网格点换仓（非重叠）
  - 截面：每期把币按 ret_1d 排序，做多最低 1/3（最超卖），做空最高 1/3（最超买）
  - dollar-neutral，gross 杠杆 = 2（多腿+1，空腿-1）

收益用 ret_1m 重建：fwd[t] = log(close[t+1+HOLD]/close[t+1])，转简单收益做组合。
手续费按换手 sum|ΔW| × fee 扣（双边已含）。
"""
import os, glob
import numpy as np
import pandas as pd
from qr import paths

FEAT_ROOT = str(paths.get('crypto_feat'))
COINS = ['btc', 'eth', 'sol', 'xrp', 'ada', 'doge', 'link', 'ltc', 'avax']
FEES = [0.0, 0.0001, 0.00025, 0.0005, 0.0010]   # 每单位换手成本（0 / 1bp / 2.5bp / 5bp / 10bp）


def load_coin(coin):
    files = sorted(glob.glob(f'{FEAT_ROOT}/*/{coin}.csv'))
    dfs = []
    for f in files:
        date = os.path.basename(os.path.dirname(f))
        try:
            df = pd.read_csv(f, usecols=['time', 'ret_1d', 'ret_1m'])
            df['date'] = date
            dfs.append(df)
        except Exception:
            pass
    return pd.concat(dfs, ignore_index=True) if dfs else pd.DataFrame()


def build_panel(hold):
    sig_list, fwd_list = {}, {}
    for coin in COINS:
        df = load_coin(coin)
        if df.empty:
            continue
        mins = df['time'].str.slice(0, 2).astype(int) * 60 + df['time'].str.slice(3, 5).astype(int)
        S = np.log1p(df['ret_1m'].fillna(0.0)).cumsum().values
        n = len(S)
        fwd = np.full(n, np.nan)
        hi = n - hold - 1
        if hi > 0:
            fwd[:hi] = S[hold+1:hold+1+hi] - S[1:1+hi]   # S[t+1+hold]-S[t+1]
        gts = df['date'].values + df['time'].values     # 全局时间键（可排序）
        grid = (mins.values % hold == 0)
        idx = np.where(grid)[0]
        sig_list[coin] = pd.Series(df['ret_1d'].values[idx], index=gts[idx])
        fwd_list[coin] = pd.Series(fwd[idx],                 index=gts[idx])
    sig = pd.DataFrame(sig_list).sort_index()
    fwd = pd.DataFrame(fwd_list).sort_index()
    return sig, fwd


def backtest(hold):
    sig, fwd = build_panel(hold)
    coins = sig.columns.tolist()
    Sg = sig.values
    Fw = np.expm1(fwd.values)   # 简单收益
    T, C = Sg.shape

    W = np.zeros((T, C))
    for i in range(T):
        s = Sg[i]; valid = np.isfinite(s) & np.isfinite(Fw[i])
        k = valid.sum()
        if k < 4:
            continue
        order = np.argsort(np.where(valid, s, np.inf))   # 升序，超卖在前
        nsel = max(1, k // 3)
        longs = order[:nsel]; shorts = order[k-nsel:k]
        W[i, longs]  = 1.0 / nsel
        W[i, shorts] = -1.0 / nsel

    Fw0 = np.nan_to_num(Fw)
    gross = (W * Fw0).sum(axis=1)                          # 每期毛收益
    turnover = np.abs(np.diff(W, axis=0, prepend=0)).sum(axis=1)
    active = (np.abs(W).sum(axis=1) > 0)

    ppy = 365 * 24 * 60 / hold                             # 每年期数
    print(f'\n{"="*64}\n  HOLD={hold}min   期数={active.sum():,}   每年≈{ppy:.0f}期   '
          f'平均换手={turnover[active].mean():.2f}\n{"="*64}')
    print(f'{"fee/side":>10} {"年化收益":>10} {"年化波动":>10} {"Sharpe":>8} '
          f'{"累计收益":>10} {"胜率":>7}')
    for fee in FEES:
        net = gross - fee * turnover
        na = net[active]
        ann_ret = na.mean() * ppy
        ann_vol = na.std() * np.sqrt(ppy)
        sharpe  = ann_ret / ann_vol if ann_vol > 0 else 0
        cum     = np.prod(1 + na) - 1
        win     = (na > 0).mean()
        print(f'{fee*1e4:>8.1f}bp {ann_ret*100:>9.2f}% {ann_vol*100:>9.2f}% '
              f'{sharpe:>8.2f} {cum*100:>9.1f}% {win*100:>6.1f}%')

    # 毛收益的盈亏平衡手续费
    g = gross[active]; tov = turnover[active]
    be = g.mean() / tov.mean() if tov.mean() > 0 else 0
    print(f'  盈亏平衡 fee/side ≈ {be*1e4:.2f}bp  （>此值则净亏）')


def main():
    print('=== ret_1d 截面反转 多空回测 (2023-2026, 9币) ===')
    for hold in [15, 60]:
        backtest(hold)


if __name__ == '__main__':
    main()
