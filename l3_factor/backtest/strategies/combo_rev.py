# 快信号三合一组合(底仓 T+0)。
# score = z(kept_rem_imb) + z(chain_imb) − z(eat_age_imb)   —— 三者方向统一成"越大越看涨":
#   kept_rem_imb 高 = 买方残量堆积(未满足需求) → 看涨(一周 next IC +0.088)
#   chain_imb    高 = 买方拆单链在推(母单执行) → 看涨(+0.033)
#   eat_age_imb  高 = 买方啃老卖单(耗竭)      → 看跌(−0.05, 取负)
# 需要面板同时含 13f 列与 uniq 列(vgen --panel a.csv,b.csv 合并)。
NAME = "combo_rev"
PARAMS = dict(k=3, hold=8, notional=50000, pxmin=15.0, pxmax=300.0,
              entry="improve", cross_pct=0.02,
              exit="twostage", exit_wait=4, exit_off=0.0)
COLS = ["eat_age_imb", "kept_rem_imb", "chain_imb"]


def mk_exit(p):
    return ("twostage", p["exit_off"], p["exit_wait"]) if p["exit"] == "twostage" \
        else ("cross", p["cross_pct"])


def z(s):
    sd = s.std()
    return (s - s.mean()) / (sd if sd else 1.0)


def on_bucket(xs, p):
    g = xs[xs.last_px.between(p["pxmin"], p["pxmax"])].copy()
    if len(g) < 100:
        return []
    g["score"] = z(g.kept_rem_imb.fillna(0)) + z(g.chain_imb.fillna(0)) - z(g.eat_age_imb.fillna(0))
    g = g.sort_values("score")
    entry = ("cross", p["cross_pct"]) if p["entry"] == "cross" else \
            ("improve", 1) if p["entry"] == "improve" else ("last",)
    out = []
    for side, rows in (("S", g.head(p["k"])), ("B", g.tail(p["k"]))):   # 高分看涨→买
        for _, r in rows.iterrows():
            out.append(dict(sym=r.sym, side=side, notional=p["notional"],
                            entry=entry, hold=p["hold"], exit=mk_exit(p)))
    return out
