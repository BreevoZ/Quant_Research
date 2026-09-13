#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
L3 订单号因子 v2 —— 纯 ID 算术的两个特征族(事件时钟, 不依赖桶宽的量)。

背景: SZ 订单号(ApplSeqNum, norm 后 channel 内严格单调)是"事件时钟":
两个号的差 = 期间整个 channel 跳了多少件事。已验证(000001, 100%):
委托事件 seqNo%1e12 == sysid → 撤单事件号与被撤单号同一序列空间, ID 差合法。

① 被吃单年龄(成交): age = |buyId − sellId|。SZ 大号=后到=主动方, 小号=被吃的挂单。
   吃老单(耐心资金的存货被消耗)vs 吃新单(高频互打)——方向失衡 = 知情压力代理。
② 撤单寿命(撤单): life = (撤单事件 seqNo % 1e12) − 被撤单 sysid。
   事件时间寿命自动归一行情快慢; 转瞬即撤(寿命处于该股当日底部分位)= spoofing/探价代理。

纪律: 绝不用 ID 水平值当特征(时间代理=泄漏), 只用差/比/失衡; 只 SZ 只连续段;
     收益不跨节次; 评估=截面 rank-IC + ret_next/ret_skip 对照(同 factor_flow)。
     per-stock-day 阈值(年龄中位/寿命q10)含全日信息 → 轻微前视, 探索期可接受, 已标注。

