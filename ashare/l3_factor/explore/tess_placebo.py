#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TESS 阴性对照(截面管道校准 · B 方案)

目的:检验我们的【截面因子评估管道】在"结构真实、因果为零"的输入上是否正确报零。
此前的阴性对照只覆盖过时序管道(AGN→指数)与打乱标签(会破坏数据自然结构),
而绝大多数真实因子活在截面管道里(每分钟给 2800 只股票排序)—— 这条从未被校准过。

数据:TESS-SPOC FFI 衍生光变曲线,Sector 85(2024-11~12),约 3300 颗恒星,200 秒频率。
      来自 MAST 官方批量清单(逐星定向查询过慢,天区扫描会在 730KB 处截断)。

★ 零因果由【时间箭头】保证:2024 年的星光在物理上不可能影响 2026 年的 A 股。
  故本方案不做重心修正(BTJD→UTC),仅按【序号顺序】把星光贴到 2026 的交易分钟上。
  代价是失去真实时间对齐,收益是截面规模一次到位(3300 ≈ 真实的 2800),
  而检验力取决于截面规模:N=40 时单桶 IC 标准误 0.16,N=2800 时降到 0.019。

★ 随机映射(星↔股)必须随机:任何按属性排序的映射都可能把星的属性与股的属性焊在一起,
  从而制造出统计上真实、但纯由排序方式产生的相关。随机映射切断这条连线,
  同时保留每颗星自身的时间序列结构(自相关/异方差/星际截面相关全部保留)。

★ 多次映射 → 零分布:单次映射只是从零分布抽一个样本;重复 M 次得到分布,
  可给出"管道本底噪声 σ"与观测值所处百分位,比单点估计的校准力强得多。

