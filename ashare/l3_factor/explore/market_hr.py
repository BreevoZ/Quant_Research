#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
市场"赫罗图" —— 把恒星分类的思路搬到股票。

天文 HR 图: 横轴温度、纵轴光度 → 恒星自然聚成主序星/巨星/白矮星等族群。
市场版: 给每只股票算几个"微观性格"坐标, 看 2800 只票会不会聚成天然族群
        (散户票 / 机构票 / 庄股 / 高频票 …), 且各族的因子表现是否不同。

坐标(每股在全样本期的中位数, 跨股可比):
  X 活跃度      log(日均成交笔数)              —— 冷清 ↔ 热闹
  Y 知情度      |eat_age_imb| 的日均            —— 噪声主导 ↔ 耗竭反转强
  C 单笔规模    log(mean_size)                 —— 散户小单 ↔ 大单
  S 撤单强度    cxl_ratio                       —— 挂了就撤 ↔ 稳定挂单
聚类(KMeans on 标准化坐标)→ 看族群, 并对每族算 eat_age_imb 的 IC(哪族最"可预测")。
输出: bt/market_hr.png
"""
import glob, sys
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from qr.paths import BT, DATA

SUR = "#fcfcfb"; INK = "#0b0b0b"; GRID = "#e8e7e3"
PAL = ["#2a78d6", "#1baf7a", "#eda100", "#e34948", "#9b59b6", "#16a085"]


def _day_stat(f):
    import pandas as pd, numpy as np
    d = pd.read_csv(f, dtype={"sym": str},
                    usecols=["sym","n_ord","eat_age_imb","mean_size","cxl_ratio","last_px"]).dropna(subset=["n_ord"])
    out = {}
    for s, g in d.groupby("sym"):
        out[s] = (g.n_ord.mean(), g.eat_age_imb.abs().mean(),
                  g.mean_size.median(), g.cxl_ratio.median(), g.last_px.median())
    return out


def main():
    files = sorted(glob.glob(DATA + "/panels/panel_id_*.csv"))[::3]   # 每3天抽1天(求中位数够用)
    print(f"[HR图] 抽 {len(files)} 天面板并行读, 算每股性格坐标", file=sys.stderr)
    cols = ["sym", "n_ord", "eat_age_imb", "mean_size", "cxl_ratio", "last_px"]
    from multiprocessing import Pool
    with Pool(20) as pool:
        parts = pool.map(_day_stat, files)
    agg = {}
    for i, part in enumerate(parts):
        for s, tup in part.items():
            agg.setdefault(s, []).append(tup)
        if (i+1) % 20 == 0:
            print(f"  {i+1}/{len(files)}", file=sys.stderr)
    rows = []
    for s, r in agg.items():
        r = np.array(r)
        if len(r) < 10:                          # 至少 10 天
            continue
        rows.append((s, np.nanmedian(r[:, 0]), np.nanmedian(r[:, 1]),
                     np.nanmedian(r[:, 2]), np.nanmedian(r[:, 3]), np.nanmedian(r[:, 4])))
    df = pd.DataFrame(rows, columns=["sym", "activity", "informed", "size", "cxl", "px"]).dropna()
    df = df[(df.activity > 0) & (df.size > 0)]
    df["logact"] = np.log10(df.activity)
    df["logsize"] = np.log10(df.size)
    print(f"  {len(df)} 只股票入图", file=sys.stderr)

    from sklearn.preprocessing import StandardScaler
    from sklearn.cluster import KMeans
    Xc = StandardScaler().fit_transform(df[["logact", "informed", "logsize", "cxl"]])
    km = KMeans(n_clusters=4, random_state=7, n_init=10).fit(Xc)
    df["cluster"] = km.labels_

    # 每族画像
    print("\n族群画像(中位数):", file=sys.stderr)
    names = {}
    for c in sorted(df.cluster.unique()):
        g = df[df.cluster == c]
        print(f"  族{c} ({len(g):4d}只): 活跃 {10**g.logact.median():6.0f}笔/桶 | "
              f"知情 {g.informed.median():.3f} | 单笔 {10**g.logsize.median():5.0f}股 | "
              f"撤单率 {g.cxl.median():.2f} | 价 {g.px.median():.1f}元", file=sys.stderr)

    # 画 HR 图: X=活跃度, Y=知情度, 颜色=族, 点大小=单笔规模
    plt.rcParams.update({"font.family": ["DejaVu Sans", "AR PL UMing CN"], "axes.unicode_minus": False,
                         "figure.facecolor": SUR, "axes.facecolor": SUR, "text.color": INK,
                         "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID, "grid.alpha": 0.5,
                         "xtick.color": INK, "ytick.color": INK, "axes.labelcolor": INK,
                         "axes.spines.top": False, "axes.spines.right": False})
    fig, ax = plt.subplots(figsize=(10, 7.5), dpi=110)
    for c in sorted(df.cluster.unique()):
        g = df[df.cluster == c]
        ax.scatter(g.logact, g.informed, s=8+18*(g.logsize-df.logsize.min()),
                   c=PAL[c % len(PAL)], alpha=0.45, edgecolors="none", label=f"族{c} ({len(g)}只)")
    ax.set_xlabel("活跃度  log₁₀(日均成交笔数/分钟桶)  →  越右越热闹")
    ax.set_ylabel("知情度  |被吃单年龄失衡| 日均  →  越上耗竭反转越强")
    ax.set_title("市场'赫罗图': 2800 只 A 股按微观性格聚类\n(点大小=单笔委托规模; 借恒星分类思路)",
                 loc="left", fontsize=12.5)
    ax.legend(frameon=False, fontsize=9, loc="upper right")
    fig.tight_layout()
    fig.savefig(BT + "/market_hr.png", facecolor=SUR)
    df.to_csv(BT + "/market_hr.csv", index=False)
    print("[图] bt/market_hr.png | [数据] bt/market_hr.csv", file=sys.stderr)


if __name__ == "__main__":
    main()