用法: python factors/factor_id.py [--limit N] [--bucket 60] [--dump panel.csv]
"""
import sys, os, glob, argparse
import numpy as np, pandas as pd
from qr import l3cache
from qr import paths

CACHE_DIR = str(paths.data("cache", "20260707"))   # 默认: 20260707 试验缓存
MOD = 10**12                                  # tl_parser.h norm(): id % 1e12 剥 channel 前缀
AM = (9*3600+30*60, 11*3600+30*60)
PM = (13*3600,       14*3600+57*60)

FEATS = ["trd_imb",                           # 基线: 经典主动量失衡(无年龄) —— 用来看年龄有没有增量
         "eat_age_imb",                       # ① 买方所吃卖单 vs 卖方所吃买单 的(量加权 log 年龄)失衡
         "eat_age_med",                       # ① 无向: 本桶被吃单年龄中位(是否在深挖老簿)
         "old_eat_imb",                       # ① 只看"老单"(年龄≥该股当日中位)被吃量的方向失衡
         "cxl_life_med",                      # ② 本桶被撤单的事件时间寿命中位
         "flash_cxl_ratio",                   # ② 转瞬撤单(寿命≤该股当日q10)量占撤单量比
         "flash_cxl_imb",                     # ② 转瞬撤单的买卖方向失衡(+ = 买侧在闪撤)
         "mean_size",                         # v1: 平均单笔委托量
         # —— v1 流量特征并入(2026-07-16), 冗余 ord_rate(=n_ord/60)已删 ——
         "n_ord",                             # v1: 桶内到达委托笔数(密集度; 无方向信号, 留作波动代理)
         "ord_imb_cnt",                       # v1: 买卖委托笔数失衡
         "ord_imb_vol",                       # v1: 买卖委托量失衡
         "cxl_ratio",                         # v1: 撤单数/挂单数
         "large_share"]                       # v1: 大单(该股全日p90)委托量占比(⚠全日分位, 轻微前视)
COLS = ["sym", "sess", "bkt"] + FEATS + ["last_px"]


def features_one(sym, a, bucket):
    sec = a["timeSeconds"]
    m = ((sec >= AM[0]) & (sec < AM[1])) | ((sec >= PM[0]) & (sec < PM[1]))
    a = a[m]; sec = sec[m]
    if len(a) == 0:
        return None
    at = a["actionType"]
    isO = at == 0
    isT = (at == 2) & (a["buyId"] > 0) & (a["sellId"] > 0)
    life = (a["seqNo"] % MOD) - a["sysid"]                    # 撤单寿命(事件tick)
    isC = (at == 1) & (a["sysid"] > 0) & (life > 0)
    logage = np.log1p(np.abs(a["buyId"] - a["sellId"]).astype(np.float64))

    # —— 该股当日阈值(⚠ 全日分位=轻微前视, 探索期可接受)——
    med_age = np.median(logage[isT]) if isT.any() else np.nan
    q10_life = np.quantile(life[isC], 0.10) if isC.any() else np.nan
    thr_big = np.quantile(a["volume"][isO], 0.90) if isO.any() else np.nan   # v1 large_share 阈值

    # —— 向量化聚合(2026-07-16): 逐桶 Python 循环 → 一次 groupby, 语义与旧实现逐值一致 ——
    vol = a["volume"].astype(np.float64)
    isB = a["direction"] == b"B"
    isBAgg = a["buyId"] > a["sellId"]
    old = (logage >= med_age) if not np.isnan(med_age) else np.zeros(len(a), bool)
    flash = (life <= q10_life) if not np.isnan(q10_life) else np.zeros(len(a), bool)
    bigo = (a["volume"] >= thr_big) if not np.isnan(thr_big) else np.zeros(len(a), bool)
    loglife = np.log1p(np.maximum(life, 0).astype(np.float64))

    def msk(cond, val):                                        # 掩码求和列: 条件外记 0
        return np.where(cond, val, 0.0)

    df = pd.DataFrame({
        "sess": np.where(sec < PM[0], 0, 1), "bkt": (sec // bucket).astype(np.int64),
        # 成交侧
        "nT": isT.astype(np.int64),
        "tvB": msk(isT & isBAgg, vol),   "tvS": msk(isT & ~isBAgg, vol),
        "twB": msk(isT & isBAgg, vol*logage), "twS": msk(isT & ~isBAgg, vol*logage),
        "toB": msk(isT & old & isBAgg, vol), "toS": msk(isT & old & ~isBAgg, vol),
        "age_t": np.where(isT, logage, np.nan),                # 中位数: 掩码 NaN, groupby 自动跳过
        "px_t":  np.where(isT, a["price"], np.nan),            # last_px: 组内最后一个非 NaN
        # 撤单侧
        "nC": isC.astype(np.int64),
        "cv": msk(isC, vol), "cvf": msk(isC & flash, vol),
        "cvfB": msk(isC & flash & isB, vol), "cvfS": msk(isC & flash & ~isB, vol),
        "life_c": np.where(isC, loglife, np.nan),
        # 挂单侧
        "nO": isO.astype(np.int64), "nOB": (isO & isB).astype(np.int64),
        "ovB": msk(isO & isB, vol), "ovS": msk(isO & ~isB, vol),
        "ovBig": msk(isO & bigo, vol), "ov": msk(isO, vol),
    })
    g = df.groupby(["sess", "bkt"], sort=True)
    s = g.sum(min_count=1)
    s["age_med"] = g["age_t"].median()
    s["life_med"] = g["life_c"].median()
    s["last_px"] = g["px_t"].last()                            # 跳过 NaN → 组内最后一笔成交价
    s = s[(s.nO > 0) | (s.nT > 0)]                             # 与旧实现同口径: 全空桶不出行

    nan = np.nan
    tv = s.tvB + s.tvS
    hasT = (s.nT > 0) & (tv > 0)
    trd_imb = np.where(hasT, (s.tvB - s.tvS) / tv.replace(0, nan), nan)
    both = hasT & (s.tvB > 0) & (s.tvS > 0)
    eb = s.twB / s.tvB.replace(0, nan); es = s.twS / s.tvS.replace(0, nan)
    eat_age_imb = np.where(both, (eb - es) / (eb + es), nan)
    eat_age_med = np.where(hasT, s.age_med, nan)
    to = s.toB + s.toS
    old_eat_imb = np.where(hasT & (to > 0), (s.toB - s.toS) / to.replace(0, nan), nan)
    hasC = (s.nC > 0) & (s.cv > 0)
    cxl_life_med = np.where(hasC, s.life_med, nan)
    flash_ratio = np.where(hasC, s.cvf / s.cv.replace(0, nan), nan)
    cvf = s.cvfB + s.cvfS
    flash_imb = np.where(hasC & (cvf > 0), (s.cvfB - s.cvfS) / cvf.replace(0, nan), nan)
    hasO = s.nO > 0
    ov = s.ovB + s.ovS
    mean_size = np.where(hasO, s.ov / s.nO.replace(0, nan), nan)
    n_ord = np.where(hasO, s.nO, nan)
    nb = s.nOB; ns = s.nO - s.nOB
    ord_imb_cnt = np.where(hasO, (nb - ns) / s.nO.replace(0, nan), nan)
    ord_imb_vol = np.where(hasO & (ov > 0), (s.ovB - s.ovS) / ov.replace(0, nan), nan)
    cxl_ratio = np.where(hasO, s.nC / s.nO.replace(0, nan), nan)
    large_share = np.where(hasO & (ov > 0), s.ovBig / ov.replace(0, nan), nan)

    res = pd.DataFrame({"trd_imb": trd_imb, "eat_age_imb": eat_age_imb,
                        "eat_age_med": eat_age_med, "old_eat_imb": old_eat_imb,
                        "cxl_life_med": cxl_life_med, "flash_cxl_ratio": flash_ratio,
                        "flash_cxl_imb": flash_imb, "mean_size": mean_size,
                        "n_ord": n_ord, "ord_imb_cnt": ord_imb_cnt, "ord_imb_vol": ord_imb_vol,
                        "cxl_ratio": cxl_ratio, "large_share": large_share,
                        "last_px": s.last_px}, index=s.index).reset_index()
    res.insert(0, "sym", sym)
    return [tuple(r) for r in res[COLS].itertuples(index=False, name=None)]


def process(args):
    fp, bucket = args
    sym = os.path.basename(fp)[-10:-4]          # <prefix>_<sym6>.bin, 前缀 tl_sz_/flow_sz_ 都适配
    try:
        return features_one(sym, l3cache.load(fp), bucket) or []
    except Exception as e:
        sys.stderr.write(f"[skip {fp}: {e}]\n"); return []


def xs_ic(feat, col, lab):
    ics = []
    for _, g in feat.groupby(["sess", "bkt"]):
        sub = g[[col, lab]].dropna()
        if len(sub) < 50:
            continue
        ics.append(sub[col].rank().corr(sub[lab].rank()))
    ics = np.array(ics)
    return (ics.mean(), ics.std(), len(ics)) if len(ics) else (float("nan"), float("nan"), 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--bucket", type=int, default=60)
    ap.add_argument("--dump", default="")
    ap.add_argument("--workers", type=int, default=32)
    ap.add_argument("--cache-dir", default=CACHE_DIR, help="CachedRec 缓存目录(含 tl_sz_*/flow_sz_*.bin)")
    ap.add_argument("--no-ic", action="store_true", help="只抽特征落盘, 不算单日 IC(批量模式)")
    a = ap.parse_args()

    files = sorted(glob.glob(f"{a.cache_dir}/flow_sz_*.bin")) or \
            sorted(glob.glob(f"{a.cache_dir}/tl_sz_*.bin"))      # 源前缀自适应(TL/Flow 结果逐字节一致)
    # TL 源未经 tltoflow 过滤, 混有可转债(12,T+0!)/ETF(15)/LOF(16)/B股(20) → 只留 SZ A股(00/30)
    files = [f for f in files if os.path.basename(f)[-10:-8] in ("00", "30")]
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

    feat = feat.sort_values(["sym", "sess", "bkt"]).reset_index(drop=True)
    feat["last_px"] = feat.groupby(["sym", "sess"])["last_px"].ffill()
    g = feat.groupby(["sym", "sess"])["last_px"]
    p0 = feat["last_px"]; p1 = g.shift(-1); p2 = g.shift(-2)
    feat["ret_next"] = (p1 - p0) / p0
    feat["ret_skip"] = (p2 - p1) / p1

    if a.dump:
        feat.to_csv(a.dump + ".tmp", index=False)
        os.replace(a.dump + ".tmp", a.dump)          # 原子替换: 中断不留半截面板
        print(f"[落盘] -> {a.dump}", file=sys.stderr)

    if a.no_ic:
        return
    print("\n============ 订单号因子: 截面 rank-IC(全 SZ, 单日)============")
    print(f"{'feature':<15} {'lab':<9} {'meanIC':>8} {'IC_std':>8} {'IR':>7} {'t':>7} {'nbkt':>5}")
    for f in FEATS:
        for lab in ["ret_next", "ret_skip"]:
            m, s, n = xs_ic(feat, f, lab)
            ir = m / s if s and s > 0 else 0
            print(f"{f:<15} {lab:<9} {m:>8.4f} {s:>8.4f} {ir:>7.3f} {ir*np.sqrt(n):>7.2f} {n:>5}")
    print("\n⚠ 单日: t 桶间自相关偏乐观; trd_imb 是无年龄基线, 年龄特征须打赢它才算 ID 有增量。")


if __name__ == "__main__":
    main()
