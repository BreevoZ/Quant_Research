#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
回测盈亏追踪图(每轮回测跑完 pnl.py 后执行, 出一张 2×2 PNG):
  A 日内累计净盈亏(合计/多腿/空腿, 按触发时刻)     — 盈亏从哪段时间来
  B 每对腿净盈亏分布                                — 是普遍小亏还是几笔大亏
  C 逆向选择诊断: 纸面alpha(成交组 vs 未成交组)     — 亏损机制一眼可见
  D 关键数字(净bp/胜率/填单率/往返数)

用法: python backtest/plot_pnl.py --pnl bt/res_20260713_pnl.csv \
        --panel data/panels/panel_id_20260713.csv --out bt/pnl_20260713.png [--hold 4]
"""
import argparse
import numpy as np, pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

# —— 视觉参数(dataviz 参考调色板, light 模式)——
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e8e7e3"
BLUE, AQUA, YELLOW = "#2a78d6", "#1baf7a", "#eda100"   # 合计 / 多腿 / 空腿(固定槽位)
plt.rcParams.update({
    "font.family": ["DejaVu Sans", "AR PL UMing CN"],
    "axes.unicode_minus": False,
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "text.color": INK, "axes.labelcolor": INK2,
    "xtick.color": INK2, "ytick.color": INK2,
    "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False,
})

ap = argparse.ArgumentParser()
ap.add_argument("--pnl", required=True, help="pnl.py 落盘的逐对明细 *_pnl.csv")
ap.add_argument("--panel", required=True, help="该日特征面板(算纸面alpha诊断)")
ap.add_argument("--out", required=True)
ap.add_argument("--hold", type=int, default=4)
a = ap.parse_args()

d = pd.read_csv(a.pnl, dtype={"sym": str})
pan = pd.read_csv(a.panel, dtype={"sym": str})[["sym", "bkt", "last_px"]].dropna()
px = pan.set_index(["sym", "bkt"]).last_px

# 交易分钟轴(挤掉午休): AM 09:30-11:30 → 0-120, PM 13:00-14:57 → 120-237
def tmin(sec): s = sec/60; return s-570 if s <= 690 else 120+(s-780)
def tlab(m, _): m = int(m); mm = 570+m if m <= 120 else 780+(m-120); return f"{mm//60:02d}:{mm%60:02d}"

fig, axs = plt.subplots(2, 2, figsize=(13.5, 8.2), dpi=110)
fig.subplots_adjust(hspace=0.42, wspace=0.26, left=0.07, right=0.97, top=0.90, bottom=0.08)
date = a.pnl.split("_")[-2] if a.pnl.count("_") >= 2 else ""
fig.suptitle(f"底仓 T+0 回测盈亏追踪 — {date}(样本外, v0 参数)", fontsize=14, x=0.07, ha="left", color=INK)

# ── A 日内累计净盈亏 ──
ax = axs[0, 0]
by = d.groupby("trig").net.sum().sort_index()
xs = [tmin(t) for t in by.index]
series = [("合计", by, BLUE, 2.2)]
for nm, dr, c in (("多腿", "B", AQUA), ("空腿", "S", YELLOW)):
    g = d[d.dir == dr].groupby("trig").net.sum().reindex(by.index, fill_value=0)
    series.append((nm, g, c, 1.6))
for nm, s, c, lw in series:
    ax.plot(xs, s.cumsum(), color=c, lw=lw, label=nm, solid_capstyle="round")
    ax.annotate(f" {nm} {s.sum():+,.0f}", (xs[-1], s.cumsum().iloc[-1]),
                color=c, fontsize=9, va="center")
ax.axhline(0, color=INK2, lw=0.8, alpha=0.5)
ax.axvline(120, color=GRID, lw=1)                      # 午休界(11:30|13:00)
ax.set_xlim(0, 262); ax.xaxis.set_major_formatter(FuncFormatter(tlab))
ax.set_xticks([0, 30, 60, 90, 120, 150, 180, 210, 237])
ax.set_title("A · 日内累计净盈亏(元)", loc="left", fontsize=11, color=INK)
ax.legend(frameon=False, fontsize=9, loc="lower left")

# ── B 每对净盈亏分布 ──
ax = axs[0, 1]
t = d[d.q > 0]
ax.hist(t.net, bins=40, color=BLUE, edgecolor=SURFACE, linewidth=0.8)
ax.axvline(0, color=INK2, lw=0.8, alpha=0.5)
mu = t.net.mean()
ax.axvline(mu, color=YELLOW, lw=1.6)
ax.annotate(f"均值 {mu:+,.0f} ", (mu, ax.get_ylim()[1]*0.98), color="#9a6b00",
            fontsize=9, ha="right", va="top", fontweight="bold")
ax.set_title(f"B · 每对净盈亏分布(元, {len(t)} 对完整往返)", loc="left", fontsize=11, color=INK)

# ── C 逆向选择诊断: 纸面 alpha(bp), 成交 vs 未成交 ──
ax = axs[1, 0]
rows = []
for _, m in d.iterrows():
    b0, b1 = m.trig//60-1, m.trig//60-1+a.hold
    try: p0, p1 = px.loc[(m.sym, b0)], px.loc[(m.sym, b1)]
    except KeyError: continue
    rows.append(((p1-p0)/p0*(1 if m.dir == "B" else -1)*1e4, m.fill_e > 0))
pr = pd.DataFrame(rows, columns=["bp", "filled"])
grp = [("未成交\n(错过的)", pr[~pr.filled].bp), ("成交\n(拿到的)", pr[pr.filled].bp), ("全部信号", pr.bp)]
vals = [g.mean() for _, g in grp]
bars = ax.bar(range(3), vals, width=0.55, color=BLUE, edgecolor=SURFACE)
for i, (v, (nm, g)) in enumerate(zip(vals, grp)):
    ax.annotate(f"{v:+.1f} bp", (i, v), ha="center", color=INK,
                va="bottom" if v >= 0 else "top", fontsize=10, fontweight="bold")
ax.axhline(0, color=INK2, lw=0.8)
ax.set_xticks(range(3)); ax.set_xticklabels([nm for nm, _ in grp], fontsize=9)
ax.set_title(f"C · 逆向选择诊断: 持有{a.hold}分钟纸面alpha(不含成本)", loc="left", fontsize=11, color=INK)

# ── D 关键数字 ──
ax = axs[1, 1]; ax.axis("off")
to = (t.px_e*t.q + t.px_x*t.q).sum()
tiles = [
    (f"{t.net.sum()/to*1e4:+.1f} bp", "净收益 / 双边成交额"),
    (f"{(t.net > 0).mean()*100:.0f}%", "胜率(净, 按对)"),
    (f"{d.fill_e.sum()/d.plan.sum()*100:.0f}%", "进场填单率(量)"),
    (f"{len(t)} / {len(d)}", "完整往返 / 总腿对"),
]
for i, (big, small) in enumerate(tiles):
    x, y = (i % 2)*0.5+0.03, 0.72-(i//2)*0.5
    ax.text(x, y, big, fontsize=24, fontweight="bold", color=INK, transform=ax.transAxes)
    ax.text(x, y-0.14, small, fontsize=10, color=INK2, transform=ax.transAxes)
ax.set_title("D · 关键数字", loc="left", fontsize=11, color=INK)

fig.savefig(a.out, facecolor=SURFACE, bbox_inches="tight")
print(f"[图] -> {a.out}")
