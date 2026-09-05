#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
执行择时对照实验 —— L3 微观信号在"母单执行"场景下的价值验证。

场景: 某股票今日需在 10:00-10:30 完成一笔 100 万元的买入(或卖出), 分 30 个分钟切片。
      门票(印花/佣金/价差)本来就要付, 信号不承担新成本 —— 只决定【每分钟买多少】。

三组对照(每组独立跑 vsim, 避免互相争抢流动性):
  twap  : 均匀切片(执行基准)
  rand  : 随机权重(对照组, 排除"非均匀本身"带来的效应)
  sig   : 信号择时 —— 买单在"预测将涨"的分钟多买、"预测将跌"的分钟少买(卖单反之)
          score = z(kept_rem_imb) + z(chain_imb) − z(eat_age_imb), 截面 z, 高分=看涨
          权重 w ∝ clip(1 ± λ·score, 0.3, 2.0), 三组【总量完全相同】(最大余额法配额)

全部切片使用 marketable 单(限价±2%保险帽, 实际按对手价成交) → 三组填单率均≈100%,
因此成交均价的差异【纯粹来自择时】, 不掺填单率差异。

指标: 各组实际成交均价(执行 VWAP); 买单价格越低越好, 卖单越高越好。
      改善(bp) = 方向化(twap 均价 − sig 均价) / twap 均价 × 1e4

