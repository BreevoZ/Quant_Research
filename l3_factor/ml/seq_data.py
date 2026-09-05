#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
序列数据集构造 —— 把逐笔事件流切成 [K, F] 张量, 供序列模型直接学习"事件顺序"的信息。

【动机】已有的 22 个因子全是"袋装统计量"(计数/求和/求差), 对一分钟内事件的【先后顺序】
不敏感: "大买→大买→撤单" 与 "撤单→大买→大买" 给出完全相同的因子值。本模块保留顺序。

【窗口: 时间窗, 不是事件数窗】★关键设计
  取分钟末【之前 WIN 秒】内的事件。若用固定事件数(如最后64个), 活跃股只覆盖 2 秒、
  冷清股覆盖 30 分钟(实测差 870 倍), 截面上根本不可比。时间窗保证所有标的信息窗口一致。
  窗口内事件 > K 个 → 只保留【最近的 K 个】(丢更早的); < K 个 → 左侧零填充。

【★时间等距分格 G=32】不再逐事件喂模型, 而是把 WIN 秒切成 G 个等长子区间,
  每格聚合成一个向量。三个好处: ①时间等距(解决"不规则采样喂给假设等距的模型")
  ②格内聚合降噪 ③序列长度 256→32, 训练快 8 倍。代价是丢失格内(1.9秒)的先后, 该尺度顺序信息本就很弱。

【每格 F=9 维(聚合量)】
  0 n_evt_log     该格事件数(log1p)                    —— 活跃度
  1 n_order_r     委托占比 ┐
  2 n_cancel_r    撤单占比 ├ 事件构成
  3 n_trade_r     成交占比 ┘
  4 side_imb      量加权买卖失衡 (买量−卖量)/总量       —— 方向
  5 vol_log       该格总量(log1p, 按当日中位标准化)     —— 规模
  6 px_rel        格末价相对窗口起价 ×1e4 (bp)          —— 价格轨迹
  7 idgap_log     该格订单号跨度(log1p)                 —— 事件时钟(独有维度)
  8 age_norm      该格距窗口末的相对位置 [0,1]          —— 显式"多久之前"(等距, 冗余但无害)

【防前视】只取分钟末之前已发生的事件; 标签用其后的收益(skip-1 口径)。
【标准化】量纲量按"该股当日"统计标准化 → 跨股跨日可比, 且只用当日信息。

