#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TESS 光变曲线下载 —— 按【目标定向查询】(比天区扫描快两个量级, MAST 位置扫描会超时)。
产品: SPOC 120s(2分钟, 精选名单) + TESS-SPOC 200s(FFI 衍生, 无差别覆盖) 全要,
      两者互补 → 同一颗星的时间覆盖最大化。统一按时间排序后落盘。

输出: data/tess/lc_<TIC>.parquet  列 = [t_btjd, flux, exptime, sector]
用法: python explore/tess_lc.py [n_stars]
"""
import os, sys, warnings
warnings.filterwarnings("ignore")
import numpy as np, pandas as pd
from qr.paths import DATA

CACHE = DATA + "/tess"
os.makedirs(CACHE, exist_ok=True)


def fetch_one(tic):
    fp = f"{CACHE}/lc_{tic}.parquet"
    if os.path.exists(fp):
        return pd.read_parquet(fp), "cached"
    import lightkurve as lk
    sr = lk.search_lightcurve(f"TIC {tic}", mission="TESS")
    tb = sr.table.to_pandas()
    keep = tb[((tb.author == "SPOC") & (tb.exptime == 120)) |
              ((tb.author == "TESS-SPOC") & (tb.exptime == 200))].index.tolist()
    if not keep:
        return None, "no-product"
    parts = []
    for i in keep:
        try:
            lc = sr[int(i)].download()
            if lc is None:
                continue
            d = pd.DataFrame({
                "t_btjd": np.asarray(lc.time.value, float),
                "flux": np.asarray(lc.flux.value, float),
            }).dropna()
            if len(d) < 200:
                continue
            d["exptime"] = float(tb.loc[i, "exptime"])
            d["sector"] = int(tb.loc[i, "mission"].split()[-1]) if "Sector" in str(tb.loc[i, "mission"]) else -1
            parts.append(d)
        except Exception:
            continue
    if not parts:
        return None, "download-failed"
    out = pd.concat(parts, ignore_index=True).sort_values("t_btjd").reset_index(drop=True)
    out.to_parquet(fp, index=False)
    return out, "ok"


def main():
    want = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    tg = pd.read_csv(DATA + "/tess_targets.csv")
    # 优先取被多个 sector 观测的星(时间延续性最好)
    ns = tg.groupby("target_name").sequence_number.nunique().sort_values(ascending=False)
    tics = [str(t) for t in ns.index][:want*2]

    ok = 0
    for i, tic in enumerate(tics):
        if ok >= want:
            break
        df, status = fetch_one(tic)
        if df is not None:
            ok += 1
            span = df.t_btjd.max() - df.t_btjd.min()
            print(f"  [{ok:3d}] TIC {tic}: {len(df):6d} 点, {df.sector.nunique()} 个sector, "
                  f"跨度 {span:.0f} 天, 频率 {sorted(df.exptime.unique())}", file=sys.stderr)
        else:
            print(f"        TIC {tic}: {status}", file=sys.stderr)
    print(f"\n[完成] {ok} 颗星的光变曲线 → {CACHE}/")


if __name__ == "__main__":
    main()
