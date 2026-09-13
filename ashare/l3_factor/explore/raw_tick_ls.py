#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
逐笔 Lomb-Scargle —— L-S 名正言顺的场合: 单股成交时间戳天然不规则(10ms 网格, 中位间隔0)。
FFT 做不了(必须先分桶); L-S 直接吃不规则时间, 无需分桶。

对每只活跃票的【连续竞价段】成交, 跑三条不规则序列 + 一个点过程:
  ① 价      L-S(price@t)       —— 变星亮度的正统类比; 价近鞅, 预期无干净时钟
  ② 量      L-S(volume@t)      —— 算法定时切"等量子单"会在这里留时钟(60s/30s)
  ③ 主动流  L-S(dir*vol@t)     —— 有向委托流的节律
  ④ 到达    Rayleigh 周期图     —— 成交到达点过程本身的时钟(1min/30s 是否单股可见)

优化:
  · astropy method='fast'(FFT 加速 L-S), 0.07s/序列 vs scipy 的 >120s
  · 频率网格 samples_per_peak=5(吸取教训: 网格必须够密, 否则窄峰漏采 → picket-fence)
  · placebo: 打乱 marks 100 次取峰值分布 → 95% 阈值(时间戳不动, 只毁时间锁定结构)
  · Rayleigh 用解析 χ²₂ 阈值(Bonferroni 家族校正); 因成交成簇非泊松, 阈值偏松, 已注明
