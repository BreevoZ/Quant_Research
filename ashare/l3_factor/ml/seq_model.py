#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
序列模型 —— 让模型直接从原始事件序列学"顺序信息", 对照手工特征基线。

【实验问题】一分钟内事件的【先后顺序】是否含有超出袋装统计量(我们22个手工因子)的信息?

【三层对照】(缺一不可, 否则是虚假胜利)
  base_gbdt : 22 手工特征 + GBDT   → 已知 IC ≈ 0.0356   ★必须打败这个
  seq_gru   : 原始事件序列 + GRU   → 本实验
  (安慰剂)  : 打乱标签重训, IC 必须塌到 ~0

【模型】GRU 而非 CNN:
  · CNN 平移不变 → 分不清"刚发生"还是"很久前"(即使有 age 特征也只是靠数值推断)
  · GRU 递归结构 → 隐状态逐步衰减, 天然给近期事件更大权重, 与"信号快速衰减"的先验一致
  · 输入已含 age_log(距窗口末秒数), 双保险

【防自欺】
  · 按月扩窗 walk-forward, 训练集尾部留 1 天 embargo(绝不随机切分)
  · 每 epoch 在验证月上评估截面 IC, 早停看 IC 不看 loss
  · 损失: 截面内的排序损失(直接优化我们评估用的目标)

用法: python ml/seq_model.py [--epochs 8] [--placebo]
"""
import argparse, glob, os, sys, time
import numpy as np
from qr.paths import DATA


def load_days(files):
    Xs, ys, ss, bs, ds = [], [], [], [], []
    for f in files:
        d = np.load(f, allow_pickle=True)
        Xs.append(d["X"]); ys.append(d["y"]); ss.append(d["sym"]); bs.append(d["bkt"])
        ds.append(np.full(len(d["y"]), os.path.basename(f)[:8]))
    return (np.concatenate(Xs), np.concatenate(ys), np.concatenate(ss),
            np.concatenate(bs), np.concatenate(ds))


def xs_ic(pred, y, day, bkt):
    """截面 rank-IC: 每(日,分钟)横截面内算, 再对截面取均值"""
    key = np.char.add(day.astype(str), bkt.astype(str))
    ics = []
    for k in np.unique(key):
        m = key == k
        if m.sum() >= 30:
            p, t = pred[m], y[m]
            pr = np.argsort(np.argsort(p)); tr = np.argsort(np.argsort(t))
            if pr.std() > 0 and tr.std() > 0:
                ics.append(np.corrcoef(pr, tr)[0, 1])
    return float(np.mean(ics)) if ics else np.nan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--hidden", type=int, default=48)
    ap.add_argument("--tag", default="")
    ap.add_argument("--batch", type=int, default=512)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--placebo", action="store_true", help="打乱训练标签(安慰剂)")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--sample", type=int, default=300_000, help="每折训练抽样上限")
    a = ap.parse_args()

    from nn_gru import GRURegressor
    np.random.seed(a.seed)

    files = sorted(glob.glob(DATA + "/seq/*.npz"))
    days = [os.path.basename(f)[:8] for f in files]
    months = sorted({d[:6] for d in days})
    print(f"[数据] {len(files)} 天: {days[0]}~{days[-1]} | 月份 {months}", file=sys.stderr)
    if len(months) < 2:
        print("⚠ 只有一个月, 无法做扩窗 walk-forward; 将按日 7:3 时序切分", file=sys.stderr)

    def train_eval(tr_files, te_files, tag):
        Xtr, ytr, _, btr, dtr = load_days(tr_files)
        Xte, yte, _, bte, dte = load_days(te_files)
        if a.placebo:                       # 安慰剂: 截面内打乱训练标签
            key = np.char.add(dtr.astype(str), btr.astype(str))
            for k in np.unique(key):
                m = key == k
                ytr[m] = np.random.permutation(ytr[m])
        mu, sd = Xtr.reshape(-1, Xtr.shape[2]).mean(0), Xtr.reshape(-1, Xtr.shape[2]).std(0) + 1e-6
        Xtr = (Xtr - mu) / sd; Xte = (Xte - mu) / sd
        rng = np.random.default_rng(a.seed)
        if a.sample and len(Xtr) > a.sample:                 # 抽样控训练时长
            sub = rng.permutation(len(Xtr))[:a.sample]
            Xtr, ytr = Xtr[sub], ytr[sub]
        # ★ 早停必须用【训练集内划出的验证集】, 绝不能用测试集
        #   (原实现取"测试集上 8 轮的最大 IC" = 在测试集上早停 = 选择偏差:
        #    纯噪声取 max 也必然为正, 安慰剂实测被虚高到 +0.0037)
        nv = max(int(len(Xtr)*0.15), 5000)
        Xva, yva = Xtr[-nv:], ytr[-nv:]
        Xtr, ytr = Xtr[:-nv], ytr[:-nv]
        net = GRURegressor(Xtr.shape[2], hid=a.hidden, seed=a.seed)
        best_va, best_te, patience = -9.0, np.nan, 0
        for ep in range(a.epochs):
            loss = net.fit_epoch(Xtr, ytr, batch=a.batch, lr=a.lr, rng=rng)
            va = float(np.corrcoef(np.argsort(np.argsort(net.predict(Xva))),
                                   np.argsort(np.argsort(yva)))[0, 1])   # 验证集(非截面, 仅供早停)
            te = xs_ic(net.predict(Xte), yte, dte, bte)                   # 测试集(仅记录, 不用于选择)
            print(f"    [{tag}] ep{ep+1}/{a.epochs} loss {loss:.5f} | 验证 {va:+.4f} | (测试 {te:+.4f})",
                  file=sys.stderr)
            if va > best_va:
                best_va, best_te, patience = va, te, 0
            else:
                patience += 1
                if patience >= 3:
                    break
        return best_te                                        # 报告"验证集最优轮"对应的测试 IC

    t0 = time.time()
    results = []
    if len(months) >= 2:
        for mi in range(1, len(months)):
            m = months[mi]
            tr = [f for f, d in zip(files, days) if d[:6] < m][:-1]        # 1 天 embargo
            te = [f for f, d in zip(files, days) if d[:6] == m]
            if not tr or not te:
                continue
            results.append(train_eval(tr, te, m))
    else:
        k = int(len(files)*0.7)
        results.append(train_eval(files[:k], files[k:], "时序7:3"))

    print("\n" + "="*56)
    print(f"  序列模型(GRU) {'【安慰剂: 训练标签已打乱】' if a.placebo else ''}")
    print("="*56)
    print(f"  各折测试 IC: {[f'{r:+.4f}' for r in results]}")
    print(f"  平均 IC    : {np.mean(results):+.4f}")
    print(f"  基线(22手工特征+GBDT): +0.0356   ← 必须打败它才说明'顺序'有增量")
    print(f"  用时 {time.time()-t0:.0f}s")
    print("="*56)


if __name__ == "__main__":
    main()
