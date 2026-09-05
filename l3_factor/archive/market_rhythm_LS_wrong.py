#!/usr/bin/env python3
# ⚠️ 已废弃(2026-07-27): 结论错——报"5分钟主峰", 正确应为"1分钟(19×)/30秒(13×)主导, 5分钟仅4.4×配角"。
#    ── 病因(经对照实验坐实, 已更正之前的误判)──
#    真凶: 手工指定的频率网格 periods=geomspace(5,1800,400) 太粗(60s 附近步长~0.87s)。
#          整数时钟峰是针状窄峰(周期轴上仅~0.25s宽), 粗网格踩不到 60.00s 只测到肩膀,
#          功率丢 92%(30s 丢 95%); 而 5分钟是宽包, 只丢 23% → 尖峰被误伤、宽包幸存, 排名翻车。
#          换成对齐 FFT 的细网格, L-S 结果与 FFT 一字不差 → 这是"栅栏效应/峰落网格缝"(picket-fence),
#          用 FFT 喂同样粗频点也会犯, 与 L-S 无关。
#    非病因: mask=(resid!=0) 剔零点。对照实验显示去不去 mask 主峰都是 302s, mask 对排名无影响。
#    数据本已聚合成【每秒规则序列】, 本就该直接用 FFT(全分辨率), L-S 在此无优势。
#    正确版见 market_rhythm.py / plot_rhythm.py。保留此文件作"频率轴要采够密"的反面教材。
# -*- coding: utf-8 -*-
"""
市场隐藏节律 —— 用天文的 Lomb-Scargle 周期图找 A 股日内成交强度的周期成分。

天文里 L-S 是找变星周期的标准工具(专治不规则采样)。这里问: 全市场成交强度除了
已知的开盘/收盘 U 型, 有没有藏着更细的日内节律(如整点脉动、固定分钟频率的波动)?

方法:
  ① 全市场每秒总成交笔数 → 日内强度序列(多日叠加, 去掉每日均值消除 U 型基线)
  ② 对残差做 L-S 周期图, 扫描周期 5秒 ~ 30分钟
  ③ 峰值 = 候选节律; 用置换检验(打乱时间)定显著性阈值
输出: bt/rhythm.png(周期图 + 日内平均曲线)
"""
import glob, sys
import numpy as np
from qr import l3cache
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from qr.paths import BT, DATA

AM = (9*3600+30*60, 11*3600+30*60)
PM = (13*3600, 14*3600+57*60)
SUR = "#fcfcfb"; INK = "#0b0b0b"; BLUE = "#2a78d6"; RED = "#e34948"; GRID = "#e8e7e3"


def _read_trades(fp):
    a = l3cache.load(fp); s = a["timeSeconds"]; at = a["actionType"]
    m = (at == 2) & (((s >= AM[0]) & (s < AM[1])) | ((s >= PM[0]) & (s < PM[1])))
    return s[m]


def day_intensity(date, nstocks=300):
    """该日全市场每秒成交笔数(连续段), 返回 sec_of_session(从09:30=0起, 挤掉午休), counts"""
    files = sorted(glob.glob(f"{DATA}/cache/{date}/tl_sz_00*.bin") +
                   glob.glob(f"{DATA}/cache/{date}/tl_sz_30*.bin"))[:nstocks]
    from multiprocessing import Pool
    with Pool(24) as pool:
        parts = pool.map(_read_trades, files)
    allsec = [p for p in parts if p is not None and len(p)]
    if not allsec:
        return None, None
    sec = np.concatenate(allsec)
    # 会话内秒(挤掉午休): 上午 [0,7200), 下午接 [7200,...)
    ss = np.where(sec < PM[0], sec - AM[0], sec - PM[0] + (AM[1]-AM[0]))
    T = (AM[1]-AM[0]) + (PM[1]-PM[0])
    bins = np.arange(0, T+1, 1.0)
    cnt, _ = np.histogram(ss, bins=bins)
    return bins[:-1], cnt.astype(float)


