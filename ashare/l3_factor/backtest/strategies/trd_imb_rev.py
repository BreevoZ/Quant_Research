# 示例策略②: 经典成交流失衡反转(基线因子 trd_imb, 证明框架与因子解耦)。
# 主动买占比最高(买压耗竭)→ 卖; 主动卖占比最高 → 买。
NAME = "trd_imb_rev"
PARAMS = dict(k=3, hold=4, notional=50000, pxmin=3.0, pxmax=300.0)
COLS = ["trd_imb"]


def on_bucket(xs, p):
    g = xs[xs.last_px.between(p["pxmin"], p["pxmax"])].sort_values("trd_imb")
    out = []
    for side, rows in (("B", g.head(p["k"])), ("S", g.tail(p["k"]))):
        for _, r in rows.iterrows():
            out.append(dict(sym=r.sym, side=side, notional=p["notional"],
                            entry=("last",), hold=p["hold"], exit=("cross", 0.02)))
    return out
