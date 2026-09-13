#!/usr/bin/env python3
# 按族分别算 eat_age_imb 的样本外 IC, 验证"冷门高知情族信号更强"
import glob, sys
from multiprocessing import Pool

import numpy as np, pandas as pd

from qr.paths import BT, DATA

# 模块级读入: day_ic 在子进程里要用 c2, 留在模块层最省事。
hr = pd.read_csv(BT + "/market_hr.csv", dtype={"sym": str})
c2 = dict(zip(hr.sym, hr.cluster))                       # sym → 族


def day_ic(f):
    d = pd.read_csv(f, dtype={"sym":str}, usecols=["sym","sess","bkt","eat_age_imb","ret_skip"]).dropna()
    d["cl"] = d.sym.map(c2)
    out = {}
    for cl, g in d.groupby("cl"):
        ics = []
        for _, gg in g.groupby(["sess","bkt"]):
            if len(gg) >= 20:
                ics.append(gg.eat_age_imb.rank().corr(gg.ret_skip.rank()))
        if ics: out[int(cl)] = np.nanmean(ics)
    return out


def main():
    files = sorted(glob.glob(DATA + "/panels/panel_id_2023*.csv") +      # 样本外年份
                   glob.glob(DATA + "/panels/panel_id_2024*.csv") +
                   glob.glob(DATA + "/panels/panel_id_2025*.csv"))
    print(f"[分族IC] {len(files)} 天样本外", file=sys.stderr)
    with Pool(20) as p:
        res = p.map(day_ic, files)
    acc = {}
    for r in res:
        for cl, ic in r.items():
            acc.setdefault(cl, []).append(ic)
    names = {0:"冷门高知情", 1:"市场主体", 2:"中小盘", 3:"热门低知情"}
    print("\n族  画像        样本外 skip-IC   日间t   同号%   天数")
    for cl in sorted(acc):
        v = np.array([x for x in acc[cl] if np.isfinite(x)])
        t = v.mean()/v.std(ddof=1)*np.sqrt(len(v))
        print(f"{cl}  {names.get(cl,''):10s} {v.mean():+.4f}      {t:+.1f}   {(np.sign(v)==np.sign(v.mean())).mean()*100:.0f}%   {len(v)}")


if __name__ == "__main__":
    main()
