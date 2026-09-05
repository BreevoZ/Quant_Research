#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
⚠ 已归档(2026-07-16): 本文件的特征已并入 factor_id.py(统一抽取器, 冗余 ord_rate 已删)。
   保留仅作历史参照, 不再维护。

L3 订单流因子 —— 全 SZ 池, 截面 rank-IC(equity factor 标准评估)。

数据: vsim 全市场 flow 二进制缓存(见 l3cache), numpy 直读, 不重扫 58GB 源。
特征: 订单密集度 + 订单大小(师傅两个) + 撤单强度。全部只用委托流事件, 暂不解引用订单号。
评估: 每时间桶【横截面】spearman(feat, 未来收益) → 对桶均值; ret_next 紧邻 / ret_skip 跳1桶。
纪律: 只 SZ、只连续段、收益不跨节次; 单日只是"截面形态", 多日才是定论(见 README)。

用法:
  python factor_flow.py [--limit N] [--bucket 60] [--dump panel.csv]
"""
import sys, os, glob, argparse
import numpy as np, pandas as pd
from qr import l3cache
from qr import paths

# —— 缓存位置(⚠ 上一轮 precache 落在 scratchpad 的临时产物, 只对 20260707 有效, 随时可能被清)——
CACHE_DIR = str(paths.data("cache", "20260707"))

AM = (9*3600+30*60, 11*3600+30*60)      # 早盘连续段
PM = (13*3600,       14*3600+57*60)      # 午盘连续段(收盘竞价 14:57 后不计)

FEATS = ["n_ord", "ord_rate", "ord_imb_cnt", "mean_size", "ord_imb_vol", "cxl_ratio", "large_share"]
COLS  = ["sym", "sess", "bkt"] + FEATS + ["last_px"]


def features_one(sym, a, bucket):
    """单只股票的逐事件数组 -> 每(节次,桶)一行特征。"""
    sec = a["timeSeconds"]
    m = ((sec >= AM[0]) & (sec < AM[1])) | ((sec >= PM[0]) & (sec < PM[1]))
    a = a[m]; sec = sec[m]
    if len(a) == 0:
        return None
    sess = np.where(sec < PM[0], 0, 1)                     # 0=AM 1=PM
    bkt = (sec // bucket).astype(np.int64)
    at = a["actionType"]; d = a["direction"]; vol = a["volume"]; px = a["price"]
    isO = at == 0                                          # 挂单
    thr = np.quantile(vol[isO], 0.90) if isO.any() else 0  # 该股全日大单阈值(轻微前视, 探索期可接受)
    df = pd.DataFrame({"sess": sess, "bkt": bkt, "isB": d == b"B", "vol": vol, "px": px,
                       "isO": isO, "isC": at == 1, "isT": at == 2, "big": vol >= thr})
    out = []
    for (s, b), g in df.groupby(["sess", "bkt"]):
        o = g[g.isO]; nO = len(o)
        if nO == 0 and not g.isT.any():
            continue
        nb = int(o.isB.sum()); ns = nO - nb
        vb = int(o.vol[o.isB].sum()); vs = int(o.vol[~o.isB].sum())
        tg = g[g.isT]
        out.append((sym, int(s), int(b),
            nO, nO / bucket,                                       # 密集度: 笔数, 到达率
            (nb - ns) / (nb + ns) if nb + ns else 0.0,             # 买卖笔数失衡
            o.vol.mean() if nO else 0.0,                           # 平均单笔委托量
            (vb - vs) / (vb + vs) if vb + vs else 0.0,             # 买卖【量】失衡
            int(g.isC.sum()) / nO if nO else 0.0,                  # 撤单强度
            (o.vol[o.big].sum() / o.vol.sum()) if nO and o.vol.sum() > 0 else 0.0,  # 大单量占比
            tg.px.iloc[-1] if len(tg) else np.nan))                # 桶末最后成交价(建标签)
    return out


def process(args):
    fp, bucket = args
    sym = os.path.basename(fp)[8:14]
    try:
        return features_one(sym, l3cache.load(fp), bucket) or []
    except Exception as e:
        sys.stderr.write(f"[skip {fp}: {e}]\n"); return []


def xs_ic(feat, col, lab):
    """截面 rank-IC: 每(sess,bkt)横截面 spearman, 再对桶收集。"""
    ics = []
    for _, g in feat.groupby(["sess", "bkt"]):
        sub = g[[col, lab]].dropna()
        if len(sub) < 50:                                  # 每桶至少 50 只才算截面
            continue
        ics.append(sub[col].rank().corr(sub[lab].rank()))
    ics = np.array(ics)
    return ics.mean(), ics.std(), len(ics)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0, help="只跑前 N 只(测试); 0=全部")
    ap.add_argument("--bucket", type=int, default=60, help="秒/桶")
    ap.add_argument("--dump", default="", help="把特征面板落盘到该 CSV(多日累加用)")
    ap.add_argument("--workers", type=int, default=32)
    a = ap.parse_args()

    files = sorted(glob.glob(f"{CACHE_DIR}/flow_sz_*.bin"))
    if a.limit:
        files = files[:a.limit]
    print(f"[读] {len(files)} 只 SZ, bucket={a.bucket}s", file=sys.stderr)

    rows = []
    from multiprocessing import Pool
    with Pool(a.workers) as pool:
        for i, r in enumerate(pool.imap_unordered(process, [(f, a.bucket) for f in files], chunksize=8)):
            rows.extend(r)
            if (i + 1) % 400 == 0:
                print(f"  {i+1}/{len(files)}", file=sys.stderr)
    feat = pd.DataFrame(rows, columns=COLS)
    print(f"[面板] {len(feat)} 行, {feat.sym.nunique()} 股", file=sys.stderr)

    # 未来收益(节次内 shift, 不跨节次)
    feat = feat.sort_values(["sym", "sess", "bkt"]).reset_index(drop=True)
    feat["last_px"] = feat.groupby(["sym", "sess"])["last_px"].ffill()
    g = feat.groupby(["sym", "sess"])["last_px"]
    p0 = feat["last_px"]; p1 = g.shift(-1); p2 = g.shift(-2)
    feat["ret_next"] = (p1 - p0) / p0
    feat["ret_skip"] = (p2 - p1) / p1

    if a.dump:
        feat.to_csv(a.dump, index=False)
        print(f"[落盘] 特征面板 -> {a.dump}", file=sys.stderr)

    print("\n============ 截面 rank-IC(全 SZ, 单日)============")
    print(f"{'feature':<13} {'lab':<9} {'meanIC':>8} {'IC_std':>8} {'IR':>7} {'t':>7} {'nbkt':>5}")
    for f in FEATS:
        for lab in ["ret_next", "ret_skip"]:
            m, s, n = xs_ic(feat, f, lab)
            ir = m / s if s > 0 else 0
            print(f"{f:<13} {lab:<9} {m:>8.4f} {s:>8.4f} {ir:>7.3f} {ir*np.sqrt(n):>7.2f} {n:>5}")
    print("\n⚠ 单日: t 是桶间的、桶高度自相关 → 偏乐观; 真检验需多日(见 README)。")


if __name__ == "__main__":
    main()