输出: data/seq/<date>.npz  含 X[N,K,F](float32), y[N](截面rank), sym, bkt
用法: python ml/seq_data.py <date> [--stocks 400] [--K 256] [--win 60]
"""
import argparse, glob, os, sys
import numpy as np
from qr import l3cache
from qr.paths import DATA

G_DEFAULT, WIN_DEFAULT = 32, 60.0    # G=等距分格数
F = 9
AM = (9*3600+30*60, 11*3600+30*60)
PM = (13*3600, 14*3600+57*60)
MOD = 10**12


def build_stock(fp, G, win):
    """单只股票 → (X[n,G,F], bkts[n], last_px[n]); G 个等距时间格"""
    a = l3cache.load(fp)
    sec = a["timeSeconds"]
    m = ((sec >= AM[0]) & (sec < AM[1])) | ((sec >= PM[0]) & (sec < PM[1]))
    if m.sum() < 500:
        return None
    a = a[m]; sec = sec[m]
    at = a["actionType"]; px = a["price"].astype(np.float64); vol = a["volume"].astype(np.float64)
    is_trd = at == 2
    side = np.where(a["direction"] == b"B", 1.0, -1.0)
    side = np.where(is_trd, np.where(a["buyId"] > a["sellId"], 1.0, -1.0), side)
    sysid = np.where(is_trd, np.maximum(a["buyId"], a["sellId"]), a["sysid"]) % MOD

    n = len(sec)
    dt = np.diff(sec, prepend=sec[0])
    idgap = np.diff(sysid, prepend=sysid[0])
    volm = np.median(vol[vol > 0]) if (vol > 0).any() else 1.0

    base_feat = np.empty((n, F), dtype=np.float32)
    base_feat[:, 0] = np.log1p(np.clip(dt, 0, None))
    base_feat[:, 1] = (at == 0)
    base_feat[:, 2] = (at == 1)
    base_feat[:, 3] = is_trd
    base_feat[:, 4] = side
    base_feat[:, 5] = np.log1p(vol / max(volm, 1e-9))
    base_feat[:, 6] = 0.0                                  # 按窗口填
    base_feat[:, 7] = np.log1p(np.clip(idgap, 0, None))
    base_feat[:, 8] = 0.0                                  # 按窗口填

    bkt = (sec // 60).astype(np.int64)
    ub = np.unique(bkt)
    bkt_end_t = (ub + 1) * 60.0
    step = win / G                                       # 每格时长
    is_ord = (at == 0); is_cxl = (at == 1)
    sv = side * vol                                      # 带符号量

    Xs, bs, lp = [], [], []
    for j, b in enumerate(ub):
        t_end = bkt_end_t[j]
        edges = t_end - win + np.arange(G + 1) * step     # G+1 个格边界
        pos = np.searchsorted(sec, edges, side="left")
        if pos[-1] - pos[0] < 8:                          # 窗口内事件太少
            continue
        seg = np.zeros((G, F), dtype=np.float32)
        pw_all = px[pos[0]:pos[-1]]
        valid = pw_all[pw_all > 0]
        if len(valid) == 0:
            continue
        base = valid[0]
        last_px_seen = base
        for gi in range(G):
            s0, s1 = int(pos[gi]), int(pos[gi+1])
            if s1 > s0:
                cnt = s1 - s0
                v = vol[s0:s1]; vs_ = sv[s0:s1]
                seg[gi, 0] = np.log1p(cnt)
                seg[gi, 1] = is_ord[s0:s1].mean()
                seg[gi, 2] = is_cxl[s0:s1].mean()
                seg[gi, 3] = is_trd[s0:s1].mean()
                tv = v.sum()
                seg[gi, 4] = vs_.sum() / tv if tv > 0 else 0.0
                seg[gi, 5] = np.log1p(tv / max(volm, 1e-9))
                pg = px[s0:s1][px[s0:s1] > 0]
                if len(pg):
                    last_px_seen = pg[-1]
                seg[gi, 7] = np.log1p(max(sysid[s1-1] - sysid[s0], 0))
            seg[gi, 6] = (last_px_seen - base) / base * 1e4        # 格末价(空格沿用上一价)
            seg[gi, 8] = (G - 1 - gi) / (G - 1)                     # 距窗口末的相对位置
        Xs.append(seg); bs.append(b)
        tp = px[pos[0]:pos[-1]][is_trd[pos[0]:pos[-1]] & (px[pos[0]:pos[-1]] > 0)]
        lp.append(tp[-1] if len(tp) else np.nan)
    if not Xs:
        return None
    return np.stack(Xs), np.array(bs), np.array(lp, dtype=np.float64)


def _job(fp, G, win):
    try:
        return os.path.basename(fp)[-10:-4], build_stock(fp, G, win)
    except Exception:
        return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("date"); ap.add_argument("--stocks", type=int, default=400)
    ap.add_argument("--G", type=int, default=G_DEFAULT)
    ap.add_argument("--win", type=float, default=WIN_DEFAULT)
    ap.add_argument("--workers", type=int, default=24)
    a = ap.parse_args()
    files = sorted(glob.glob(f"{DATA}/cache/{a.date}/tl_sz_00*.bin") +
                   glob.glob(f"{DATA}/cache/{a.date}/tl_sz_30*.bin"))[:a.stocks]
    print(f"[{a.date}] {len(files)} 只股票, G={a.G} 格, 时间窗={a.win:.0f}s", file=sys.stderr)

    from multiprocessing import Pool
    with Pool(a.workers) as pool:
        res = pool.starmap(_job, [(fp, a.G, a.win) for fp in files])

    X, syms, bkts, px_by = [], [], [], {}
    for sym, out in res:
        if out is None:
            continue
        Xi, bi, lpi = out
        X.append(Xi); syms += [sym]*len(bi); bkts.append(bi)
        px_by[sym] = dict(zip(bi, lpi))
    if not X:
        sys.exit("无数据")
    X = np.concatenate(X); bkts = np.concatenate(bkts); syms = np.array(syms)

    # 标签: skip-1 口径 → (p[t+2] − p[t+1]) / p[t+1], 再做每分钟截面 rank
    y = np.full(len(X), np.nan)
    for i, (s, b) in enumerate(zip(syms, bkts)):
        d = px_by[s]
        p1, p2 = d.get(b+1, np.nan), d.get(b+2, np.nan)
        if np.isfinite(p1) and np.isfinite(p2) and p1 > 0:
            y[i] = (p2 - p1) / p1
    ok = np.isfinite(y)
    X, y, syms, bkts = X[ok], y[ok], syms[ok], bkts[ok]
    yr = np.full(len(y), np.nan)
    for b in np.unique(bkts):
        m = bkts == b
        if m.sum() >= 30:
            yr[m] = np.argsort(np.argsort(y[m])) / max(m.sum()-1, 1) - 0.5
    ok = np.isfinite(yr)
    os.makedirs(DATA + "/seq", exist_ok=True)
    np.savez_compressed(f"{DATA}/seq/{a.date}.npz", X=X[ok].astype(np.float32),
                        y=yr[ok].astype(np.float32), sym=syms[ok], bkt=bkts[ok])
    # 诊断: 窗口实际覆盖与填充率
    nz = (X[ok][:, :, 0] > 0).sum(axis=1)
    print(f"[{a.date}] 样本 {ok.sum():,} | X{X[ok].shape} (G={a.G} 格 × {a.win/a.G:.2f}s)", file=sys.stderr)
    print(f"  非空格数 中位 {np.median(nz):.0f}/{a.G}", file=sys.stderr)


if __name__ == "__main__":
    main()