输出: bt/raw_ls.png, 控制台汇总表
"""
import glob, sys, time
import numpy as np
from multiprocessing import Pool
from qr import l3cache
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from astropy.timeseries import LombScargle
from qr.paths import BT, DATA

TICKERS = ["000001", "000725", "002594", "300750"]
AM0, AM1, PM0, PM1 = 34200, 41400, 46800, 53820          # 连续竞价(去开/收集合竞价)
PMIN, PMAX = 2.0, 600.0                                    # 关注周期 2s~10min
SPP = 5                                                    # samples_per_peak(L-S 网格密度)
NPERM = 60
CLOCKS = [60, 30, 20, 15, 10, 5]                          # 关注的整数时钟(秒)


def _toms(b):
    hh, mm, rest = b.decode().split(":"); ss, ms = rest.split(".")
    return (((int(hh) * 60 + int(mm)) * 60 + int(ss)) * 1000 + int(ms)) / 1000.0


def _load(code):
    day = sorted(glob.glob(DATA + "/cache/2026*"))[-1]
    a = l3cache.load(f"{day}/tl_sz_{code}.bin")
    tr = a[a["actionType"] == 2]
    sec = tr["timeSeconds"]
    m = ((sec >= AM0) & (sec < AM1)) | ((sec >= PM0) & (sec < PM1))
    tr = tr[m]
    t = np.array([_toms(b) for b in tr["time"]])
    tc = np.where(t < AM1, t - AM0, t - PM0 + (AM1 - AM0))   # 会话内秒(挤午休)
    o = np.argsort(tc); tc = tc[o]
    px = tr["price"][o].astype(float)
    vol = tr["volume"][o].astype(float)
    d = tr["tradeDirection"][o].astype(float); d[d == 0] = np.nan
    flow = np.where(np.isnan(d), 0.0, d) * vol
    return tc, px, vol, flow


def _ls(tc, y, freq):
    y = y - np.nanmean(y)
    return LombScargle(tc, y).power(freq, method="fast")


def _placebo_thr(tc, y, freq, rng):
    y = y - np.nanmean(y)
    peaks = np.empty(NPERM)
    for i in range(NPERM):
        peaks[i] = LombScargle(tc, rng.permutation(y)).power(freq, method="fast").max()
    return np.percentile(peaks, 95)


def _rayleigh(tc):
    """点过程 Rayleigh 周期图 Z=2R²/N(独立粗网格, 周期5~300s); float32 分块省内存。"""
    T = tc.max() - tc.min()
    freq = np.arange(1/300.0, 1/5.0, 1.0/(3*T)).astype(np.float32)
    tcf = tc.astype(np.float32); N = len(tcf)
    Z = np.empty(len(freq), np.float32)
    for s in range(0, len(freq), 300):
        w = (2*np.pi) * freq[s:s+300][:, None] * tcf[None, :]     # (nf,N) float32
        Z[s:s+300] = 2.0/N * (np.cos(w).sum(1)**2 + np.sin(w).sum(1)**2)
        del w
    thr = -2 * np.log(0.05 / len(freq))                           # χ²₂ + Bonferroni
    return 1/freq, Z, thr


def _one(code):
    rng = np.random.default_rng(hash(code) & 0xffff)
    tc, px, vol, flow = _load(code)
    T = tc.max() - tc.min()
    freq = np.arange(1/PMAX, 1/PMIN, 1.0/(SPP*T))
    per = 1/freq
    out = {"code": code, "N": len(tc), "per": per, "freq": freq}
    for nm, y in [("price", px), ("size", vol), ("flow", flow)]:
        pw = _ls(tc, y, freq)
        thr = _placebo_thr(tc, y, freq, rng)
        out[nm] = pw; out[nm+"_thr"] = thr
    out["arr_per"], out["arr"], out["arr_thr"] = _rayleigh(tc)
    return out


def _near(per, pw, P, tol=0.02):
    m = np.abs(per - P) <= P * tol
    return pw[m].max() if m.any() else 0.0


def main():
    t0 = time.time()
    with Pool(len(TICKERS)) as p:
        R = p.map(_one, TICKERS)
    print(f"[逐笔L-S] {len(R)} 只票, 用时 {time.time()-t0:.1f}s\n")

    # 汇总表: 各信号在整数时钟上的功率 / 阈值倍数
    for r in R:
        print(f"■ {r['code']}  N={r['N']}成交")
        for nm, lbl in [("size","量"),("flow","主动流"),("arr","到达"),("price","价")]:
            thr = r[nm+"_thr"]
            perx = r["arr_per"] if nm == "arr" else r["per"]
            cells = []
            for P in CLOCKS:
                v = _near(perx, r[nm], P)
                mark = "*" if v >= thr else " "
                cells.append(f"{P}s:{v/thr:4.1f}{mark}")
            print(f"    {lbl:5s}(阈{thr:8.2e}) " + " ".join(cells))
        print()

    # 图: 每只票一行, 画 到达Rayleigh + 量L-S(最可能带时钟的两条), 标 60/30s
    n = len(R)
    plt.rcParams.update({"font.family":["DejaVu Sans","AR PL UMing CN"],"axes.unicode_minus":False,
        "figure.facecolor":"#fcfcfb","axes.facecolor":"#fcfcfb","text.color":"#0b0b0b",
        "axes.edgecolor":"#e8e7e3","axes.grid":True,"grid.color":"#e8e7e3","grid.alpha":.6,
        "xtick.color":"#0b0b0b","ytick.color":"#0b0b0b","axes.labelcolor":"#0b0b0b",
        "axes.spines.top":False,"axes.spines.right":False})
    fig, axes = plt.subplots(n, 2, figsize=(13, 2.1*n), dpi=110)
    fig.suptitle("逐笔 Lomb-Scargle —— 单股不规则成交时间戳的日内节律(连续竞价, "
                 f"{sorted(glob.glob(DATA + '/cache/2026*'))[-1].split('/')[-1]})",
                 fontsize=13, x=0.5, y=0.995)
    for row, r in enumerate(R):
        for col, (nm, title) in enumerate([("arr","到达点过程 Rayleigh 周期图"), ("size","成交量 L-S")]):
            ax = axes[row, col]
            per = r["arr_per"] if nm == "arr" else r["per"]
            ax.semilogx(per, r[nm], color="#2a78d6", lw=0.8)
            ax.axhline(r[nm+"_thr"], color="#e34948", ls="--", lw=1,
                       label=("95%阈值" if row==0 else None))
            for P in [60, 30]:
                ax.axvline(P, color="#1baf7a", ls=":", lw=1, alpha=.7)
            ax.set_ylabel(f"{r['code']}", fontsize=9)
            if row == 0: ax.set_title(title, fontsize=10.5)
            if row == n-1: ax.set_xlabel("周期(秒, 对数轴; 绿虚线=60s/30s)")
        if row == 0: axes[0,0].legend(frameon=False, fontsize=8, loc="upper left")
    fig.tight_layout(rect=[0,0,1,0.98])
    fig.savefig(BT + "/raw_ls.png", facecolor="#fcfcfb")
    print("[图] bt/raw_ls.png")


if __name__ == "__main__":
    main()