def main():
    dates = [d.split("/")[-1] for d in sorted(glob.glob(DATA + "/cache/2026*"))][:20]
    print(f'[节律] 将处理 {len(dates)} 天', file=sys.stderr)
    print(f"[节律] {len(dates)} 天全市场逐秒成交强度", file=sys.stderr)
    prof = None; nd = 0
    for d in dates:
        t, c = day_intensity(d)
        if c is None:
            continue
        prof = c if prof is None else prof + c
        nd += 1
        print(f'  {d} ({nd}/{len(dates)})', file=sys.stderr)
    prof /= nd                                              # 日内平均强度曲线(含 U 型)
    t = np.arange(len(prof))
    # 去 U 型基线: 减去平滑趋势(移动平均), 留下高频波动
    win = 300
    kernel = np.ones(win)/win
    base = np.convolve(prof, kernel, mode="same")
    resid = prof - base

    from scipy.signal import lombscargle
    periods = np.geomspace(5, 1800, 400)                    # 5秒 ~ 30分钟
    ang = 2*np.pi / periods
    mask = resid != 0
    pgram = lombscargle(t[mask].astype(float), resid[mask], ang, normalize=True)

    # 置换检验: 打乱残差, 求纯噪声下峰值分布 → 95% 阈值
    rng = np.random.default_rng(0)
    null_peaks = []
    for _ in range(200):
        pg = lombscargle(t[mask].astype(float), rng.permutation(resid[mask]), ang, normalize=True)
        null_peaks.append(pg.max())
    thr = np.percentile(null_peaks, 95)

    # 找显著峰
    from scipy.signal import find_peaks
    pk, _ = find_peaks(pgram, height=thr)
    pk = pk[np.argsort(-pgram[pk])][:6]
    print("\n显著周期成分(超过置换检验 95% 阈值):", file=sys.stderr)
    for i in pk:
        p = periods[i]
        lbl = f"{p:.0f}秒" if p < 60 else f"{p/60:.1f}分钟"
        print(f"  周期 {lbl:8s} 功率 {pgram[i]:.3f} (阈值 {thr:.3f})", file=sys.stderr)

    # 画图
    plt.rcParams.update({"font.family": ["DejaVu Sans", "AR PL UMing CN"], "axes.unicode_minus": False,
                         "figure.facecolor": SUR, "axes.facecolor": SUR, "text.color": INK,
                         "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID,
                         "xtick.color": INK, "ytick.color": INK, "axes.labelcolor": INK,
                         "axes.spines.top": False, "axes.spines.right": False})
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), dpi=110)
    fig.suptitle(f"A 股日内成交节律 — Lomb-Scargle 周期图({nd} 天全市场叠加)",
                 fontsize=13, x=0.09, ha="left", color=INK)
    # 上: 日内平均强度
    ax1.plot(t/60, prof, color=BLUE, lw=1.2)
    ax1.axvline((AM[1]-AM[0])/60, color=GRID, lw=1.5)
    ax1.set_title("日内平均成交强度(笔/秒, 已挤掉午休)", loc="left", fontsize=10.5)
    ax1.set_xlabel("会话分钟"); ax1.set_ylabel("笔/秒")
    # 下: 周期图
    ax2.semilogx(periods, pgram, color=BLUE, lw=1.3)
    ax2.axhline(thr, color=RED, ls="--", lw=1.2, label=f"置换检验 95% 阈值 {thr:.2f}")
    for i in pk:
        p = periods[i]; lbl = f"{p:.0f}s" if p < 60 else f"{p/60:.0f}min"
        ax2.annotate(lbl, (p, pgram[i]), color=RED, fontsize=9, ha="center", va="bottom")
    ax2.set_title("周期图: 去 U 型基线后的残差(峰=隐藏节律)", loc="left", fontsize=10.5)
    ax2.set_xlabel("周期(秒, 对数轴)"); ax2.set_ylabel("归一化功率")
    ax2.legend(frameon=False, fontsize=9)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(BT + "/rhythm.png", facecolor=SUR)
    print("[图] bt/rhythm.png", file=sys.stderr)


if __name__ == "__main__":
    main()
