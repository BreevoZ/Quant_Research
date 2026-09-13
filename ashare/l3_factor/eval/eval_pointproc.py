#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
点过程因子评估: 截面 rank-IC(next/skip) + 与 n_ord 的偏相关。
核心问题: 事件时间因子(BB/Hawkes) 是否比固定降频 n_ord 带来【增量】,
          还是只是密集度换皮? → 控制 n_ord 后的偏 IC 是判决。
"""
import sys
import numpy as np, pandas as pd
from qr.paths import DATA

PP = sys.argv[1] if len(sys.argv) > 1 else DATA + "/pp_20260713.csv"
DATE = PP.split("_")[-1].split(".")[0]
PANEL = f"{DATA}/panels/panel_id_{DATE}.csv"
FEATS = ["hk_branch", "hk_halflife", "hk_intens", "burst_ratio", "burst_switch", "hk_imb"]

pp = pd.read_csv(PP, dtype={"sym": str})
mk = pd.read_csv(PANEL, dtype={"sym": str}, usecols=["sym", "sess", "bkt", "n_ord", "ord_imb_vol", "last_px"]).dropna(subset=["last_px"])
mk = mk.sort_values(["sym", "sess", "bkt"])
g = mk.groupby(["sym", "sess"])["last_px"]
p1 = g.shift(-1)
mk["ret_next"] = (p1 - mk.last_px) / mk.last_px
mk["ret_skip"] = (g.shift(-2) - p1) / p1
df = mk.merge(pp, on=["sym", "sess", "bkt"], how="inner")
print(f"合并面板: {len(df)} 行 / {df.sym.nunique()} 股 (点过程 ∩ 主面板)")

def xs_ic(col, lab, ctrl=None):
    ics = []
    for _, gg in df.groupby(["sess", "bkt"]):
        sub = gg[[col, lab] + ([ctrl] if ctrl else [])].dropna()
        if len(sub) < 30:
            continue
        rx, ry = sub[col].rank(), sub[lab].rank()
        if ctrl is None:
            ics.append(rx.corr(ry))
        else:
            rc = sub[ctrl].rank()
            rxy, rxz, ryz = rx.corr(ry), rx.corr(rc), rc.corr(ry)
            den = np.sqrt((1-rxz**2)*(1-ryz**2))
            if den > 1e-9:
                ics.append((rxy - rxz*ryz)/den)
    v = np.array(ics)
    return (v.mean(), v.mean()/v.std()*np.sqrt(len(v)) if v.std() > 0 else 0, len(v)) if len(v) else (np.nan,)*3

print("\n=== 截面 rank-IC(单日, 桶间 t 仅参考)===")
print(f"{'因子':<14}{'next IC(t)':>16}{'skip IC(t)':>16}{'next|控n_ord IC(t)':>22}")
for f in ["n_ord", "ord_imb_vol"] + FEATS:
    n = xs_ic(f, "ret_next"); s = xs_ic(f, "ret_skip")
    pc = xs_ic(f, "ret_next", ctrl="n_ord") if f not in ("n_ord",) else (np.nan,)*3
    pcs = f"{pc[0]:+.4f}({pc[1]:+.1f})" if not np.isnan(pc[0]) else "—"
    print(f"{f:<14}{f'{n[0]:+.4f}({n[1]:+.1f})':>16}{f'{s[0]:+.4f}({s[1]:+.1f})':>16}{pcs:>22}")
print("\n判读: 事件时间因子若控制 n_ord 后 IC 塌 → 只是密集度换皮; 若保留 → 真增量。")
