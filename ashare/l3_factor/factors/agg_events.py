#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
主动单事件提取 —— Hawkes / Bayesian Blocks 的公共预处理。

原始成交有"同笔主动单扫穿多档 → 多条成交记录、同一时刻"的会计碎片,
直接当点过程会污染(零间隔=无穷强度)。这里用【订单号】把同一笔主动单的
多条成交合并回【一个到达事件】,并只保留主动方(自激发的因果源头)。

产出每个主动单事件: (t 到达时刻, side 买/卖, vol 总成交量, npart 吃掉几个对手, aggid)
SZ 规则: 成交双方 ID 中【大号=后到=主动方】; buyId>sellId → 买方主动。

用法: python factors/agg_events.py <TIC> [date]
"""
import sys
import numpy as np
from qr import l3cache
from qr.paths import DATA

AM = (9*3600+30*60, 11*3600+30*60)
PM = (13*3600, 14*3600+57*60)


def extract(tic, date="20260713", cache=DATA + "/cache"):
    a = l3cache.load(f"{cache}/{date}/tl_sz_{tic}.bin")
    sec = a["timeSeconds"]; at = a["actionType"]
    buyId = a["buyId"]; sellId = a["sellId"]; vol = a["volume"]
    out = {}
    for name, (s0, s1) in (("AM", AM), ("PM", PM)):
        m = (at == 2) & (sec >= s0) & (sec < s1) & (buyId > 0) & (sellId > 0)
        if m.sum() < 100:
            continue
        t = sec[m]; bi = buyId[m]; si = sellId[m]; v = vol[m].astype(np.int64)
        aggr_is_buy = bi > si
        aggid = np.where(aggr_is_buy, bi, si)     # 主动单订单号

        # 按主动单ID聚合: 到达时刻取该单首笔成交(最早), 量求和, 方向一致
        order = np.argsort(aggid, kind="stable")
        aggid_s = aggid[order]; t_s = t[order]; v_s = v[order]; buy_s = aggr_is_buy[order]
        # 分组边界
        uniq, first_idx, counts = np.unique(aggid_s, return_index=True, return_counts=True)
        # 每组最早时刻(首笔成交时刻) —— 该主动单"到达"市场的时刻
        t_arr = np.minimum.reduceat(t_s, first_idx)
        v_sum = np.add.reduceat(v_s, first_idx)
        side_buy = buy_s[first_idx]               # 同一主动单方向一致
        npart = counts

        # 按到达时刻重新排序
        o2 = np.argsort(t_arr, kind="stable")
        t_arr = t_arr[o2]; side_i = np.where(side_buy[o2], 1, -1)
        v_sum = v_sum[o2]; npart = npart[o2]

        # ★ 毫秒去重: 毫秒是本数据的物理时间分辨率, 同毫秒的多笔主动单先后【未被记录】,
        #   合并成"这一毫秒的主动流"是承认精度上限(而非微抖动凭空造亚毫秒结构 → 污染 Hawkes)。
        #   量相加; 方向取量加权净方向(买量-卖量>0 记买); npart 累加。
        ms = np.round(t_arr * 1000).astype(np.int64)
        um, fi, cc = np.unique(ms, return_index=True, return_counts=True)
        vbuy = np.add.reduceat(np.where(side_i > 0, v_sum, 0), fi)
        vsell = np.add.reduceat(np.where(side_i < 0, v_sum, 0), fi)
        ev = dict(t=um / 1000.0,
                  side=np.where(vbuy >= vsell, 1, -1),
                  vol=vbuy + vsell,
                  imb=(vbuy - vsell) / np.maximum(vbuy + vsell, 1),   # 该毫秒主动流净方向强度
                  npart=np.add.reduceat(npart, fi),
                  nmerge=cc)                                          # 同毫秒并了几笔主动单
        out[name] = ev
    return out


if __name__ == "__main__":
    tic = sys.argv[1] if len(sys.argv) > 1 else "000001"
    date = sys.argv[2] if len(sys.argv) > 2 else "20260713"
    ev = extract(tic, date)
    for name, e in ev.items():
        t = e["t"]; dt = np.diff(t)
        nb = (e["side"] == 1).sum()
        print(f"[{name}] {len(t):,} 个毫秒主动事件 | 买 {nb/len(t)*100:.0f}%/卖 {100-nb/len(t)*100:.0f}% | "
              f"间隔 中位 {np.median(dt)*1000:.0f}ms p90 {np.percentile(dt,90)*1000:.0f}ms 最长 {dt.max():.0f}s | "
              f"零间隔 {(dt==0).mean()*100:.1f}% | 平均每毫秒事件并 {e['nmerge'].mean():.1f} 笔主动单")
