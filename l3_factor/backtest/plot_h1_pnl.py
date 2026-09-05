#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
半年回测 PnL 聚合出图: 读 bt/h1bt/res_h1_<date>_pnl.csv 全部逐对明细 →
  A 半年累计净盈亏曲线(按日)  B 日净盈亏分布  C 月度净bp  D 关键数字
用法: python plot_h1_pnl.py [--dir bt/h1bt] [--out bt/h1_pnl.png]
"""
import argparse, glob, os, re
import numpy as np, pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from qr.paths import BT

SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e8e7e3"
BLUE, AQUA, YELLOW, RED = "#2a78d6", "#1baf7a", "#eda100", "#e34948"
plt.rcParams.update({
    "font.family": ["DejaVu Sans", "AR PL UMing CN"], "axes.unicode_minus": False,
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "text.color": INK, "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
    "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False,
})

ap = argparse.ArgumentParser()
ap.add_argument("--dir", default=BT + "/h1bt")
ap.add_argument("--out", default=BT + "/h1_pnl.png")
ap.add_argument("--title", default="eat_age_rev(提一档·持8分·底仓T+0)半年逐日回测")
a = ap.parse_args()

rows = []
for fp in sorted(glob.glob(f"{a.dir}/res_h1_*_pnl.csv")):
    d = re.search(r"res_h1_(\d{8})_pnl", fp).group(1)
    t = pd.read_csv(fp, dtype={"sym": str})
    t = t[t.q > 0]
    to = (t.px_e*t.q + t.px_x*t.q).sum()
    rows.append((d, t.net.sum(), to, len(t), (t.net > 0).mean()*100))
df = pd.DataFrame(rows, columns=["date", "net", "turnover", "pairs", "win"]).sort_values("date")
df["dt"] = pd.to_datetime(df.date)
df["bp"] = df.net/df.turnover*1e4
print(df.tail(3).to_string(index=False))

fig, axs = plt.subplots(2, 2, figsize=(13.5, 8.2), dpi=110)
fig.subplots_adjust(hspace=0.42, wspace=0.24, left=0.07, right=0.97, top=0.90, bottom=0.08)
fig.suptitle(f"{a.title} — {df.date.iloc[0]}~{df.date.iloc[-1]}({len(df)} 个交易日)",
             fontsize=13.5, x=0.07, ha="left", color=INK)

ax = axs[0, 0]
ax.plot(df.dt, df.net.cumsum(), color=BLUE, lw=2.0)
ax.fill_between(df.dt, df.net.cumsum(), 0, color=BLUE, alpha=0.08)
ax.axhline(0, color=INK2, lw=0.8, alpha=0.5)
ax.annotate(f" 累计 {df.net.sum():+,.0f} 元", (df.dt.iloc[-1], df.net.cumsum().iloc[-1]),
            color=BLUE, fontsize=10, va="center", fontweight="bold")
ax.set_title("A · 累计净盈亏(元, 按日)", loc="left", fontsize=11, color=INK)

ax = axs[0, 1]
ax.hist(df.net, bins=30, color=BLUE, edgecolor=SURFACE, linewidth=0.8)
ax.axvline(0, color=INK2, lw=0.8, alpha=0.5)
ax.axvline(df.net.mean(), color=YELLOW, lw=1.6)
ax.annotate(f"日均 {df.net.mean():+,.0f} ", (df.net.mean(), ax.get_ylim()[1]*0.97),
            color="#9a6b00", fontsize=9, ha="right", va="top", fontweight="bold")
ax.set_title("B · 日净盈亏分布(元)", loc="left", fontsize=11, color=INK)

ax = axs[1, 0]
mo = df.groupby(df.dt.dt.strftime("%m月")).apply(lambda g: g.net.sum()/g.turnover.sum()*1e4, include_groups=False)
ax.bar(range(len(mo)), mo.values, width=0.6, color=BLUE, edgecolor=SURFACE)
for i, v in enumerate(mo.values):
    ax.annotate(f"{v:+.1f}", (i, v), ha="center", va="bottom" if v >= 0 else "top", fontsize=9, color=INK)
ax.axhline(0, color=INK2, lw=0.8)
ax.set_xticks(range(len(mo))); ax.set_xticklabels(mo.index, fontsize=9)
ax.set_title("C · 月度净收益(bp/双边)", loc="left", fontsize=11, color=INK)

ax = axs[1, 1]; ax.axis("off")
tiles = [(f"{df.net.sum()/df.turnover.sum()*1e4:+.1f} bp", "净收益/双边(全期)"),
         (f"{(df.net>0).mean()*100:.0f}%", "盈利天占比"),
         (f"{df.net.mean():+,.0f} 元", "日均净盈亏"),
         (f"{int(df.pairs.sum()):,} 对", "完整往返(全期)")]
for i, (big, small) in enumerate(tiles):
    x, y = (i % 2)*0.5+0.03, 0.72-(i//2)*0.5
    ax.text(x, y, big, fontsize=22, fontweight="bold", color=INK, transform=ax.transAxes)
    ax.text(x, y-0.14, small, fontsize=10, color=INK2, transform=ax.transAxes)
ax.set_title("D · 关键数字", loc="left", fontsize=11, color=INK)

fig.savefig(a.out, facecolor=SURFACE, bbox_inches="tight")
print(f"[图] -> {a.out}")
