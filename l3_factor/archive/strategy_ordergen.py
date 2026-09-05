#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
⚠ 已归档(2026-07-16): v0 单策略版, 已被 vgen.py(策略插件框架)+ strategies/eat_age_rev.py 取代。
   保留仅作历史参照, 不再维护。

策略 → vsim 虚拟订单生成器(底仓 T+0, 围绕 eat_age_imb 反转信号)。

策略(v0, 框架优先):
  - 每 --every 分钟触发一次: 用【刚结束的那个 1 分钟桶】的 eat_age_imb 做截面排名。
  - 多腿: 最负的 K 只(卖方啃穿老买单=卖方耗竭 → 预期涨)→ 买入, 持有 --hold 分钟后平仓;
  - 空腿: 最正的 K 只(买方啃穿老卖单=买方耗竭 → 预期跌)→ 卖出底仓, --hold 分钟后买回。
  - 底仓 T+0 假设: 每只候选股都有足额底仓, 日内可先卖后买; 日终回到底仓水平。
  - 进场: 被动限价 = 信号桶末价(vsim 真实排队, 排不到就是排不到 —— 填单率诚实);
    平仓: 穿价限价(±2%, 按对手价成交, 确保出得去), 同时撤掉进场腿残量。

防前视: 只用信号桶【结束之后】的时刻下单(桶 t 特征 → t 桶末+1s 下单);
        只读特征列和 last_px, 绝不碰 ret_next/ret_skip 标签列。

输出:
  orders.csv  vsim 格式(id,action,market,symbol,side,price,qty,t)
  meta.csv    配对信息(pair_id, 进/出腿id, 触发时刻, 方向, 计划量, 参考价)→ 给 PnL 结算用

用法: python strategy_ordergen.py --panel data/panels/panel_id_20260713.csv \
        --out bt/orders_20260713.csv --meta bt/meta_20260713.csv
"""
import argparse, os
import numpy as np, pandas as pd

TICK = 0.01     # SZ 股票最小价位

ap = argparse.ArgumentParser()
ap.add_argument("--panel", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--meta", required=True)
ap.add_argument("--every", type=int, default=5, help="触发间隔(分钟)")
ap.add_argument("--k", type=int, default=3, help="每次每方向选股数")
ap.add_argument("--hold", type=int, default=4, help="持有(分钟)")
ap.add_argument("--notional", type=float, default=50000, help="每腿名义(元)")
ap.add_argument("--pxmin", type=float, default=3.0)
ap.add_argument("--pxmax", type=float, default=300.0)
a = ap.parse_args()

f = pd.read_csv(a.panel, dtype={"sym": str})
f = f[["sym", "sess", "bkt", "eat_age_imb", "last_px"]].dropna()   # 只碰特征+参考价, 不碰标签

# 触发时刻表(信号桶 bkt 结束 = (bkt+1)*60 秒): AM 09:40–11:25, PM 13:05–14:48
AM0, AM1 = 9*3600+40*60, 11*3600+25*60
PM0, PM1 = 13*3600+5*60, 14*3600+48*60
trigs = []
for s0, s1 in ((AM0, AM1), (PM0, PM1)):
    t = s0
    while t <= s1 - a.hold*60:                       # 平仓不越过节次末
        trigs.append(t)
        t += a.every*60

orders, meta = [], []
pid = 0
for tsec in trigs:
    sig_bkt = tsec//60 - 1                           # 刚结束的桶
    g = f[(f.bkt == sig_bkt) & f.last_px.between(a.pxmin, a.pxmax)]
    if len(g) < 200:                                  # 截面太小不触发
        continue
    g = g.sort_values("eat_age_imb")
    picks = [("B", g.head(a.k))] + [("S", g.tail(a.k))]
    for side, rows in picks:
        for _, r in rows.iterrows():
            px = round(r.last_px, 2)
            qty = int(a.notional/px//100)*100
            if qty < 100:
                continue
            pid += 1
            t_in  = (tsec + 1)*1000                   # 桶末+1s 进场
            t_out = (tsec + a.hold*60)*1000           # hold 分钟后平仓
            eid, xid = f"E{pid}", f"X{pid}"
            side_in  = 1 if side == "B" else 2
            side_out = 2 if side == "B" else 1
            # 平仓穿价限价: 多腿卖出压价 2%, 空腿买回抬价 2%(确保 crossing, 成交价=对手价)
            px_out = round(px*0.98, 2) if side == "B" else round(px*1.02, 2)
            orders.append(f"{eid},1,102,{r.sym},{side_in},{px:.2f},{qty},{t_in}")
            orders.append(f"{eid},2,,,,,,{t_out}")                       # 撤进场腿残量
            orders.append(f"{xid},1,102,{r.sym},{side_out},{px_out:.2f},{qty},{t_out}")
            meta.append((pid, eid, xid, r.sym, side, tsec, px, qty))

os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
with open(a.out, "w") as fo:
    fo.write("id,action,market,symbol,side,price,qty,t\n")
    fo.write("\n".join(orders) + "\n")
pd.DataFrame(meta, columns=["pair", "eid", "xid", "sym", "dir", "trig_sec", "ref_px", "qty"]) \
  .to_csv(a.meta, index=False)
print(f"[订单] {len(meta)} 对腿({len(orders)} 行动作), {len(set(m[3] for m in meta))} 只股票, "
      f"{len(trigs)} 个触发点 -> {a.out}")
