#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
多日因子评估 —— 日度截面 IC 聚合。

与单日脚本的关键区别: t 值按【日间】算(每天先得一个日均 IC, 天与天独立),
不再用桶间 t(桶自相关严重偏乐观)。这是第一个"算数"的显著性。

用法: python eval/eval_multiday.py [--panels data/panels] [--days 20260707,20260708,...]
      缺省读 panels 目录下全部 panel_id_*.csv。
"""
import sys, glob, os, argparse
import numpy as np, pandas as pd
from qr.paths import DATA

NONFEAT = {"sym", "sess", "bkt", "last_px", "ret_next", "ret_skip"}
FEATS: list = []          # 运行时从面板列自动发现(main 里填)
LABS = ["ret_next", "ret_skip"]


def day_ics(path):
    """一个面板 → {(feature,label): 日均IC}"""
    f = pd.read_csv(path)
    out = {}
    for feat in FEATS:
        for lab in LABS:
            ics = []
            for _, g in f.groupby(["sess", "bkt"]):
                s = g[[feat, lab]].dropna()
                if len(s) >= 50:
                    ics.append(s[feat].rank().corr(s[lab].rank()))
            out[(feat, lab)] = float(np.mean(ics)) if ics else np.nan
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--panels", default=DATA + "/panels")
    ap.add_argument("--days", default="", help="逗号分隔; 缺省=目录下全部")
    ap.add_argument("--exclude", default="", help="逗号分隔, 剔除(如回测保留日)")
    ap.add_argument("--jobs", type=int, default=32)
    ap.add_argument("--dump", default="", help="日度IC序列落盘 CSV")
    a = ap.parse_args()

    files = sorted(glob.glob(f"{a.panels}/panel_id_*.csv"))
    if a.days:
        keep = set(a.days.split(","))
        files = [f for f in files if os.path.basename(f)[9:17] in keep]
    if a.exclude:
        drop = set(a.exclude.split(","))
        files = [f for f in files if os.path.basename(f)[9:17] not in drop]
    days = [os.path.basename(f)[9:17] for f in files]
    global FEATS
    FEATS = [c for c in pd.read_csv(files[0], nrows=1).columns if c not in NONFEAT]
    print(f"[评估] {len(files)} 天: {days[0]}..{days[-1]}; {len(FEATS)} 特征", file=sys.stderr)

    from multiprocessing import Pool
    with Pool(min(a.jobs, len(files))) as pool:
        results = pool.map(day_ics, files)
    rows = dict(zip(days, results))
    if a.dump:
        rec = [dict(day=d, feature=f, label=l, ic=v)
               for d in days for (f, l), v in rows[d].items()]
        pd.DataFrame(rec).to_csv(a.dump, index=False)
        print(f"[落盘] 日度IC序列 -> {a.dump}", file=sys.stderr)

    # 汇总表: ≤10 天打逐日列, 否则打按月分段 + 全期
    print("\n========== 日度截面 IC(每天=各桶截面 rank-IC 的日均)==========")
    months = sorted({d[:6] for d in days})
    wide = len(days) <= 10
    for lab in LABS:
        print(f"\n--- {lab} ---")
        if wide:
            print(f"{'feature':<16}" + "".join(f"{d[4:]:>8}" for d in days) + f"{'mean':>9}{'t(日间)':>9}")
        else:
            print(f"{'feature':<16}" + "".join(f"{m[2:]:>9}" for m in months)
                  + f"{'mean':>9}{'t(日间)':>9}{'同号%':>8}")
        for feat in FEATS:
            xs = np.array([rows[d][(feat, lab)] for d in days], dtype=float)
            v = xs[~np.isnan(xs)]
            m = v.mean() if len(v) else np.nan
            t = m / v.std(ddof=1) * np.sqrt(len(v)) if len(v) > 1 and v.std(ddof=1) > 0 else np.nan
            if wide:
                print(f"{feat:<16}" + "".join(f"{x:>8.4f}" for x in xs) + f"{m:>9.4f}{t:>9.2f}")
            else:
                mo = [np.nanmean([rows[d][(feat, lab)] for d in days if d[:6] == mm]) for mm in months]
                sgn = np.mean(np.sign(v) == np.sign(m))*100 if len(v) else np.nan
                print(f"{feat:<16}" + "".join(f"{x:>9.4f}" for x in mo)
                      + f"{m:>9.4f}{t:>9.2f}{sgn:>7.0f}%")
    n = len(days)
    print(f"\n共 {n} 个独立交易日; 同号% = 与全期均值同符号的天数占比(方向稳定性)。")
    if n <= 10:
        print(f"⚠ 日间 t 自由度只有 {n-1}: 结论仍需更多天数。")


if __name__ == "__main__":
    main()
