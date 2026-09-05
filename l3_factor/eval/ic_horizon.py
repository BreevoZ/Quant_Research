#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
IC 期限结构 —— 信号延续程度的直接度量。

对每个特征 × 每个持有期 h ∈ {1,2,4,8,15,30,60} 分钟, 算截面 rank-IC。
标签统一用【可交易口径】: 错后 1 分钟进场, 持有 h 分钟:
    ret(h) = ( px[t+1+h] − px[t+1] ) / px[t+1]     (节次内, 不跨午休)
读法: IC 随 h 增长/保持 = 慢信号(每笔往返携带更多 alpha, 能摊薄费用地板);
      IC 在 h=1 达峰后速降 = 快反转(eat_age 型, 只配执行择时)。

用法: python eval/ic_horizon.py --panels data/panels_uniq [--days d1,d2] [--feats a,b]
"""
import sys, os, glob, argparse
import numpy as np, pandas as pd

NONFEAT = {"sym", "sess", "bkt", "last_px", "ret_next", "ret_skip"}
HORIZONS = [1, 2, 4, 8, 15, 30, 60]


def day_ics(args):
    path, feats = args
    f = pd.read_csv(path, dtype={"sym": str})
    f = f.sort_values(["sym", "sess", "bkt"]).reset_index(drop=True)
    f["last_px"] = f.groupby(["sym", "sess"])["last_px"].ffill()
    g = f.groupby(["sym", "sess"])["last_px"]
    p1 = g.shift(-1)                                   # 进场价: 错后 1 分钟
    labs = {}
    for h in HORIZONS:
        labs[h] = (g.shift(-(1 + h)) - p1) / p1
    out = {}
    for feat in feats:
        for h in HORIZONS:
            ics = []
            sub = pd.DataFrame({"sess": f.sess, "bkt": f.bkt, "x": f[feat], "y": labs[h]}).dropna()
            for _, gg in sub.groupby(["sess", "bkt"]):
                if len(gg) >= 50:
                    ics.append(gg.x.rank().corr(gg.y.rank()))
            out[(feat, h)] = float(np.mean(ics)) if ics else np.nan
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--panels", required=True)
    ap.add_argument("--days", default="")
    ap.add_argument("--feats", default="", help="逗号分隔; 缺省=自动发现全部")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()
    files = sorted(glob.glob(f"{a.panels}/panel_id_*.csv"))
    if a.days:
        keep = set(a.days.split(","))
        files = [f for f in files if os.path.basename(f)[9:17] in keep]
    days = [os.path.basename(f)[9:17] for f in files]
    feats = a.feats.split(",") if a.feats else \
        [c for c in pd.read_csv(files[0], nrows=1).columns if c not in NONFEAT]
    print(f"[期限结构] {len(files)} 天 × {len(feats)} 特征 × {len(HORIZONS)} 持有期", file=sys.stderr)

    from multiprocessing import Pool
    with Pool(min(a.jobs, len(files))) as pool:
        results = pool.map(day_ics, [(f, feats) for f in files])

    print(f"\n===== IC 期限结构(错后1分钟进场, 持有 h 分钟; {len(days)} 天均值[日间t])=====")
    hdr = f"{'feature':<15}" + "".join(f"{'h='+str(h):>14}" for h in HORIZONS)
    print(hdr)
    for feat in feats:
        cells = []
        for h in HORIZONS:
            xs = np.array([r.get((feat, h), np.nan) for r in results], dtype=float)
            v = xs[~np.isnan(xs)]
            if len(v) > 1 and v.std(ddof=1) > 0:
                cells.append(f"{v.mean():+.4f}[{v.mean()/v.std(ddof=1)*np.sqrt(len(v)):+.1f}]")
            elif len(v):
                cells.append(f"{v.mean():+.4f}[  - ]")
            else:
                cells.append("-")
        print(f"{feat:<15}" + "".join(f"{c:>14}" for c in cells))
    print(f"\n读法: 沿行向右看 —— IC 保持/增长=慢信号(可摊费用); 速降=快信号(只配执行择时)。")
    print(f"⚠ {len(days)} 天日间 t 自由度 {len(days)-1}, 只看形态。h=60 尾部桶大量缺失, 样本偏早盘。")


if __name__ == "__main__":
    main()
