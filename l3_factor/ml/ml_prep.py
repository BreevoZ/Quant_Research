#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ML 数据准备: 每天 13f+uniq 面板合并 → 22 特征【日内截面 rank 归一】(0~1, 无跨日统计→无前视)
→ data/ml/day_<date>.parquet(float32)。标签: ret_skip 原值 + 截面rank。
"""
import glob, os, sys
import numpy as np, pandas as pd
from qr.paths import DATA

F13 = ["trd_imb","eat_age_imb","eat_age_med","old_eat_imb","cxl_life_med","flash_cxl_ratio",
       "flash_cxl_imb","mean_size","n_ord","ord_imb_cnt","ord_imb_vol","cxl_ratio","large_share"]
FUQ = ["chain_share","chain_imb","agg_hhi","agg_hhi_imb","kept_rem_imb","chase_fwd_imb",
       "book_age_imb","book_age_mean","refill_imb"]
FEATS = F13 + FUQ

def prep_one(d):
    out = f"{DATA}/ml/day_{d}.parquet"
    if os.path.exists(out):
        return d, -1
    a = pd.read_csv(f"{DATA}/panels/panel_id_{d}.csv", dtype={"sym": str},
                    usecols=["sym","sess","bkt"]+F13+["ret_skip"])
    b = pd.read_csv(f"{DATA}/panels_uniq/panel_id_{d}.csv", dtype={"sym": str},
                    usecols=["sym","sess","bkt"]+FUQ)
    m = a.merge(b, on=["sym","sess","bkt"], how="inner").dropna(subset=["ret_skip"])
    g = m.groupby(["sess","bkt"])
    for c in FEATS:                                   # 截面 rank → [0,1], NaN 保留
        m[c] = g[c].rank(pct=True).astype(np.float32)
    m["y_rank"] = g["ret_skip"].rank(pct=True).astype(np.float32)
    m["ret_skip"] = m.ret_skip.astype(np.float32)
    m[["sym","sess","bkt"]+FEATS+["ret_skip","y_rank"]].to_parquet(out, index=False)
    return d, len(m)

if __name__ == "__main__":
    os.makedirs(DATA + "/ml", exist_ok=True)
    days = sorted(os.path.basename(p)[9:17] for p in glob.glob(DATA + "/panels_uniq/panel_id_*.csv"))
    from multiprocessing import Pool
    with Pool(8) as pool:
        for d, n in pool.imap_unordered(prep_one, days):
            if n >= 0:
                print(f"[{d}] {n} 行", file=sys.stderr)
    print(f"完成: {len(glob.glob(DATA + '/ml/day_*.parquet'))} 天")
