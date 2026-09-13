#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
指数择时 v0 —— 自下而上: 全市场 L3 因子逐分钟聚合 → 预测指数未来走势。

① 市场聚合: 每(日,分钟桶) 对 2800+ 只股票的每个因子取截面均值 → 市场级时序信号
② 标签: 指数(sh000905/sh000300)分钟收盘, skip-1 进场, 持有 h ∈ {5,15,30,60} 分钟(不隔夜)
③ 评估: 逐日时序相关(TS-IC) → 日间 mean/t/同号%; 另报方向命中率
⚠ breadth 警告: 指数一天只有 ~240 个高度自相关的观测, 独立赌注远少于截面; t 只看日间。

用法: python explore/index_ts.py [--jobs 8] [--dump data/market_ts.csv]
"""
import sys, glob, os, argparse
import numpy as np, pandas as pd
from qr.paths import DATA

NONFEAT = {"sym", "sess", "bkt", "last_px", "ret_next", "ret_skip"}
HOR = [5, 15, 30, 60]


def agg_one(path):
    d = os.path.basename(path)[9:17]
    f = pd.read_csv(path)
    feats = [c for c in f.columns if c not in NONFEAT]
    g = f.groupby("bkt")[feats].mean()
    g.insert(0, "date", d)
    return g.reset_index()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--panels", default=DATA + "/panels")
    ap.add_argument("--index", default=DATA + "/index_min_2026h1.csv")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--dump", default=DATA + "/market_ts.csv")
    a = ap.parse_args()

    files = sorted(glob.glob(f"{a.panels}/panel_id_*.csv"))
    print(f"[聚合] {len(files)} 天全市场面板 → 市场级时序", file=sys.stderr)
    from multiprocessing import Pool
    with Pool(a.jobs) as pool:
        parts = pool.map(agg_one, files)
    mkt = pd.concat(parts, ignore_index=True)
    mkt.to_csv(a.dump, index=False)
    feats = [c for c in mkt.columns if c not in ("date", "bkt")]
    print(f"[市场时序] {len(mkt)} 行 × {len(feats)} 信号 -> {a.dump}", file=sys.stderr)

    idx = pd.read_csv(a.index)
    idx["date"] = idx.d.str.replace("-", "")
    out_rows = []
    for symbol, ig in idx.groupby("symbol"):
        piv = ig.pivot_table(index=["date", "hm"], values="px").px
        for h in HOR:
            # skip-1 进场: 信号桶 m 收盘后, 以 m+1 分钟收盘进场, 持有 h 分钟(同日, 不跨节次界不强制—指数连续)
            byday = {}
            for d, dg in ig.groupby("date"):
                s = dg.set_index("hm").px.sort_index()
                ent = s.shift(-1); fut = s.shift(-(1 + h))
                r = (fut - ent) / ent
                byday[d] = r
            out_rows.append((symbol, h, byday))

    print("\n===== 市场聚合信号 → 指数未来收益: 逐日TS-IC(日间统计) =====")
    mkt = mkt.set_index(["date", "bkt"])
    for symbol in ("sh000905", "sh000300"):
        rows = [r for r in out_rows if r[0] == symbol]
        print(f"\n--- {symbol} ({'中证500' if symbol.endswith('905') else '沪深300'}) ---")
        print(f"{'signal':<16}" + "".join(f"{'h='+str(h):>16}" for h in HOR) + "   (IC[t] 同号%)")
        for feat in feats:
            cells = []
            for h in HOR:
                byday = next(r[2] for r in rows if r[1] == h)
                ics = []
                for d, rser in byday.items():
                    if (d, ) and (d in mkt.index.get_level_values(0)):
                        sig = mkt.loc[d][feat]
                        lab = rser.reindex(sig.index)
                        sub = pd.concat([sig, lab], axis=1).dropna()
                        if len(sub) >= 100:
                            ics.append(sub.iloc[:, 0].corr(sub.iloc[:, 1]))
                v = np.array([x for x in ics if not np.isnan(x)])
                if len(v) > 2:
                    t = v.mean()/v.std(ddof=1)*np.sqrt(len(v))
                    sg = (np.sign(v) == np.sign(v.mean())).mean()*100
                    cells.append(f"{v.mean():+.3f}[{t:+.1f}]{sg:.0f}%")
                else:
                    cells.append("-")
            print(f"{feat:<16}" + "".join(f"{c:>16}" for c in cells))
    print(f"\n共 {mkt.index.get_level_values(0).nunique()} 天; TS-IC=当日内(信号_t, 指数收益_t+1→t+1+h)的相关。")


if __name__ == "__main__":
    main()
