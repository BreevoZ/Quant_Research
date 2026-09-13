#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
回测 PnL 结算: vsim 逐腿结果 + 订单配对 meta → 底仓 T+0 口径的净收益。

口径:
  - 每对腿: 成交配对量 q = min(进场腿成交, 平仓腿成交); 毛利 = 方向化价差 × q。
  - 进场腿没成交的部分: 平仓腿多卖/多买的量记入"底仓吸收"(unmatched), 单独报告, 不计盈亏
    (底仓 T+0 假设下是底仓数量的暂时变化, 诚实做法是不给它记收益)。
  - 费用: 佣金 2bp × 双边成交额 + 印花税 5bp × 卖出成交额(现行: 仅卖方)。
  - vsim 的成交价已含真实排队/穿价 → 滑点天然在毛利里, 不另计。

用法: python backtest/pnl.py --res bt/res.csv --meta bt/meta_20260713.csv
"""
import argparse
import pandas as pd, numpy as np

COMM = 0.0002      # 佣金 2bp(双边)
STAMP = 0.0005     # 印花税 5bp(仅卖出)

ap = argparse.ArgumentParser()
ap.add_argument("--res", required=True, help="vsim --out 的逐腿结果 CSV")
ap.add_argument("--meta", required=True)
a = ap.parse_args()

res = pd.read_csv(a.res, dtype={"symbol": str}).set_index("id")
meta = pd.read_csv(a.meta, dtype={"sym": str})

rows = []
has_p = "pid_p" in meta.columns
for _, m in meta.iterrows():
    if m.eid not in res.index:
        continue
    e = res.loc[m.eid]
    # 平仓侧: 普通=单腿 X; 两段式=P(被动)+X(穿价扫尾) 加权合并
    fx, amt = 0, 0.0
    for oid in ([m.xid] + ([m.pid_p] if has_p and isinstance(m.pid_p, str) and m.pid_p else [])):
        if oid in res.index and res.loc[oid].filled > 0:
            fx += int(res.loc[oid].filled)
            amt += res.loc[oid].avg_price * res.loc[oid].filled
    if fx == 0 and m.xid not in res.index:
        continue
    px_x = amt / fx if fx else 0.0
    x = pd.Series({"filled": fx, "avg_price": px_x})
    q = int(min(e.filled, x.filled))
    if m.dir == "B":   # 买入 e.avg → 卖出 x.avg
        gross = (x.avg_price - e.avg_price) * q
        sell_notional = x.avg_price * q
    else:              # 卖出 e.avg → 买回 x.avg
        gross = (e.avg_price - x.avg_price) * q
        sell_notional = e.avg_price * q
    fee = COMM * (e.avg_price + x.avg_price) * q + STAMP * sell_notional if q else 0.0
    rows.append(dict(pair=m.pair, sym=m.sym, dir=m.dir, trig=m.trig_sec, plan=m.qty,
                     fill_e=int(e.filled), fill_x=int(x.filled), q=q,
                     px_e=e.avg_price, px_x=x.avg_price,
                     gross=gross, fee=fee, net=gross-fee,
                     unmatched=int(abs(e.filled-x.filled)),
                     displaced=int(e.displaced)))
d = pd.DataFrame(rows)

print("================ 底仓 T+0 回测结算 ================")
print(f"腿对 {len(d)} | 股票 {d.sym.nunique()} | 进场填单率(量) "
      f"{d.fill_e.sum()/d.plan.sum()*100:.1f}% | 完整往返(q>0) {(d.q>0).sum()} 对")
traded = d[d.q > 0]
if len(traded):
    to = (traded.px_e*traded.q + traded.px_x*traded.q).sum()
    print(f"\n成交额(双边)   {to:,.0f} 元")
    print(f"毛利           {traded.gross.sum():+,.0f} 元  ({traded.gross.sum()/to*1e4:+.2f} bp/双边)")
    print(f"费用           {traded.fee.sum():,.0f} 元  (佣金2bp双边+印花5bp卖出)")
    print(f"净利           {traded.net.sum():+,.0f} 元  ({traded.net.sum()/to*1e4:+.2f} bp/双边)")
    print(f"胜率(净)       {(traded.net>0).mean()*100:.1f}%   均净 {traded.net.mean():+,.0f} 元/对")
    print(f"底仓吸收       {d.unmatched.sum():,} 股 (进/出腿成交不齐部分, 不计盈亏)")
    for dr, g in traded.groupby("dir"):
        to_d = (g.px_e*g.q + g.px_x*g.q).sum()
        print(f"  [{'多腿(买→卖)' if dr=='B' else '空腿(卖→买)'}] {len(g)}对  "
              f"毛 {g.gross.sum():+,.0f}  净 {g.net.sum():+,.0f} ({g.net.sum()/to_d*1e4:+.2f}bp)  "
              f"胜率 {(g.net>0).mean()*100:.0f}%")
print("\n⚠ 单日样本外 + v0 参数拍脑袋: 只验证框架与量级, 不是策略结论。")
d.to_csv(a.res.replace(".csv", "_pnl.csv"), index=False)
print(f"[明细] -> {a.res.replace('.csv','_pnl.csv')}")
