#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vgen —— 通用「策略 + 当日数据 → vsim 虚拟订单」框架。

分工:
  策略(strategies/*.py)只回答: 买谁/卖谁、多少名义、怎么进、持有多久、怎么出。
  框架负责其余一切: 触发调度、防前视(只喂已走完的桶)、节次边界、订单编排
  (进场/撤残量/平仓)、meta 配对表(pnl.py / plot_pnl.py 直接可用)。

策略接口(见 strategies/eat_age_rev.py 模板):
  PARAMS: dict            默认参数(--set k=5 覆盖)
  COLS:   list[str]       需要面板提供的特征列(框架校验, 缺列即报错)
  def on_bucket(xs, p) -> list[Intent]
      xs: 该触发点【刚走完的桶】的全市场截面 DataFrame(sym + COLS + last_px)
      Intent = dict(sym, side='B'|'S', notional=元,
                    entry=('last')|('improve',n档)|('cross',pct),
                    hold=分钟, exit=('cross',pct)|('passive'))

已知边界(诚实声明): 订单是【预生成】的 —— 策略看不到自己的成交(开环)。
仓位/出场不能依赖 fill 反馈; 那需要 vsim 侧的 co-simulation(README「下一步」), 本接口
按可平移设计: on_bucket 语义与未来 on_bar 回调一致, 到时策略文件原样搬。

用法:
  python backtest/vgen.py --strategy backtest/strategies/eat_age_rev.py \
      --panel data/panels/panel_id_20260713.csv --outdir bt --tag v1 \
      [--set k=5 --set hold=8]
"""
import argparse, importlib.util, os, sys
import pandas as pd

TICK = 0.01
AM0, AM1 = 9*3600+40*60, 11*3600+25*60      # 触发窗(避开盘/收边界)
PM0, PM1 = 13*3600+5*60, 14*3600+48*60
MIN_XS = 200                                 # 截面最少股票数


def load_strategy(path):
    spec = importlib.util.spec_from_file_location("stg", path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    for req in ("PARAMS", "COLS", "on_bucket"):
        assert hasattr(m, req), f"策略缺 {req}"
    return m


def entry_price(ref, side, entry):
    kind = entry[0]
    if kind == "last":    return ref
    if kind == "improve": n = entry[1]; return ref + (n*TICK if side == "B" else -n*TICK)
    if kind == "cross":   pct = entry[1]; return ref*(1+pct) if side == "B" else ref*(1-pct)
    raise ValueError(f"未知 entry: {entry}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strategy", required=True)
    ap.add_argument("--panel", required=True)
    ap.add_argument("--outdir", default="bt")
    ap.add_argument("--tag", default="")
    ap.add_argument("--every", type=int, default=5, help="触发间隔(分钟)")
    ap.add_argument("--set", action="append", default=[], help="覆盖策略参数 k=v")
    a = ap.parse_args()

    stg = load_strategy(a.strategy)
    p = dict(stg.PARAMS)
    for kv in a.set:
        k, v = kv.split("=", 1)
        assert k in p, f"策略无参数 {k}(有: {list(p)})"
        p[k] = type(p[k])(v)

    # --panel 支持逗号分隔多个面板文件(如 13f 面板 + uniq 面板), 按 (sym,sess,bkt) 合并
    parts = []
    for i, path in enumerate(a.panel.split(",")):
        t = pd.read_csv(path, dtype={"sym": str})
        t = t.drop(columns=[c for c in ("ret_next", "ret_skip") if c in t.columns])
        if i > 0:
            t = t.drop(columns=[c for c in ("last_px",) if c in t.columns])
        parts.append(t)
    f = parts[0]
    for t in parts[1:]:
        f = f.merge(t, on=["sym", "sess", "bkt"], how="inner")
    need = ["sym", "bkt"] + stg.COLS + ["last_px"]
    miss = [c for c in need if c not in f.columns]
    assert not miss, f"面板缺列: {miss}"
    f = f[need].dropna()                         # 防前视: 标签列(ret_*)根本不进内存

    date = os.path.basename(a.panel.split(",")[0]).split("_")[-1].split(".")[0]
    name = getattr(stg, "NAME", os.path.basename(a.strategy).rsplit(".", 1)[0])
    tag = a.tag or name

    max_hold = 0
    orders, meta, pid = [], [], 0
    for s0, s1 in ((AM0, AM1), (PM0, PM1)):
        t = s0
        while t <= s1:
            sig_bkt = t//60 - 1                  # 只喂【刚走完】的桶
            xs = f[f.bkt == sig_bkt]
            if len(xs) >= MIN_XS:
                for it in stg.on_bucket(xs, p):
                    hold = int(it.get("hold", 4)); max_hold = max(max_hold, hold)
                    if t + hold*60 > s1 + (a.every-1)*60:      # 平仓不越节次
                        continue
                    ref = float(xs.loc[xs.sym == it["sym"], "last_px"].iloc[0])
                    qty = int(it.get("notional", 50000)/ref//100)*100
                    if qty < 100:
                        continue
                    pid += 1
                    eid, xid = f"E{pid}", f"X{pid}"
                    s_in = 1 if it["side"] == "B" else 2
                    px_in = round(entry_price(ref, it["side"], it.get("entry", ("last",))), 2)
                    ex = it.get("exit", ("cross", 0.02))
                    o_side = "S" if it["side"] == "B" else "B"
                    t_in, t_out = (t+1)*1000, (t+hold*60)*1000
                    if ex[0] == "twostage":
                        # 两段式出场: t_out 先挂被动平仓(参考价±offset_bp), 等 wait 分钟,
                        # 撤残量并穿价扫尾。被动腿成交的部分省下整个半价差(甚至赚)。
                        off_bp, wait_min = ex[1], ex[2]
                        px_p = round(ref*(1 + off_bp/1e4) if o_side == "S" else ref*(1 - off_bp/1e4), 2)
                        px_c = round(entry_price(ref, o_side, ("cross", 0.02)), 2)
                        pidp = f"P{pid}"
                        t_out2 = t_out + wait_min*60*1000
                        if (t + hold*60 + wait_min*60) > s1 + (a.every-1)*60:
                            continue                     # 扫尾也不许越节次
                        orders += [f"{eid},1,102,{it['sym']},{s_in},{px_in:.2f},{qty},{t_in}",
                                   f"{eid},2,,,,,,{t_out}",
                                   f"{pidp},1,102,{it['sym']},{3-s_in},{px_p:.2f},{qty},{t_out}",
                                   f"{pidp},2,,,,,,{t_out2}",
                                   f"{xid},1,102,{it['sym']},{3-s_in},{px_c:.2f},{qty},{t_out2}"]
                        meta.append((pid, eid, xid, it["sym"], it["side"], t, ref, qty, pidp))
                    else:
                        px_out = round(entry_price(ref, o_side, ex if ex[0] != "passive" else ("last",)), 2)
                        orders += [f"{eid},1,102,{it['sym']},{s_in},{px_in:.2f},{qty},{t_in}",
                                   f"{eid},2,,,,,,{t_out}",
                                   f"{xid},1,102,{it['sym']},{3-s_in},{px_out:.2f},{qty},{t_out}"]
                        meta.append((pid, eid, xid, it["sym"], it["side"], t, ref, qty, ""))
            t += a.every*60

    os.makedirs(a.outdir, exist_ok=True)
    fo, fm = f"{a.outdir}/orders_{tag}_{date}.csv", f"{a.outdir}/meta_{tag}_{date}.csv"
    with open(fo, "w") as h:
        h.write("id,action,market,symbol,side,price,qty,t\n" + "\n".join(orders) + "\n")
    pd.DataFrame(meta, columns=["pair", "eid", "xid", "sym", "dir", "trig_sec", "ref_px", "qty", "pid_p"]).to_csv(fm, index=False)
    print(f"[vgen] 策略={name} 参数={p} | {len(meta)} 对腿 / {len(set(m[3] for m in meta))} 股 -> {fo}")


if __name__ == "__main__":
    main()
