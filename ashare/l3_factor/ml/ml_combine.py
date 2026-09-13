#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ML 组合器实验 —— 22 特征 → ret_skip 截面预测, 按月扩窗 walk-forward + 1 天 embargo。

三层对照(裁决标准):
  base  : 最强单因子 eat_age_imb(取负, 负IC→看涨方向)
  ridge : 22 特征线性组合(闭式解, 无调参)
  gbdt  : LightGBM(固定一组保守参数, 【绝不调参】—— 调参=向本样本过拟合)
GBDT 只有显著赢过 ridge, 才能声称"非线性/交互有增量"。
评估: 测试月逐日截面 rank-IC(与 ret_skip 原值), 日间 t; GBDT−Ridge 配对差的日间 t; 尾部3% alpha。
⚠ 特征本身在本半年被选择过 → 本实验只回答"组合方式的相对增量", 绝对水平待 2023-25 净土。
"""
import glob, os, sys
import numpy as np, pandas as pd
from qr.paths import BT, DATA

F13 = ["trd_imb","eat_age_imb","eat_age_med","old_eat_imb","cxl_life_med","flash_cxl_ratio",
       "flash_cxl_imb","mean_size","n_ord","ord_imb_cnt","ord_imb_vol","cxl_ratio","large_share"]
FUQ = ["chain_share","chain_imb","agg_hhi","agg_hhi_imb","kept_rem_imb","chase_fwd_imb",
       "book_age_imb","book_age_mean","refill_imb"]
FEATS = F13 + FUQ
EXCL = {"20260713", "20260714"}          # 回测保留日, 两边都不用
TRAIN_SAMPLE = 120_000                    # 每训练日抽样行数(控内存/时长)
SEED = 7


def load_day(d, sample=0):
    f = pd.read_parquet(f"{DATA}/ml/day_{d}.parquet")
    if sample and len(f) > sample:
        f = f.sample(sample, random_state=SEED)
    return f


def daily_ic(pred, day_df):
    out = []
    t = day_df.assign(p=pred)
    for _, g in t.groupby(["sess", "bkt"]):
        if len(g) >= 50:
            out.append(g.p.rank().corr(g.ret_skip.rank()))
    return float(np.nanmean(out))


def tail_alpha(pred, day_df, k=0.03):
    t = day_df.assign(p=pred)
    vals = []
    for _, g in t.groupby(["sess", "bkt"]):
        if len(g) < 200:
            continue
        n = max(int(len(g)*k), 5)
        gs = g.sort_values("p")
        vals.append((gs.tail(n).ret_skip.mean() - gs.head(n).ret_skip.mean()) / 2 * 1e4)
    return float(np.nanmean(vals))


def main():
    days = sorted(os.path.basename(p)[4:12] for p in glob.glob(DATA + "/ml/day_*.parquet"))
    days = [d for d in days if d not in EXCL]
    months = sorted({d[:6] for d in days})
    import lightgbm as lgb

    recs = []
    imp_last = None
    for mi, m in enumerate(months):
        if mi == 0:
            continue                                  # 首月只当训练
        train_days = [d for d in days if d[:6] < m][:-1]      # 1 天 embargo
        test_days = [d for d in days if d[:6] == m]
        tr = pd.concat([load_day(d, TRAIN_SAMPLE) for d in train_days], ignore_index=True)
        X = tr[FEATS].fillna(0.5).values.astype(np.float32)
        y = (tr.y_rank - 0.5).values.astype(np.float32)
        # ridge 闭式解
        lam = 1e-3 * len(X)
        A = X.T @ X + lam * np.eye(len(FEATS), dtype=np.float32)
        w = np.linalg.solve(A, X.T @ y)
        # gbdt 固定参数
        gb = lgb.LGBMRegressor(n_estimators=300, learning_rate=0.05, num_leaves=63,
                               min_child_samples=500, subsample=0.8, colsample_bytree=0.8,
                               random_state=SEED, n_jobs=32, verbose=-1)
        gb.fit(X, y)
        imp_last = pd.Series(gb.feature_importances_, index=FEATS)
        for d in test_days:
            te = load_day(d)
            Xt = te[FEATS].fillna(0.5).values.astype(np.float32)
            preds = {"base": -te.eat_age_imb.fillna(0.5).values,
                     "ridge": Xt @ w,
                     "gbdt": gb.predict(Xt)}
            row = {"month": m, "day": d}
            for k2, p in preds.items():
                row[f"ic_{k2}"] = daily_ic(p, te)
                row[f"ta_{k2}"] = tail_alpha(p, te)
            recs.append(row)
        print(f"[{m}] train {len(train_days)}d×{TRAIN_SAMPLE//1000}k  test {len(test_days)}d 完成", file=sys.stderr)

    r = pd.DataFrame(recs)
    r.to_csv(BT + "/ml_daily_ic.csv", index=False)
    print("\n===== ML 组合器 walk-forward(测试月 = 训练期之后)=====")
    print(f"{'模型':<8}{'日均IC':>9}{'日间t':>8}{'同号%':>7}{'尾部3% alpha(bp)':>18}")
    for k2, nm in (("base", "单因子"), ("ridge", "Ridge"), ("gbdt", "GBDT")):
        v = r[f"ic_{k2}"].dropna()
        t = v.mean()/v.std(ddof=1)*np.sqrt(len(v))
        print(f"{nm:<8}{v.mean():>+9.4f}{t:>+8.1f}{(np.sign(v)==np.sign(v.mean())).mean()*100:>6.0f}%"
              f"{r[f'ta_{k2}'].mean():>+16.1f}")
    d1 = (r.ic_gbdt - r.ic_ridge).dropna()
    d2 = (r.ic_ridge - r.ic_base).dropna()
    print(f"\n配对检验(日间): Ridge−单因子 Δ={d2.mean():+.4f} t={d2.mean()/d2.std(ddof=1)*np.sqrt(len(d2)):+.1f}"
          f" | GBDT−Ridge Δ={d1.mean():+.4f} t={d1.mean()/d1.std(ddof=1)*np.sqrt(len(d1)):+.1f}")
    print("\n按月分解(日均IC):")
    print(r.groupby("month")[["ic_base", "ic_ridge", "ic_gbdt"]].mean().round(4).to_string())
    print("\nGBDT 特征重要度(最后一折, 前10):")
    print((imp_last.sort_values(ascending=False).head(10) / imp_last.sum() * 100).round(1).to_string())


if __name__ == "__main__":
    main()