用法: python explore/tess_placebo.py [--n-stars 2800] [--n-maps 30] [--days 40]
"""
import argparse, glob, os, sys, warnings
warnings.filterwarnings("ignore")
import numpy as np, pandas as pd
from qr.paths import BT, DATA

FFI = DATA + "/tess_ffi"
HOR = [1, 2, 5, 10, 30]          # 持有期(分钟), 与真实评估一致
AM = (9*60+30, 11*60+30)
PM = (13*60, 14*60+57)


def load_star(fp):
    """读一个 TESS FITS → (亮度变化率序列). 用 PDCSAP(已扣仪器系统误差)。"""
    from astropy.io import fits
    try:
        with fits.open(fp, memmap=False) as h:
            d = h[1].data
            t = np.asarray(d["TIME"], float)
            f = np.asarray(d["PDCSAP_FLUX"], float)
    except Exception:
        return None
    m = np.isfinite(t) & np.isfinite(f) & (f > 0)
    t, f = t[m], f[m]
    if len(t) < 300:
        return None
    mag = -2.5*np.log10(f)
    dt = np.diff(t)*24*60                        # 天 → 分钟
    rate = -np.diff(mag)/np.where(dt > 0, dt, np.nan)     # 每分钟增亮速率
    ok = np.isfinite(rate) & (dt < 30)           # 丢弃跨下传间断的差分
    r = rate[ok]
    if len(r) < 200:
        return None
    lim = np.nanpercentile(np.abs(r), 99.5)      # 去极端跳变
    return r[np.abs(r) <= lim]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n-stars", type=int, default=2800)
    ap.add_argument("--n-maps", type=int, default=30, help="随机映射次数 → 零分布")
    ap.add_argument("--days", type=int, default=40, help="用多少个交易日")
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()

    files = sorted(glob.glob(f"{FFI}/**/*_lc.fits", recursive=True))
    print(f"[1/4] 发现 {len(files)} 个 FITS,读取中 ...", file=sys.stderr)
    stars = []
    for i, fp in enumerate(files):
        if len(stars) >= a.n_stars:
            break
        r = load_star(fp)
        if r is not None:
            stars.append(r)
        if (i+1) % 400 == 0:
            print(f"      {i+1}/{len(files)} → 有效 {len(stars)} 颗", file=sys.stderr)
    if len(stars) < 100:
        sys.exit(f"❌ 有效恒星仅 {len(stars)} 颗,不足以做截面检验")
    print(f"[2/4] 有效恒星 {len(stars)} 颗(每颗 {np.median([len(s) for s in stars]):.0f} 个点)", file=sys.stderr)

    # 市场面板:取最近 N 个交易日
    panels = sorted(glob.glob(DATA + "/panels/panel_id_*.csv"))[-a.days:]
    days = [os.path.basename(p)[9:17] for p in panels]
    print(f"[3/4] 市场面板 {len(days)} 天({days[0]}~{days[-1]}),逐日评估 ...", file=sys.stderr)

    rng = np.random.default_rng(a.seed)
    recs = []
    for di, (p, d) in enumerate(zip(panels, days)):
        mk = pd.read_csv(p, dtype={"sym": str}, usecols=["sym", "sess", "bkt", "last_px"]).dropna()
        mk = mk.sort_values(["sym", "sess", "bkt"])
        g = mk.groupby(["sym", "sess"])["last_px"]
        p1 = g.shift(-1)
        for h in HOR:
            mk[f"r{h}"] = (g.shift(-(1+h)) - p1)/p1
        syms = sorted(mk.sym.unique())
        n = min(len(stars), len(syms))
        buckets = sorted(mk.bkt.unique())
        buckets = [b for b in buckets if (AM[0] <= b < AM[1]) or (PM[0] <= b < PM[1])]

        for mi in range(a.n_maps):
            pick_star = rng.permutation(len(stars))[:n]        # 星↔股随机映射
            pick_sym = rng.permutation(syms)[:n]
            off = rng.integers(0, 10**6)                        # 星光序列的随机起点
            ics = {h: [] for h in HOR}
            for bi, b in enumerate(buckets):
                sub = mk[mk.bkt == b]
                if len(sub) < 200:
                    continue
                # 星光"贴"到该分钟:第 k 颗星取其序列的第 (off+bi) 个点(循环)
                vals = np.array([stars[si][(off+bi) % len(stars[si])] for si in pick_star])
                fac = pd.DataFrame({"sym": pick_sym, "x": vals})
                m2 = sub.merge(fac, on="sym", how="inner")
                if len(m2) < 200:
                    continue
                rx = m2.x.rank()
                for h in HOR:
                    y = m2[f"r{h}"]
                    ok = y.notna()
                    if ok.sum() >= 200:
                        ics[h].append(rx[ok].corr(y[ok].rank()))
            for h in HOR:
                if ics[h]:
                    recs.append({"date": d, "map": mi, "h": h, "ic": float(np.nanmean(ics[h])),
                                 "nbkt": len(ics[h]), "n_xs": n})
        if (di+1) % 5 == 0:
            print(f"      {di+1}/{len(days)} 天", file=sys.stderr)

    r = pd.DataFrame(recs)
    r.to_csv(BT + "/tess_placebo_daily.csv", index=False)

    print("\n" + "="*70)
    print("  TESS 阴性对照:恒星亮度变化率 → A股未来收益(截面 rank-IC)")
    print("="*70)
    print(f"  {len(stars)} 颗恒星(Sector 85, 2024-11) × {len(days)} 个交易日 × {a.n_maps} 次随机映射")
    print(f"  截面规模 N≈{r.n_xs.median():.0f} | 零因果由时间箭头保证(2024 星光 vs 2026 股价)")
    print(f"\n  {'持有期':<8}{'日均IC':>10}{'零分布σ':>10}{'日间t':>9}{'|t|>2的映射':>12}")
    for h in HOR:
        sub = r[r.h == h]
        per_map = sub.groupby("map").ic.mean()          # 每次映射的全期均值
        ts = []
        for mi, gg in sub.groupby("map"):
            v = gg.ic.dropna()
            if len(v) > 2 and v.std(ddof=1) > 0:
                ts.append(v.mean()/v.std(ddof=1)*np.sqrt(len(v)))
        ts = np.array(ts)
        print(f"  {str(h)+' 分钟':<8}{per_map.mean():>+10.5f}{per_map.std():>10.5f}"
              f"{np.mean(ts):>+9.2f}{(np.abs(ts) > 2).mean()*100:>11.0f}%")
    print("="*70)
    print("  预期(管道无缺陷): 日均IC≈0、|日间t|<2、超阈映射比例≈5%(名义显著水平)。")
    print("  零分布σ = 管道在纯噪声输入下的本底波动 —— 真实因子的 IC 必须显著大于它才有意义。")
    print(f"  [明细] bt/tess_placebo_daily.csv")


if __name__ == "__main__":
    main()
