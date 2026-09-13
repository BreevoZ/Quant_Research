#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ML 安慰剂检验: 训练标签在【每个截面内随机打乱】后重跑 walk-forward。
真信号被打散 → 三模型 IC 都应塌到 ~0; 若 GBDT 仍显著>0 或仍>Ridge, 说明增量来自
数据结构假象/泄漏, 而非真交互。同口径对照真实运行(ml_combine)。
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
TRAIN_SAMPLE = 120_000
SEED = 7
rng = np.random.default_rng(SEED)


def load_day(d, sample=0, shuffle=False):
    f = pd.read_parquet(f"{DATA}/ml/day_{d}.parquet")
    if shuffle:                                   # 每截面内打乱标签 → 摧毁特征↔收益对应
        f["y_rank"] = f.groupby(["sess", "bkt"])["y_rank"].transform(lambda s: rng.permutation(s.values))
    if sample and len(f) > sample:
        f = f.sample(sample, random_state=SEED)
    return f


def daily_ic(pred, te):
    out = []
    t = te.assign(p=pred)
    for _, g in t.groupby(["sess", "bkt"]):
        if len(g) >= 50:
            out.append(g.p.rank().corr(g.ret_skip.rank()))   # 注意: 真实收益, 非打乱的
    return float(np.nanmean(out))


def main():
    import lightgbm as lgb
    days = sorted(os.path.basename(p)[4:12] for p in glob.glob(DATA + "/ml/day_*.parquet"))
    days = [d for d in days if d not in EXCL]
    months = sorted({d[:6] for d in days})
    recs = []
    for mi, m in enumerate(months):
        if mi == 0:
            continue
        train_days = [d for d in days if d[:6] < m][:-1]
        test_days = [d for d in days if d[:6] == m]
        tr = pd.concat([load_day(d, TRAIN_SAMPLE, shuffle=True) for d in train_days], ignore_index=True)
        X = tr[FEATS].fillna(0.5).values.astype(np.float32)
        y = (tr.y_rank - 0.5).values.astype(np.float32)      # 已打乱的标签
        lam = 1e-3 * len(X)
        w = np.linalg.solve(X.T @ X + lam*np.eye(len(FEATS), dtype=np.float32), X.T @ y)
        gb = lgb.LGBMRegressor(n_estimators=300, learning_rate=0.05, num_leaves=63,
                               min_child_samples=500, subsample=0.8, colsample_bytree=0.8,
                               random_state=SEED, n_jobs=32, verbose=-1).fit(X, y)
        for d in test_days:
            te = load_day(d)                                 # 测试集用真实标签评估
            Xt = te[FEATS].fillna(0.5).values.astype(np.float32)
            recs.append({"day": d,
                         "ic_base": daily_ic(-te.eat_age_imb.fillna(0.5).values, te),
                         "ic_ridge": daily_ic(Xt @ w, te),
                         "ic_gbdt": daily_ic(gb.predict(Xt), te)})
        print(f"[{m}] 打乱训练完成", file=sys.stderr)
    r = pd.DataFrame(recs)
    print("\n===== 安慰剂(训练标签截面内打乱)=====")
    print(f"{'模型':<8}{'日均IC':>10}{'日间t':>8}")
    for k, nm in (("ic_base", "单因子"), ("ic_ridge", "Ridge打乱"), ("ic_gbdt", "GBDT打乱")):
        v = r[k].dropna()
        print(f"{nm:<8}{v.mean():>+10.4f}{v.mean()/v.std(ddof=1)*np.sqrt(len(v)):>+8.1f}")
    d1 = (r.ic_gbdt - r.ic_ridge).dropna()
    print(f"\nGBDT打乱 − Ridge打乱: Δ={d1.mean():+.4f} t={d1.mean()/d1.std(ddof=1)*np.sqrt(len(d1)):+.1f}")
    print("预期(无泄漏): Ridge打乱/GBDT打乱 IC≈0(单因子 base 不变仍≈0.021, 它没用打乱标签);")
    print("             GBDT打乱−Ridge打乱 ≈ 0。若 GBDT打乱 仍显著>0 → 有结构性泄漏, 真实结果不可信。")


if __name__ == "__main__":
    main()
