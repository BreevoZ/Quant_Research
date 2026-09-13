# 被吃单年龄失衡 → 微观耗竭反转(底仓 T+0)。
# 卖方啃穿老买单(因子最负)= 卖方力竭 → 买; 买方啃穿老卖单(最正)= 买方力竭 → 卖。
NAME = "eat_age_rev"
PARAMS = dict(k=3, hold=4, notional=50000, pxmin=3.0, pxmax=300.0,
              entry="last", cross_pct=0.02,
              exit="cross", exit_wait=4, exit_off=0.0)   # exit=twostage: 被动平仓等 exit_wait 分钟再穿价扫尾
COLS = ["eat_age_imb"]


def mk_exit(p):
    return ("twostage", p["exit_off"], p["exit_wait"]) if p["exit"] == "twostage" \
        else ("cross", p["cross_pct"])


def on_bucket(xs, p):
    g = xs[xs.last_px.between(p["pxmin"], p["pxmax"])].sort_values("eat_age_imb")
    entry = ("cross", p["cross_pct"]) if p["entry"] == "cross" else \
            ("improve", 1) if p["entry"] == "improve" else ("last",)
    out = []
    for side, rows in (("B", g.head(p["k"])), ("S", g.tail(p["k"]))):
        for _, r in rows.iterrows():
            out.append(dict(sym=r.sym, side=side, notional=p["notional"],
                            entry=entry, hold=p["hold"], exit=mk_exit(p)))
    return out
