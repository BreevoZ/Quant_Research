#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
时钟相位扫描 —— 检验"每分钟在第几秒开火"是否影响执行成本(接 exec_timing 的 +2.83bp)。

背景: 全市场 FFT/逐笔 L-S 证实成交按整数秒时钟(:00/:30)成簇。若众人在整点抢单,
      则穿价半价差可能随"距整分钟的相位"周期性变化。本实验直接测这一点。

设计:
  · 复用 exec_timing 的选股与配额(同一批 60 股/天, 与 +2.83bp 实验可比), 但【均匀 TWAP 权重】,
    唯一变量 = 每分钟切片的开火秒 offset ∈ {0,10,20,30,40,50}(6 个相位臂)。
  · 成本度量用 vsim 的 slippage/arrival_mid(方向化)= 当场穿价半价差(bp), 天然去 30 分钟漂移。
  · 配对去均值: 同一(天·股·分钟)6 相位各开一枪, 组内减均值 → 消掉个股价差水平与分钟漂移,
    只留"相位净效应", 大幅提升信噪(腿级 cost_bp 标准差 ~42bp)。
输出: bt/phase_sweep.png, bt/phase_sweep.csv
"""
import sys, glob, os
import numpy as np, pandas as pd
import exec_timing as ET
from qr.paths import BT, DATA

OFFSETS = [0, 10, 20, 30, 40, 50]


def select_mid(date, uni_n):
    """中等流动性选股(冲击通道激活区): n_ord 与价位都取中段, 保证 100万/刀 会啃穿深档。
       返回与 ET.build_day 相同格式的 meta(sym, side, ref_px, total_qty)。"""
    p13 = f"{DATA}/panels/panel_id_{date}.csv"
    if not os.path.exists(p13):
        return None
    a = pd.read_csv(p13, dtype={"sym": str}, usecols=["sym", "sess", "bkt", "n_ord", "last_px"])
    win = a[(a.sess == 0) & (a.bkt >= ET.WIN_START_MIN - 1) & (a.bkt < ET.WIN_START_MIN + ET.N_SLICES)]
    liq = win[win.last_px.between(8, 60)].groupby("sym").agg(
        n=("n_ord", "mean"), cnt=("bkt", "size"), px=("last_px", "median"))
    liq = liq[(liq.cnt >= ET.N_SLICES) & (liq.n.between(80, 250))]   # 中流动性带
    liq = liq.sort_values("n", ascending=False).head(uni_n)
    if len(liq) < 10:
        return None
    syms = list(liq.index)
    rows = []
    for i, s in enumerate(syms):
        ref = float(liq.loc[s, "px"]); lots = int(ET.NOTIONAL / ref // 100)
        if lots < ET.N_SLICES:
            continue
        rows.append((s, 1 if i % 2 == 0 else 2, ref, lots * 100))
    return pd.DataFrame(rows, columns=["sym", "side", "ref_px", "total_qty"])


def phase_orders(meta, offset):
    orders, idmap = [], {}
    for _, mm in meta.iterrows():
        s = mm.sym; side = int(mm.side); ref = float(mm.ref_px); lots = int(mm.total_qty)//100
        if lots < ET.N_SLICES:
            continue
        q = ET.alloc(np.ones(ET.N_SLICES), lots)
        for k in range(ET.N_SLICES):
            if q[k] <= 0:
                continue
            t_ms = (ET.WIN_START_MIN + k)*60*1000 + offset*1000
            px = round(ref*(1+ET.CROSS) if side == 1 else ref*(1-ET.CROSS), 2)
            oid = f"E{len(orders)+1}"
            orders.append(f"{oid},1,102,{s},{side},{px:.2f},{q[k]*100},{t_ms}")
            idmap[oid] = (s, side, k)
    return orders, idmap


def main():
    days_n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    uni_n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    # 可选: 母单名义(元), 默认 100万; 传 30000000 => 100万/刀(激活冲击通道)
    if len(sys.argv) > 3:
        ET.NOTIONAL = int(sys.argv[3])
        ET.CROSS = 0.05                       # 大单放宽保险帽保证填满
    print(f"[配置] 母单名义 {ET.NOTIONAL/1e4:.0f}万 = {ET.NOTIONAL/ET.N_SLICES/1e4:.1f}万/刀, "
          f"保险帽 {ET.CROSS*100:.0f}%", file=sys.stderr)
    all_days = sorted(os.path.basename(p)[9:17] for p in glob.glob(DATA + "/panels_uniq/panel_id_*.csv"))
    step = max(len(all_days)//days_n, 1)
    days = all_days[::step][:days_n]
    mid = len(sys.argv) > 4 and sys.argv[4] == "mid"
    print(f"[相位扫描] {len(days)} 天 × {uni_n} 股 × {len(OFFSETS)} 相位"
          f"{' [中流动性universe]' if mid else ''}", file=sys.stderr)

    rows = []
    for d in days:
        if mid:
            meta = select_mid(d, uni_n)
        else:
            _, meta = ET.build_day(d, uni_n, lam=0.0, seed=int(d[-4:]))   # lam=0 只借选股/配额
        if meta is None or len(meta) == 0:
            print(f"[{d}] 跳过", file=sys.stderr); continue
        for off in OFFSETS:
            orders, idmap = phase_orders(meta, off)
            fr = ET.run_arm(d, f"ph{off}", orders)
            if fr is None:
                continue
            r = pd.read_csv(fr, dtype={"symbol": str})
            r = r[r.filled > 0]
            for _, x in r.iterrows():
                s, side, k = idmap.get(x.id, (None, None, None))
                if s is None:
                    continue
                sgn = 1.0 if side == 1 else -1.0
                cost = sgn * x.slippage / x.arrival_mid * 1e4
                rows.append({"date": d, "sym": s, "k": k, "off": off, "side": side,
                             "cost_bp": cost, "fillpct": float(x.fill_pct)})
        print(f"[{d}] done", file=sys.stderr)

    df = pd.DataFrame(rows)
    df.to_csv(BT + "/phase_sweep.csv", index=False)
    print(f"\n样本: {len(df)} 腿, {df.groupby(['date','sym','k']).ngroups} 个(天·股·分钟)配对格")

    # 配对去均值: 每(天·股·分钟)减去该格 6 相位均值
    df["cell"] = df.date + "_" + df.sym + "_" + df.k.astype(str)
    full = df.groupby("cell").off.nunique()
    keep = full[full == len(OFFSETS)].index                       # 只保留 6 相位齐全的格
    dd = df[df.cell.isin(keep)].copy()
    dd["resid"] = dd.cost_bp - dd.groupby("cell").cost_bp.transform("mean")
    print(f"配对格(6相位齐全): {len(keep)}\n")

    print("="*64)
    print(f"{'相位(秒)':>8} {'原始成本bp':>10} {'配对残差bp':>10} {'残差SE':>8} {'填单%':>7} {'n':>7}")
    print("-"*64)
    summ = []
    for off in OFFSETS:
        g = dd[dd.off == off]
        raw = g.cost_bp.mean()
        res = g.resid.mean(); se = g.resid.std()/np.sqrt(len(g))
        fp = g.fillpct.mean()
        tag = " ← 整点/半点" if off in (0, 30) else ""
        print(f"{off:>8} {raw:>10.3f} {res:>+10.3f} {se:>8.3f} {fp:>7.1f} {len(g):>7}{tag}")
        summ.append((off, raw, res, se, fp))
    print("-"*64)
    on = dd[dd.off.isin([0, 30])].resid
    offc = dd[dd.off.isin([10, 20, 40, 50])].resid
    diff = on.mean() - offc.mean()
    sed = np.sqrt(on.var()/len(on) + offc.var()/len(offc))
    print(f"踩点(0,30) − 错峰(10,20,40,50) 残差差: {diff:+.3f} bp  (t={diff/sed:+.2f})")
    print(f"  正 = 踩点更贵(错峰有利);  |t|<2 = 无显著相位效应")
    # 买/卖分开(排除分钟内漂移 artifact: 真价差效应应买卖同号)
    for sd, nm in ((1,"买"),(2,"卖")):
        gs = dd[dd.side==sd]
        on_s = gs[gs.off.isin([0,30])].resid; of_s = gs[gs.off.isin([10,20,40,50])].resid
        dsd = on_s.mean()-of_s.mean(); ses=np.sqrt(on_s.var()/len(on_s)+of_s.var()/len(of_s))
        print(f"    [{nm}单] 踩点−错峰 {dsd:+.3f} bp (t={dsd/ses:+.2f})")
    print("="*60)

    # 图
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    S = np.array(summ)
    plt.rcParams.update({"font.family":["DejaVu Sans","AR PL UMing CN"],"axes.unicode_minus":False,
        "figure.facecolor":"#fcfcfb","axes.facecolor":"#fcfcfb","axes.grid":True,
        "grid.color":"#e8e7e3","axes.edgecolor":"#e8e7e3","axes.spines.top":False,"axes.spines.right":False})
    fig, ax = plt.subplots(figsize=(9, 5), dpi=110)
    ax.errorbar(S[:,0], S[:,2], yerr=S[:,3], marker="o", color="#2a78d6", capsize=4, lw=1.5,
                label="配对残差(去分钟均值)")
    ax.axhline(0, color="#999", lw=1)
    for x in (0, 30):
        ax.axvline(x, color="#1baf7a", ls=":", lw=1.2, alpha=.7)
    ax.set_xlabel("每分钟开火秒 offset(绿线=整点0s/半点30s, 众人抢单相位)")
    ax.set_ylabel("配对残差成本 bp(正=更贵)")
    ax.set_title(f"时钟相位 vs 执行穿价成本  ({len(keep)} 配对格, {df.date.nunique()} 天)", loc="left")
    ax.legend(frameon=False)
    fig.tight_layout(); fig.savefig(BT + "/phase_sweep.png", facecolor="#fcfcfb")
    print("[图] bt/phase_sweep.png")


if __name__ == "__main__":
    main()
