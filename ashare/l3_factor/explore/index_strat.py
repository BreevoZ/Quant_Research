#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
指数择时 v0 策略回测(中证500)。
信号: score = −z(市场eat_age_imb) − z(市场cxl_life_med)   (z 用过去20天分布, 无前视)
规则: score>+TH 做多 / <−TH 做空 / 中间清仓; 进场延迟2分钟; 只日内, 14:52后强平; 持仓变向才换手。
成本: 每次往返 COST bp(期指~2 / ETF~5), 双口径报告。
输出: 日度净收益序列 + 汇总(年化/夏普/命中/换手) + 落盘 bt/index_strat_daily.csv
"""
import numpy as np, pandas as pd
from qr.paths import BT, DATA

TH = 1.0
DELAY = 2
SIGS = {"eat_age_imb": -1.0, "cxl_life_med": -1.0}   # 权重: 负IC → 取负
LOOKBACK = 20
FLAT_AFTER = 14*60+52

mkt = pd.read_csv(DATA + "/market_ts.csv", dtype={"date": str})
idx = pd.read_csv(DATA + "/index_min_2026h1.csv")
idx["date"] = idx.d.str.replace("-", "")
px = idx[idx.symbol == "sh000905"].pivot_table(index="date", columns="hm", values="px")

days = sorted(set(mkt.date) & set(px.index))
mkt = mkt[mkt.date.isin(days)]
hist = {s: [] for s in SIGS}
rows = []
for i, d in enumerate(days):
    g = mkt[mkt.date == d].set_index("bkt")
    if i >= LOOKBACK:
        score = pd.Series(0.0, index=g.index)
        for s, w in SIGS.items():
            arr = np.concatenate(hist[s][-LOOKBACK:])
            mu, sd = np.nanmean(arr), np.nanstd(arr)
            score = score + w*(g[s]-mu)/(sd if sd else 1)
        p = px.loc[d]
        # 决策频率匹配信号horizon: 每30分钟检查点, 信号=前15分钟均分, 持有到下检查点
        CKPTS = [600, 630, 660, 780, 810, 840, 870]    # 10:00..11:00, 13:00..14:30
        pos, entry_px, ntrade, pnl = 0, 0.0, 0, 0.0
        sm = score.rolling(15, min_periods=8).mean()
        for t in CKPTS + [FLAT_AFTER]:
            if t+DELAY not in p.index:
                continue
            cur = p[t+DELAY]
            if t == FLAT_AFTER:
                want = 0
            else:
                sv = sm.reindex([t]).iloc[0] if t in sm.index else np.nan
                if np.isnan(sv):
                    continue
                want = 1 if sv > TH else (-1 if sv < -TH else pos)   # 滞回: 不过阈值就保持
            if want != pos:
                if pos != 0:
                    pnl += pos*(cur-entry_px)/entry_px; ntrade += 1
                if want != 0:
                    entry_px = cur
                pos = want
        rows.append((d, pnl*1e4, ntrade))
    for s in SIGS:
        hist[s].append(g[s].values)

df = pd.DataFrame(rows, columns=["date", "gross_bp", "trades"])
for tag, cost in (("期指2bp", 2.0), ("ETF5bp", 5.0)):
    net = df.gross_bp - df.trades*cost
    ann = net.mean()*244/100
    shp = net.mean()/net.std()*np.sqrt(244)
    print(f"[{tag}] 日均毛 {df.gross_bp.mean():+.1f}bp | 日均净 {net.mean():+.1f}bp | "
          f"年化 {ann:+.1f}% | 夏普 {shp:+.2f} | 胜率 {(net>0).mean()*100:.0f}% | 换手 {df.trades.mean():.1f} 次/日")
df["net2"] = df.gross_bp - df.trades*2.0
df["net5"] = df.gross_bp - df.trades*5.0
df.to_csv(BT + "/index_strat_daily.csv", index=False)
print(f"[{len(df)} 个交易日(前{LOOKBACK}天热身), 明细 -> bt/index_strat_daily.csv]")
