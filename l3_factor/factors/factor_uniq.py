#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
L3 独特因子 v3 —— 六个"只有逐笔才算得出"的特征族(一周试测版)。

①拆单链: 同股同向、订单号近乎连续(gap≤G)、size 相近的委托串 = 母单执行指纹
②对手集中度: 主动方 ID 的赫芬达尔 —— 一条鲸鱼 vs 一群散户
③残量行为: 主动单扫完后残量"挂着(还想要)"vs"撤掉(到此为止)"= 未满足需求存量
④撤单-重挂链: 撤了往前挂(追价, 急迫升级)vs 往后挂(退缩)
⑤簿年龄结构: 当前挂着的买墙 vs 卖墙的平均"岁数"(老墙=真支撑, 新墙=纸墙)
⑥防线韧性: 被对手打掉的价位, 同桶内被己方重新填补的比例

约定同 factor_id: 只 SZ A股/连续段/60s 桶/norm=id%1e12/事件时钟/log1p。
用法: python factors/factor_uniq.py --cache-dir data/cache/<date> --dump panel.csv [--limit N] [--workers 24]
"""
import sys, os, glob, argparse
import numpy as np, pandas as pd
from qr import l3cache

MOD = 10**12
AM = (9*3600+30*60, 11*3600+30*60)
PM = (13*3600,       14*3600+57*60)
CHAIN_GAP = 64        # ①链内相邻订单号最大间隔(channel事件数)
CHAIN_MINLEN = 3      # ①最短链长
CHASE_GAP = 256       # ④撤单→重挂最大间隔(channel事件数)
KEEP_GAP = 5000       # ③残量"保留"判定: 最后主动成交后 5000 事件内未撤
REFILL_CAP = 5.0      # ⑥补墙比例上限(防除小数爆炸)

FEATS = ["chain_share", "chain_imb",          # ①
         "agg_hhi", "agg_hhi_imb",            # ②
         "kept_rem_imb",                      # ③
         "chase_fwd_imb",                     # ④
         "book_age_imb", "book_age_mean",     # ⑤
         "refill_imb"]                        # ⑥
COLS = ["sym", "sess", "bkt"] + FEATS + ["last_px"]


def sess_bkt(sec):
    return np.where(sec < PM[0], 0, 1), (sec // 60).astype(np.int64)


def f_chain(wt):
    """① 拆单链: 按 sysid 排序后找 同向+gap小+size相近 的连续串。返回逐桶 (share, imb)。"""
    w = wt.sort_values("id").reset_index(drop=True)
    gap = w.id.diff()
    same_side = w.side.eq(w.side.shift())
    ratio = np.maximum(w.vol, w.vol.shift()) / np.maximum(1, np.minimum(w.vol, w.vol.shift()))
    brk = (~same_side) | (gap > CHAIN_GAP) | (ratio > 2.0)
    cid = brk.cumsum()
    clen = cid.map(cid.value_counts())
    w["in_chain"] = (clen >= CHAIN_MINLEN).values
    g = w.groupby(["sess", "bkt"])
    tot = g.vol.sum()
    cb = w[w.in_chain & (w.side == 1)].groupby(["sess", "bkt"]).vol.sum()
    cs = w[w.in_chain & (w.side == 0)].groupby(["sess", "bkt"]).vol.sum()
    cb, cs = cb.reindex(tot.index, fill_value=0), cs.reindex(tot.index, fill_value=0)
    share = (cb + cs) / tot.replace(0, np.nan)
    imb = (cb - cs) / (cb + cs).replace(0, np.nan)
    return share, imb


def f_hhi(tr):
    """② 对手集中度: 主动方 ID 的 HHI(量份额平方和), 买卖各一, 取均值与差。"""
    def hhi_side(t):
        if len(t) == 0:
            return pd.Series(dtype=float)
        s = t.groupby(["sess", "bkt", "aggid"]).vol.sum()
        tot = s.groupby(["sess", "bkt"]).transform("sum")
        return ((s / tot) ** 2).groupby(["sess", "bkt"]).sum()
    hb = hhi_side(tr[tr.baggr]); hs = hhi_side(tr[~tr.baggr])
    idx = hb.index.union(hs.index)
    hb, hs = hb.reindex(idx), hs.reindex(idx)
    return (hb + hs) / 2, hb - hs


def f_kept_rem(tr, wt, cl):
    """③ 残量行为: 主动单 (wt量−总成交量)>0 且未及时撤 = 保留残量; 按方向失衡。"""
    fills = pd.concat([tr[["buyId", "vol"]].rename(columns={"buyId": "id"}),
                       tr[["sellId", "vol"]].rename(columns={"sellId": "id"})])
    filled = fills.groupby("id").vol.sum()
    ag = tr.assign(aggid2=np.where(tr.baggr, tr.buyId, tr.sellId))
    last_ag = ag.groupby("aggid2").agg(last_seq=("seq", "max"), sess=("sess", "last"), bkt=("bkt", "last"))
    w = wt.set_index("id")
    last_ag = last_ag.join(w[["vol", "side"]], how="inner")          # 有委托记录的主动单
    last_ag["rem"] = (last_ag.vol - filled.reindex(last_ag.index).fillna(0)).clip(lower=0)
    cl1 = cl.groupby("id").seq.min()
    cseq = cl1.reindex(last_ag.index)
    kept = (last_ag.rem > 0) & (cseq.isna() | (cseq - last_ag.last_seq > KEEP_GAP))
    k = last_ag[kept]
    kb = k[k.side == 1].groupby(["sess", "bkt"]).rem.sum()
    ks = k[k.side == 0].groupby(["sess", "bkt"]).rem.sum()
    idx = kb.index.union(ks.index)
    kb, ks = kb.reindex(idx, fill_value=0), ks.reindex(idx, fill_value=0)
    return (kb - ks) / (kb + ks).replace(0, np.nan)


def f_chase(cl, wt):
    """④ 撤单→重挂: 同向、gap≤CHASE_GAP、size相近; 买更高/卖更低=追价(fwd)。
    ⚠ SZ 撤单记录 price=0(占位), 原价必须用撤单 id 回查委托记录。"""
    opx = wt.set_index("id").px
    out = {}
    for side in (1, 0):
        c = cl[cl.side == side].copy()
        c["opx"] = c.id.map(opx)
        c = c.dropna(subset=["opx"]).sort_values("seq")
        w = wt[wt.side == side].sort_values("id")
        if len(c) == 0 or len(w) == 0:
            continue
        m = pd.merge_asof(c[["seq", "opx", "vol"]], w[["id", "px", "vol", "sess", "bkt"]].rename(
            columns={"id": "wseq", "px": "wpx", "vol": "wvol"}),
            left_on="seq", right_on="wseq", direction="forward")
        ok = (m.wseq - m.seq <= CHASE_GAP) & (m.wvol / m.vol).between(0.7, 1.4) & (m.wpx != m.opx)
        m = m[ok]
        fwd = (m.wpx > m.opx) if side == 1 else (m.wpx < m.opx)
        out[side] = m[fwd].groupby(["sess", "bkt"]).wvol.sum()
    fb = out.get(1, pd.Series(dtype=float)); fs = out.get(0, pd.Series(dtype=float))
    idx = fb.index.union(fs.index)
    fb, fs = fb.reindex(idx, fill_value=0), fs.reindex(idx, fill_value=0)
    return (fb - fs) / (fb + fs).replace(0, np.nan)


def f_book_age(wt, cl, tr, bounds):
    """⑤ 簿年龄结构: 每桶末, 存活挂单的量加权年龄(事件时钟), 买卖对比。
    死亡 = 撤单时刻 或 累计成交≥委托量的时刻。O(n) 事件扫描 + 边界快照。"""
    fills = pd.concat([tr[["buyId", "vol", "seq"]].rename(columns={"buyId": "id"}),
                       tr[["sellId", "vol", "seq"]].rename(columns={"sellId": "id"})]).sort_values("seq")
    fills["cum"] = fills.groupby("id").vol.cumsum()
    w = wt.set_index("id")
    full = fills[fills.cum >= fills.id.map(w.vol).fillna(np.inf)].groupby("id").seq.min()
    cfirst = cl.groupby("id").seq.min()
    death = pd.concat([full, cfirst], axis=1).min(axis=1)
    res = {}
    for side in (1, 0):
        ws = w[w.side == side]
        ev = pd.DataFrame({
            "seq": np.concatenate([ws.index.values, death.reindex(ws.index).dropna().index.map(death).values]),
            "dv":  np.concatenate([ws.vol.values, -ws.vol.reindex(death.reindex(ws.index).dropna().index).values]),
            "dvb": np.concatenate([ws.vol.values * ws.index.values,
                                   -(ws.vol.reindex(death.reindex(ws.index).dropna().index)
                                     * death.reindex(ws.index).dropna().index).values]),
        }).sort_values("seq")
        cv, cvb = ev.dv.cumsum().values, ev.dvb.cumsum().values
        pos = np.searchsorted(ev.seq.values, bounds.bseq.values, side="right") - 1
        alive = np.where(pos >= 0, cv[np.clip(pos, 0, None)], 0.0)
        aliveb = np.where(pos >= 0, cvb[np.clip(pos, 0, None)], 0.0)
        age = np.where(alive > 0, (bounds.bseq.values * alive - aliveb) / np.maximum(alive, 1), np.nan)
        res[side] = np.log1p(np.maximum(age, 0))
    imb = res.get(1, np.nan) - res.get(0, np.nan)
    mean = (res.get(1, np.nan) + res.get(0, np.nan)) / 2
    return pd.Series(imb, index=bounds.set_index(["sess", "bkt"]).index), \
           pd.Series(mean, index=bounds.set_index(["sess", "bkt"]).index)


def f_refill(tr, wt):
    """⑥ 防线韧性: 本桶被对手方吃掉的价位, 同桶内被己方新委托填回的比例, 买卖对比。"""
    out = {}
    for side, aggr in ((1, False), (0, True)):     # side=1 买墙被卖方主动(aggr=False→sellId大? 用baggr)
        eaten = tr[tr.baggr != (side == 1)]        # 买墙被吃 = 卖方主动(baggr=False)
        if len(eaten) == 0:
            continue
        e = eaten.groupby(["sess", "bkt", "ip"]).vol.sum().rename("eat")
        wme = wt[wt.side == side].groupby(["sess", "bkt", "ip"]).vol.sum().rename("re")
        j = pd.concat([e, wme], axis=1, join="inner")
        r = j.groupby(["sess", "bkt"]).apply(
            lambda g: min((g.re.sum() / max(g.eat.sum(), 1)), REFILL_CAP), include_groups=False)
        out[side] = r
    rb = out.get(1, pd.Series(dtype=float)); ra = out.get(0, pd.Series(dtype=float))
    idx = rb.index.union(ra.index)
    return rb.reindex(idx) - ra.reindex(idx)


def features_one(sym, a, bucket=60):
    sec = a["timeSeconds"]
    m = ((sec >= AM[0]) & (sec < AM[1])) | ((sec >= PM[0]) & (sec < PM[1]))
    a = a[m]; sec = sec[m]
    if len(a) == 0:
        return None
    at = a["actionType"]; seqn = a["seqNo"] % MOD
    ss, bb = sess_bkt(sec)
    base = dict(sess=ss, bkt=bb, seq=seqn)
    isO, isC = at == 0, at == 1
    isT = (at == 2) & (a["buyId"] > 0) & (a["sellId"] > 0)
    wt = pd.DataFrame({**{k: v[isO] for k, v in base.items()},
                       "id": a["sysid"][isO], "side": (a["direction"][isO] == b"B").astype(int),
                       "vol": a["volume"][isO], "px": a["price"][isO], "ip": a["intPrice"][isO]})
    cl = pd.DataFrame({**{k: v[isC] for k, v in base.items()},
                       "id": a["sysid"][isC], "side": (a["direction"][isC] == b"B").astype(int),
                       "vol": a["volume"][isC], "px": a["price"][isC]})
    tr = pd.DataFrame({**{k: v[isT] for k, v in base.items()},
                       "buyId": a["buyId"][isT], "sellId": a["sellId"][isT],
                       "vol": a["volume"][isT], "px": a["price"][isT], "ip": a["intPrice"][isT]})
    tr["baggr"] = tr.buyId > tr.sellId
    tr["aggid"] = np.where(tr.baggr, tr.buyId, tr.sellId)
    if len(tr) == 0 and len(wt) == 0:
        return None

    # 桶边界(供⑤): 每 (sess,bkt) 的末事件 seq 与末成交价
    allb = pd.DataFrame({"sess": ss, "bkt": bb, "seq": seqn})
    bounds = allb.groupby(["sess", "bkt"], as_index=False).seq.max().rename(columns={"seq": "bseq"})
    lastpx = tr.groupby(["sess", "bkt"]).px.last()

    idx = pd.MultiIndex.from_frame(bounds[["sess", "bkt"]])
    out = pd.DataFrame(index=idx)
    def put(name, s):
        out[name] = s.reindex(idx) if isinstance(s, pd.Series) else s
    try: sh, im = f_chain(wt); put("chain_share", sh); put("chain_imb", im)
    except Exception: out["chain_share"] = out["chain_imb"] = np.nan
    try: h, hi = f_hhi(tr); put("agg_hhi", h); put("agg_hhi_imb", hi)
    except Exception: out["agg_hhi"] = out["agg_hhi_imb"] = np.nan
    try: put("kept_rem_imb", f_kept_rem(tr, wt, cl))
    except Exception: out["kept_rem_imb"] = np.nan
    try: put("chase_fwd_imb", f_chase(cl, wt))
    except Exception: out["chase_fwd_imb"] = np.nan
    try:
        bi, bm = f_book_age(wt, cl, tr, bounds)
        put("book_age_imb", bi); put("book_age_mean", bm)
    except Exception: out["book_age_imb"] = out["book_age_mean"] = np.nan
    try: put("refill_imb", f_refill(tr, wt))
    except Exception: out["refill_imb"] = np.nan
    put("last_px", lastpx)
    out = out.reset_index()
    out.insert(0, "sym", sym)
    return [tuple(r) for r in out[COLS].itertuples(index=False, name=None)]


def process(args):
    fp, bucket = args
    sym = os.path.basename(fp)[-10:-4]
    try:
        return features_one(sym, l3cache.load(fp), bucket) or []
    except Exception as e:
        sys.stderr.write(f"[skip {fp}: {e}]\n"); return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=24)
    a = ap.parse_args()
    files = sorted(glob.glob(f"{a.cache_dir}/flow_sz_*.bin")) or \
            sorted(glob.glob(f"{a.cache_dir}/tl_sz_*.bin"))
    if a.limit:
        files = files[:a.limit]
    print(f"[uniq] {len(files)} 只", file=sys.stderr)
    rows = []
    from multiprocessing import Pool
    with Pool(a.workers) as pool:
        for r in pool.imap_unordered(process, [(f, 60) for f in files], chunksize=8):
            rows.extend(r)
    feat = pd.DataFrame(rows, columns=COLS).sort_values(["sym", "sess", "bkt"]).reset_index(drop=True)
    feat["last_px"] = feat.groupby(["sym", "sess"])["last_px"].ffill()
    g = feat.groupby(["sym", "sess"])["last_px"]
    p0 = feat["last_px"]; p1 = g.shift(-1); p2 = g.shift(-2)
    feat["ret_next"] = (p1 - p0) / p0
    feat["ret_skip"] = (p2 - p1) / p1
    feat.to_csv(a.dump + ".tmp", index=False)
    os.replace(a.dump + ".tmp", a.dump)
    print(f"[落盘] {len(feat)} 行 -> {a.dump}", file=sys.stderr)


if __name__ == "__main__":
    main()
