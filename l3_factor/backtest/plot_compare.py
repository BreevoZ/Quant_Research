#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
多版本回测对比图: 同一天、同一信号、不同执行方式的累计净盈亏曲线 + 关键数字。
用法: python backtest/plot_compare.py --out bt/compare_20260713.png \
        --run "v0 被动挂末价·持有4分" bt/v0_20260713/res_loop_pnl.csv \
        --run "v1a 提一档·持有8分"   bt/res_v1imp_pnl.csv \
        --run "v1b 穿价全捕获·持有8分" bt/res_v1crs_pnl.csv
"""
import argparse
import numpy as np, pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e8e7e3"
SERIES = ["#2a78d6", "#1baf7a", "#eda100", "#4a3aa7"]        # 固定槽位: 蓝/青/黄/紫
plt.rcParams.update({
    "font.family": ["DejaVu Sans", "AR PL UMing CN"], "axes.unicode_minus": False,
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "text.color": INK, "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
    "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False,
})

def tmin(sec): s = sec/60; return s-570 if s <= 690 else 120+(s-780)
def tlab(m,_): m=int(m); mm = 570+m if m <= 120 else 780+(m-120); return f"{mm//60:02d}:{mm%60:02d}"

ap = argparse.ArgumentParser()
ap.add_argument("--run", nargs=2, action="append", metavar=("LABEL","PNL_CSV"), required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--title", default="同一信号 × 三种执行 — 20260713(样本外)")
a = ap.parse_args()

fig, (ax, axr) = plt.subplots(1, 2, figsize=(13.5, 5.6), dpi=110,
                              gridspec_kw={"width_ratios": [1.9, 1]})
fig.subplots_adjust(wspace=0.18, left=0.06, right=0.98, top=0.86, bottom=0.12)
fig.suptitle(a.title, fontsize=13.5, x=0.06, ha="left", color=INK)

stats = []
for i, (label, path) in enumerate(a.run):
    d = pd.read_csv(path, dtype={"sym": str})
    by = d.groupby("trig").net.sum().sort_index()
    xs = [tmin(t) for t in by.index]
    c = SERIES[i % len(SERIES)]
    ax.plot(xs, by.cumsum(), color=c, lw=2.0, label=label, solid_capstyle="round")
    ax.annotate(f" {by.sum():+,.0f}", (xs[-1], by.cumsum().iloc[-1]), color=c, fontsize=9.5, va="center")
    t = d[d.q > 0]
    to = (t.px_e*t.q + t.px_x*t.q).sum()
    stats.append((label, c, t.net.sum()/to*1e4, (t.net > 0).mean()*100,
                  d.fill_e.sum()/d.plan.sum()*100, len(t)))

ax.axhline(0, color=INK2, lw=0.8, alpha=0.5)
ax.axvline(120, color=GRID, lw=1)
ax.set_xlim(0, 268); ax.xaxis.set_major_formatter(FuncFormatter(tlab))
ax.set_xticks([0, 30, 60, 90, 120, 150, 180, 210, 237])
ax.set_title("日内累计净盈亏(元)", loc="left", fontsize=11, color=INK)
ax.legend(frameon=False, fontsize=9, loc="lower left")

axr.axis("off")
axr.set_title("关键数字", loc="left", fontsize=11, color=INK)
rows = ["净bp/双边", "胜率", "进场填单率", "完整往返"]
for j, (label, c, bp, win, fill, n) in enumerate(stats):
    x = 0.06 + j*0.33
    axr.text(x, 0.92, label.split(" ")[0], fontsize=11, fontweight="bold", color=c, transform=axr.transAxes)
    vals = [f"{bp:+.1f}", f"{win:.0f}%", f"{fill:.0f}%", f"{n}"]
    for k, v in enumerate(vals):
        axr.text(x, 0.74-k*0.19, v, fontsize=13, fontweight="bold", color=INK, transform=axr.transAxes)
for k, r in enumerate(rows):
    axr.text(0.06, 0.74-k*0.19-0.085, r, fontsize=8.5, color=INK2, transform=axr.transAxes)

fig.savefig(a.out, facecolor=SURFACE, bbox_inches="tight")
print(f"[图] -> {a.out}")
