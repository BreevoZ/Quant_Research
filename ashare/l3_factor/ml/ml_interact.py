#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
非线性来源诊断: 训一个 GBDT(1-6月), 用 H-统计量近似找最强的两两交互对,
并对 top 交互对画 2D 部分依赖网格(文本热力), 让"GBDT>Ridge 的 +0.005"变得可解释。
"""
import glob, os, sys
import numpy as np, pandas as pd
from qr.paths import DATA

F13 = ["trd_imb","eat_age_imb","eat_age_med","old_eat_imb","cxl_life_med","flash_cxl_ratio",
       "flash_cxl_imb","mean_size","n_ord","ord_imb_cnt","ord_imb_vol","cxl_ratio","large_share"]
FUQ = ["chain_share","chain_imb","agg_hhi","agg_hhi_imb","kept_rem_imb","chase_fwd_imb",
       "book_age_imb","book_age_mean","refill_imb"]
FEATS = F13 + FUQ
EXCL = {"20260713", "20260714"}


def main():
    import lightgbm as lgb
    days = sorted(os.path.basename(p)[4:12] for p in glob.glob(DATA + "/ml/day_*.parquet"))
    days = [d for d in days if d not in EXCL and d[:6] <= "202606"]
    tr = pd.concat([pd.read_parquet(f"{DATA}/ml/day_{d}.parquet").sample(80_000, random_state=7)
                    for d in days], ignore_index=True)
    X = tr[FEATS].fillna(0.5).values.astype(np.float32)
    y = (tr.y_rank - 0.5).values.astype(np.float32)
    gb = lgb.LGBMRegressor(n_estimators=300, learning_rate=0.05, num_leaves=63,
                           min_child_samples=500, subsample=0.8, colsample_bytree=0.8,
                           random_state=7, n_jobs=32, verbose=-1).fit(X, y)

    # 单特征部分依赖(基准) + 两特征联合, 差值≈交互强度
    grid = np.linspace(0.1, 0.9, 5)
    base = X.mean(axis=0)
    def pd1(fi, v):
        Z = np.tile(base, (1, 1)).astype(np.float32); Z[0, fi] = v
        return gb.predict(Z)[0]
    def pd2(fi, fj, vi, vj):
        Z = np.tile(base, (1, 1)).astype(np.float32); Z[0, fi] = vi; Z[0, fj] = vj
        return gb.predict(Z)[0]

    print("[交互强度扫描: |PD(i,j) − PD(i) − PD(j) + PD0| 的网格均值, top12]", file=sys.stderr)
    pd0 = gb.predict(base.reshape(1, -1).astype(np.float32))[0]
    pd1c = {i: {v: pd1(i, v) for v in grid} for i in range(len(FEATS))}
    inter = []
    for i in range(len(FEATS)):
        for j in range(i+1, len(FEATS)):
            hh = np.mean([abs(pd2(i, j, vi, vj) - pd1c[i][vi] - pd1c[j][vj] + pd0)
                          for vi in grid for vj in grid])
            inter.append((hh, FEATS[i], FEATS[j]))
    inter.sort(reverse=True)
    print("\n===== 最强两两交互(H-统计量近似)=====")
    for hh, a, b in inter[:12]:
        print(f"  {hh*1e4:6.2f}  {a} × {b}")

    hh, fa, fb = inter[0]
    ia, ib = FEATS.index(fa), FEATS.index(fb)
    print(f"\n===== 最强对 {fa} × {fb} 的 2D 部分依赖(×1e4, 行={fa} 列={fb})=====")
    print("        " + "".join(f"{v:>8.1f}" for v in grid))
    for vi in grid:
        row = "".join(f"{(pd2(ia, ib, vi, vj)-pd0)*1e4:>8.2f}" for vj in grid)
        print(f"{vi:>6.1f}  {row}")
    print("\n读法: 若行内趋势随列翻转(如低行递增、高行递减)= 真交互; 若各行平行 = 只是可加, 无交互。")


if __name__ == "__main__":
    main()
