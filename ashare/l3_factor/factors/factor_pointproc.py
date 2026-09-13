#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
点过程因子(★因果版, 2026-07-22 重写)—— 严格无前视。

前视 bug(旧版, 已废): Bayesian Blocks 块边界与 Hawkes 参数都用【整节次】数据(含未来)
一次性算出, 再当"每分钟的因子值"→ 每个时刻都偷看了未来。

因果修法: 每个决策时刻 t*, 只用【t* 之前 W 分钟】的事件。
  - Hawkes: 滚动窗口 MLE(每 STEP 分钟重拟合一次, 参数变化慢), 得"截止 t* 的" α/β/强度;
  - 暴发检测: 放弃 Bayesian Blocks(全局最优, 天生非因果), 改用【因果的在线速率对比】——
    当前 W_short 窗口速率 vs 过去 W_long 基线速率, 比值即"相对活跃度", switch=比值跨过阈值;
  - 所有窗口只含 t* 之前的事件(< t*, 严格小于), 分钟末因子 = 该时刻的滚动估计。

因子(全部因果):
  hk_branch   : 截止本分钟末、过去 W 分钟拟合的分支比 α(羊群/自激发占比)
  hk_halflife : 同窗 β → 半衰期 ms(记忆时长)
  hk_intens   : 本分钟末瞬时激发强度(用截止当前的参数与历史事件)
  burst_ratio : 近 W_short 速率 / 过去 W_long 基线速率(因果相对活跃度)
  burst_switch: burst_ratio 是否【刚】上穿阈值(平静→暴发, 因果)
  hk_imb      : 本分钟主动流净方向(纯当分钟, 本就无前视)

用法: python factors/factor_pointproc.py --date 20260713 --limit 50 --dump panel.csv
"""
import argparse, glob, os, sys, warnings, time
warnings.filterwarnings("ignore")
from astropy.utils import iers
iers.conf.auto_download = False
import numpy as np, pandas as pd
from agg_events import extract, AM, PM
import hawkes
from qr.paths import DATA

W_FIT = 600          # Hawkes 滚动拟合窗口(秒)
STEP = 300           # 重拟合步长(秒)——参数变化慢, 不必每分钟
W_SHORT = 60         # 暴发检测: 近端窗口
W_LONG = 600         # 暴发检测: 基线窗口
BURST_TH = 2.0       # 相对活跃度阈值
BUCKET = 60


def one_stock(args):
    tic, date, cache = args
    try:
        ev = extract(tic, date, cache)
    except Exception:
        return []
    rows = []
    for name, e in ev.items():
        t = e["t"]; side = e["side"]; vol = e["vol"].astype(float)
        if len(t) < 300:
            continue
        s0 = AM[0] if name == "AM" else PM[0]
        t_end = t[-1]

        # ── 滚动 Hawkes: 在每个 STEP 网格点用【过去 W_FIT】拟合, 存 (grid_t, α, β, μ) ──
        grid = np.arange(t[0] + W_FIT, t_end + STEP, STEP)
        fits = []
        for gt in grid:
            win = t[(t >= gt - W_FIT) & (t < gt)]      # 严格 < gt, 无前视
            if len(win) < 100:
                fits.append((gt, np.nan, np.nan, np.nan, np.nan)); continue
            r = hawkes.fit(win)
            if r.get("ok"):
                fits.append((gt, r["branching"], r["half_life_ms"], r["beta"], r["mu"]))
            else:
                fits.append((gt, np.nan, np.nan, np.nan, np.nan))
        fits = np.array(fits)
        gt_arr = fits[:, 0]

        def latest_fit(tq):
            """取 tq 之前最近一次拟合结果(因果: 只用已算出的网格点)"""
            j = np.searchsorted(gt_arr, tq, side="right") - 1
            return fits[j] if j >= 0 else None

        # ── 分钟聚合 ──
        bkt = (t // BUCKET).astype(np.int64)
        prev_burst = 0
        for b in np.unique(bkt):
            mask = bkt == b
            tb = t[mask]
            last_t = tb[-1]                            # 本分钟末 = 决策时刻
            # 因果暴发检测: 用 last_t 之前的事件
            n_short = np.sum((t >= last_t - W_SHORT) & (t < last_t))
            n_long = np.sum((t >= last_t - W_LONG) & (t < last_t))
            r_short = n_short / W_SHORT
            r_long = n_long / W_LONG if n_long > 0 else np.nan
            burst_ratio = r_short / r_long if (r_long and r_long > 0) else np.nan
            cur_burst = 1 if (burst_ratio is not np.nan and burst_ratio > BURST_TH) else 0
            burst_switch = 1 if (cur_burst == 1 and prev_burst == 0) else 0
            prev_burst = cur_burst
            # Hawkes 因子(截止 last_t 的滚动拟合)
            f = latest_fit(last_t)
            if f is not None and np.isfinite(f[1]):
                branch, halflife, beta, mu = f[1], f[2], f[3], f[4]
                alpha = branch
                his = t[t < last_t]                    # 只用历史事件算强度
                hk_intens = float(hawkes.intensity_at(his, np.array([last_t]), mu, alpha, beta)[0]) if len(his) else np.nan
            else:
                branch = halflife = hk_intens = np.nan
            vb = vol[mask][side[mask] > 0].sum(); vs = vol[mask][side[mask] < 0].sum()
            rows.append((tic, 0 if name == "AM" else 1, int(b),
                         branch, halflife, hk_intens,
                         burst_ratio if burst_ratio is not np.nan else np.nan,
                         burst_switch, (vb - vs) / max(vb + vs, 1)))
    return rows


COLS = ["sym", "sess", "bkt", "hk_branch", "hk_halflife", "hk_intens",
        "burst_ratio", "burst_switch", "hk_imb"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default="20260713")
    ap.add_argument("--cache", default=DATA + "/cache")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=24)
    ap.add_argument("--dump", required=True)
    a = ap.parse_args()
    files = sorted(glob.glob(f"{a.cache}/{a.date}/tl_sz_*.bin"))
    tics = [os.path.basename(f)[-10:-4] for f in files]
    if a.limit:
        tics = tics[:a.limit]
    print(f"[点过程因子·因果版] {len(tics)} 只 SZ, {a.date}", file=sys.stderr)
    from multiprocessing import Pool
    rows = []; t0 = time.time()
    with Pool(a.workers) as pool:
        for i, r in enumerate(pool.imap_unordered(one_stock, [(t, a.date, a.cache) for t in tics], chunksize=2)):
            rows.extend(r)
            if (i+1) % 50 == 0:
                print(f"  {i+1}/{len(tics)}  ({time.time()-t0:.0f}s)", file=sys.stderr)
    df = pd.DataFrame(rows, columns=COLS)
    df.to_csv(a.dump, index=False)
    print(f"[落盘] {len(df)} 行 / {df.sym.nunique()} 股 -> {a.dump} ({time.time()-t0:.0f}s)", file=sys.stderr)


if __name__ == "__main__":
    main()
