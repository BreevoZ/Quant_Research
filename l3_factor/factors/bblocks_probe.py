#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Bayesian Blocks 单股冒烟 + 性能验证。
优化: ①先按毫秒聚合成交(N 砍一量级) ②astropy 优化实现(Scargle 动态规划)
     ③按节次分段(降 N² 常数) ④ncp_prior 调高抑制过度切分。
先验证单股可行性与暴发结构, 再谈批量。
"""
import sys, time, warnings
warnings.filterwarnings("ignore")
from astropy.utils import iers
iers.conf.auto_download = False
import numpy as np
from qr import l3cache
from astropy.stats import bayesian_blocks
from qr.paths import DATA

CACHE = DATA + "/cache/20260713"
AM = (9*3600+30*60, 11*3600+30*60)
PM = (13*3600, 14*3600+57*60)


def run(tic):
    a = l3cache.load(f"{CACHE}/tl_sz_{tic}.bin")
    sec = a["timeSeconds"]; at = a["actionType"]; vol = a["volume"]
    out = {}
    for name, (s0, s1) in (("AM", AM), ("PM", PM)):
        m = (at == 2) & (sec >= s0) & (sec < s1)
        t = sec[m]; w = vol[m].astype(float)
        if len(t) < 100:
            continue
        # ① 毫秒聚合: 同毫秒成交量相加 → 带权事件
        ms = np.round(t * 1000).astype(np.int64)
        uniq, inv = np.unique(ms, return_inverse=True)
        tw = uniq / 1000.0
        ww = np.zeros(len(uniq)); np.add.at(ww, inv, w)
        n0 = len(t); n1 = len(tw)
        # ④ ncp_prior 抑制过度切分(默认对应 p0=0.05, 这里手动设高一点)
        t0 = time.time()
        edges = bayesian_blocks(tw, ww, fitness="events", ncp_prior=8.0)
        dt = time.time() - t0
        nblk = len(edges) - 1
        # 每块的速率(事件数/秒), 看暴发对比
        idx = np.searchsorted(edges, tw, side="right") - 1
        idx = np.clip(idx, 0, nblk - 1)
        blk_n = np.bincount(idx, minlength=nblk)
        blk_dur = np.diff(edges)
        rate = blk_n / np.maximum(blk_dur, 1e-6)
        out[name] = (n0, n1, nblk, dt, rate)
        print(f"  [{name}] 原始 {n0:,} 笔 → 毫秒聚合 {n1:,} → {nblk} 个块 | "
              f"耗时 {dt:.2f}s | 速率 min/中/max = {rate.min():.1f}/{np.median(rate):.1f}/{rate.max():.1f} 事件/秒",
              file=sys.stderr)
    return out


if __name__ == "__main__":
    tic = sys.argv[1] if len(sys.argv) > 1 else "000001"
    print(f"=== Bayesian Blocks 冒烟: {tic} (20260713) ===", file=sys.stderr)
    t0 = time.time()
    run(tic)
    print(f"=== 单股总耗时 {time.time()-t0:.1f}s ===", file=sys.stderr)