用法: python backtest/exec_timing.py --days 20 [--universe 60] [--lam 0.5]
"""
import argparse, os, subprocess, sys, glob
import numpy as np, pandas as pd
from qr import paths
from qr.paths import BT, DATA

_VSIM_CACHE = None
def _vsim():
    """vsim 可执行文件路径。延迟解析:没构建时才报错,import 本模块不受影响。"""
    global _VSIM_CACHE
    if _VSIM_CACHE is None:
        _VSIM_CACHE = str(paths.vsim())
    return _VSIM_CACHE
OUT = BT + "/exec"
WIN_START_MIN = 600            # 10:00
N_SLICES = 30                  # 30 个 1 分钟切片
NOTIONAL = 1_000_000           # 每个母单 100 万元
CROSS = 0.02                   # marketable 保险帽
CLIP = (0.3, 2.0)              # 权重上下限(现实执行算法的约束)


def alloc(weights, total_lots):
    """最大余额法: 按权重把 total_lots 手整数分配, 保证三组总量严格相等。"""
    w = np.asarray(weights, dtype=float)
    w = np.clip(w, CLIP[0], CLIP[1])
    raw = w / w.sum() * total_lots
    base = np.floor(raw).astype(int)
    rem = total_lots - base.sum()
    if rem > 0:
        idx = np.argsort(-(raw - base))[:rem]
        base[idx] += 1
    return base


def build_day(date, universe_n, lam, seed=0):
    """返回 {arm: DataFrame(orders)} + meta"""
    p13 = f"{DATA}/panels/panel_id_{date}.csv"
    puq = f"{DATA}/panels_uniq/panel_id_{date}.csv"
    if not (os.path.exists(p13) and os.path.exists(puq)):
        return None, None
    a = pd.read_csv(p13, dtype={"sym": str},
                    usecols=["sym", "sess", "bkt", "eat_age_imb", "n_ord", "last_px"])
    b = pd.read_csv(puq, dtype={"sym": str},
                    usecols=["sym", "sess", "bkt", "kept_rem_imb", "chain_imb"])
    m = a.merge(b, on=["sym", "sess", "bkt"], how="inner")
    win = m[(m.sess == 0) & (m.bkt >= WIN_START_MIN - 1) & (m.bkt < WIN_START_MIN + N_SLICES)]
    if len(win) == 0:
        return None, None

    # 选股: 窗口内委托强度最高(流动性代理) + 价格合理 + 全窗口有数据
    liq = win[win.last_px.between(5, 200)].groupby("sym").agg(
        n=("n_ord", "mean"), cnt=("bkt", "size"), px=("last_px", "median"))
    liq = liq[liq.cnt >= N_SLICES].sort_values("n", ascending=False).head(universe_n)
    syms = list(liq.index)
    if len(syms) < 10:
        return None, None

    # 方向: 一半买一半卖(同股不同时下反向单, 避免自我干扰)
    rng = np.random.default_rng(seed)
    sides = {s: (1 if i % 2 == 0 else 2) for i, s in enumerate(syms)}

    # 截面 z 分(每分钟横截面标准化, 无跨日统计量 → 无前视)
    def zs(g, c):
        v = g[c]
        sd = v.std()
        return (v - v.mean()) / (sd if sd and sd > 0 else 1)
    win = win.copy()
    for c in ("kept_rem_imb", "chain_imb", "eat_age_imb"):
        win[c] = win[c].fillna(win[c].median())
    win["score"] = win.groupby("bkt", group_keys=False).apply(
        lambda g: zs(g, "kept_rem_imb") + zs(g, "chain_imb") - zs(g, "eat_age_imb"),
        include_groups=False)
    sc = win.pivot_table(index="sym", columns="bkt", values="score")

    arms = {k: [] for k in ("twap", "rand", "sig")}
    meta = []
    for s in syms:
        side = sides[s]
        ref = float(liq.loc[s, "px"])
        lots = int(NOTIONAL / ref // 100)
        if lots < N_SLICES:                       # 保证每片至少能有量
            continue
        # 三组权重
        w_twap = np.ones(N_SLICES)
        w_rand = rng.uniform(CLIP[0], CLIP[1], N_SLICES)
        sig = np.array([sc.loc[s].get(WIN_START_MIN - 1 + k, np.nan) for k in range(N_SLICES)])
        sig = np.nan_to_num(sig, nan=0.0)
        # 买单: 预测涨(score高)→ 现在多买; 卖单: 预测跌(score低)→ 现在多卖
        w_sig = 1 + lam * (sig if side == 1 else -sig)
        for arm, w in (("twap", w_twap), ("rand", w_rand), ("sig", w_sig)):
            q = alloc(w, lots)
            for k in range(N_SLICES):
                if q[k] <= 0:
                    continue
                t_ms = (WIN_START_MIN + k) * 60 * 1000 + 1000        # 该分钟起始+1秒
                px = round(ref * (1 + CROSS) if side == 1 else ref * (1 - CROSS), 2)
                arms[arm].append(f"E{len(arms[arm])+1},1,102,{s},{side},{px:.2f},{q[k]*100},{t_ms}")
        meta.append((s, side, ref, lots*100))
    return arms, pd.DataFrame(meta, columns=["sym", "side", "ref_px", "total_qty"])


def run_arm(date, arm, orders):
    os.makedirs(OUT, exist_ok=True)
    fo = f"{OUT}/ord_{arm}_{date}.csv"
    with open(fo, "w") as h:
        h.write("id,action,market,symbol,side,price,qty,t\n" + "\n".join(orders) + "\n")
    fr = f"{OUT}/res_{arm}_{date}.csv"
    r = subprocess.run([_vsim(), "--tl-dir", str(paths.data("tmp", date)), "--date", date,
                        "--orders", fo, "--cache-dir", DATA + "/cache",
                        "--out", fr, "--trust-cache"], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"[{date} {arm}] vsim 失败: {r.stderr[-300:]}\n")
        return None
    os.remove(fo)
    return fr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=20)
    ap.add_argument("--universe", type=int, default=60)
    ap.add_argument("--lam", type=float, default=0.5)
    a = ap.parse_args()

    all_days = sorted(os.path.basename(p)[9:17] for p in glob.glob(DATA + "/panels_uniq/panel_id_*.csv"))
    step = max(len(all_days) // a.days, 1)
    days = all_days[::step][:a.days]
    print(f"[执行择时实验] {len(days)} 天 × {a.universe} 股 × 3 组, λ={a.lam}", file=sys.stderr)

    rows = []
    for d in days:
        arms, meta = build_day(d, a.universe, a.lam, seed=int(d[-4:]))
        if arms is None:
            print(f"[{d}] 跳过(面板不全)", file=sys.stderr); continue
        res = {}
        for arm in ("twap", "rand", "sig"):
            fr = run_arm(d, arm, arms[arm])
            if fr is None:
                break
            r = pd.read_csv(fr, dtype={"symbol": str})
            r = r[r.filled > 0]
            g = r.groupby("symbol").apply(
                lambda x: pd.Series({"vwap": (x.avg_price*x.filled).sum()/x.filled.sum(),
                                     "filled": x.filled.sum()}), include_groups=False)
            res[arm] = g
        if len(res) < 3:
            continue
        for _, mm in meta.iterrows():
            s = mm.sym
            if not all(s in res[k].index for k in res):
                continue
            row = {"date": d, "sym": s, "side": mm.side, "plan": mm.total_qty}
            for k in res:
                row[f"vwap_{k}"] = res[k].loc[s, "vwap"]
                row[f"fill_{k}"] = res[k].loc[s, "filled"]
            rows.append(row)
        print(f"[{d}] 完成 {len(meta)} 个母单", file=sys.stderr)

    df = pd.DataFrame(rows)
    df.to_csv(BT + "/exec_timing_detail.csv", index=False)
    sgn = np.where(df.side == 1, 1.0, -1.0)      # 买: 价低好 → (twap−sig) 为正即改善
    for arm in ("sig", "rand"):
        df[f"imp_{arm}"] = sgn * (df.vwap_twap - df[f"vwap_{arm}"]) / df.vwap_twap * 1e4

    print("\n" + "="*66)
    print("  执行择时对照实验: 母单 100 万元 / 10:00-10:30 / 30 切片")
    print("="*66)
    print(f"  样本: {len(df)} 个母单 ({df.date.nunique()} 天 × ~{len(df)//max(df.date.nunique(),1)} 股)")
    for k in ("twap", "rand", "sig"):
        print(f"  填单率 {k:<5}: {(df[f'fill_{k}']/df.plan).mean()*100:.1f}%")
    print("-"*66)
    for arm, nm in (("rand", "随机权重"), ("sig", "信号择时")):
        v = df[f"imp_{arm}"].dropna()
        byday = df.groupby("date")[f"imp_{arm}"].mean().dropna()
        t = byday.mean()/byday.std(ddof=1)*np.sqrt(len(byday)) if len(byday) > 2 else np.nan
        print(f"  {nm} vs TWAP: {v.mean():+.2f} bp/母单 | 日间 t={t:+.2f} | "
              f"胜率 {(v>0).mean()*100:.0f}% | 天数同号 {(np.sign(byday)==np.sign(byday.mean())).mean()*100:.0f}%")
    print("-"*66)
    for side, nm in ((1, "买入母单"), (2, "卖出母单")):
        g = df[df.side == side]
        if len(g):
            print(f"  [{nm}] 信号改善 {g.imp_sig.mean():+.2f} bp (n={len(g)})")
    print("="*66)
    print("  说明: 三组总量严格相同、均为 marketable 切片 → 差异纯来自【每分钟买多少】的分配。")
    print("  母单的印花/佣金/价差本就要付, 信号不承担新成本, 故改善为纯增量。")
    print(f"  [明细] bt/exec_timing_detail.csv")


if __name__ == "__main__":
    main()
