#!/usr/bin/env python3
"""
ret_1d 反转 —— 时间序列(TS)信号的市场中性回测

与截面版的区别：每个币先对【自己的历史】标准化，再跨币去均值做中性。
这才忠实于我们验证的 time-series IC（币相对自己均值回归）。

  z_i,t = (ret_1d_i,t - 自己trailing均值) / 自己trailing std   # 只用过去数据
  w_i,t = -(z_i,t - 跨币均值)                                   # 反转 + 市场中性
  归一化到 gross=2，pnl = Σ w_i * fwd_i
口径同前：t+1 进场(skip-1)，持有 HOLD 分钟，非重叠换仓。
"""
import os, glob
import numpy as np
import pandas as pd
from qr import paths

FEAT_ROOT = str(paths.get('crypto_feat'))
COINS = ['btc', 'eth', 'sol', 'xrp', 'ada', 'doge', 'link', 'ltc', 'avax']
FEES = [0.0, 0.0001, 0.00025, 0.0005, 0.0010]


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


def build_panel(hold):
    sig_list, fwd_list = {}, {}
    for coin in COINS:
        df = load_coin(coin)
        if df.empty:
            continue
        mins = df['time'].str.slice(0, 2).astype(int) * 60 + df['time'].str.slice(3, 5).astype(int)
        S = np.log1p(df['ret_1m'].fillna(0.0)).cumsum().values
        n = len(S); fwd = np.full(n, np.nan); hi = n - hold - 1
        if hi > 0:
            fwd[:hi] = S[hold+1:hold+1+hi] - S[1:1+hi]
        gts = df['date'].values + df['time'].values
        grid = (mins.values % hold == 0); idx = np.where(grid)[0]
        sig_list[coin] = pd.Series(df['ret_1d'].values[idx], index=gts[idx])
        fwd_list[coin] = pd.Series(fwd[idx],                 index=gts[idx])
    return pd.DataFrame(sig_list).sort_index(), pd.DataFrame(fwd_list).sort_index()


def backtest(hold):
    sig, fwd = build_panel(hold)
    ppg = int(round(24 * 60 / hold))     # 每天网格点数
    win = 30 * ppg                       # 30天 trailing 窗口
    mp  = 5 * ppg                        # 至少5天才出仓位

    # 每币对自己历史滚动 z-score（trailing，shift(1) 防未来函数）
    mu = sig.rolling(win, min_periods=mp).mean().shift(1)
    sd = sig.rolling(win, min_periods=mp).std().shift(1)
    Z  = (sig - mu) / sd

    Zv = Z.values; Fw = np.expm1(fwd.values)
    T, C = Zv.shape
    W = np.zeros((T, C))
    for i in range(T):
        z = Zv[i]; valid = np.isfinite(z) & np.isfinite(Fw[i])
        if valid.sum() < 4:
            continue
        zc = z.copy(); zc[~valid] = np.nan
        zc = zc - np.nanmean(zc)              # 跨币去均值 → 市场中性
        w = -np.nan_to_num(zc)                # 反转
        g = np.abs(w).sum()
        if g > 0:
            W[i] = w / g * 2.0                # 归一化 gross=2

    Fw0 = np.nan_to_num(Fw)
    gross = (W * Fw0).sum(axis=1)
    turnover = np.abs(np.diff(W, axis=0, prepend=0)).sum(axis=1)
    active = (np.abs(W).sum(axis=1) > 1e-9)

    ppy = 365 * 24 * 60 / hold
    print(f'\n{"="*64}\n  TS市场中性  HOLD={hold}min  期数={active.sum():,}  '
          f'每年≈{ppy:.0f}期  平均换手={turnover[active].mean():.3f}\n{"="*64}')
    print(f'{"fee/side":>10} {"年化收益":>10} {"年化波动":>10} {"Sharpe":>8} {"累计":>10} {"胜率":>7}')
    for fee in FEES:
        net = (gross - fee * turnover)[active]
        ar = net.mean() * ppy; av = net.std() * np.sqrt(ppy)
        sh = ar / av if av > 0 else 0; cum = np.prod(1 + net) - 1; win_r = (net > 0).mean()
        print(f'{fee*1e4:>8.1f}bp {ar*100:>9.2f}% {av*100:>9.2f}% {sh:>8.2f} '
              f'{cum*100:>9.1f}% {win_r*100:>6.1f}%')
    g = gross[active]; tov = turnover[active]
    be = g.mean() / tov.mean() if tov.mean() > 0 else 0
    print(f'  盈亏平衡 fee/side ≈ {be*1e4:.2f}bp')


def main():
    print('=== ret_1d 时序反转 市场中性回测 (2023-2026, 9币) ===')
    for hold in [15, 60]:
        backtest(hold)


if __name__ == '__main__':
    main()
