#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vloop —— 用「不动点迭代」在预生成订单上实现依赖成交的逻辑(开环跑出闭环)。

原理(用户点破的): 磁带确定性重放 + 虚拟单不扰动磁带 ⇒ "成交到 X 股的时刻"可以
事后精确求出。跑一遍 vsim 拿 fills 时间线 → 把条件动作(按实际成交调平仓量/达量撤单)
写回订单 → 再跑。因果性保证 t* 之前轨迹不变 ⇒ 结果精确等价于实时条件单。
同股兄弟腿共享预算有弱耦合 ⇒ 迭代到订单文件不动(实测 1-2 轮收敛), 不收敛则报错。

当前实现的条件规则:
  R1 平仓量 = 进场腿在平仓时刻的实际成交量(取代计划量; 底仓吸收→0)
     进场腿 0 成交 ⇒ 删平仓腿(只留撤单)。
(扩展位: 达量撤单/止盈止损同机制, 在 rewrite() 里加规则即可。)

用法: python backtest/vloop.py --orders bt/orders_chk_20260713.csv --meta bt/meta_chk_20260713.csv \
        --tl-dir data/tmp/20260713 --date 20260713 --cache-dir data/cache --outdir bt --tag loop
"""
import argparse, os, subprocess, sys
import pandas as pd
from qr import paths

_VSIM_CACHE = None
def _vsim():
    """vsim 可执行文件路径。延迟解析:没构建时才报错,import 本模块不受影响。"""
    global _VSIM_CACHE
    if _VSIM_CACHE is None:
        _VSIM_CACHE = str(paths.vsim())
    return _VSIM_CACHE
MAX_ITER = 5

ap = argparse.ArgumentParser()
ap.add_argument("--orders", required=True)
ap.add_argument("--meta", required=True)
ap.add_argument("--tl-dir", required=True)
ap.add_argument("--date", required=True)
ap.add_argument("--cache-dir", required=True)
ap.add_argument("--outdir", default="bt")
ap.add_argument("--tag", default="loop")
ap.add_argument("--trust-cache", action="store_true",
                help="透传给 vsim: 源CSV已删只剩缓存时跳过指纹核对(半年批量回测)")
a = ap.parse_args()

meta = pd.read_csv(a.meta, dtype={"sym": str})
base = open(a.orders).read().strip().split("\n")
hdr, rows = base[0], base[1:]

def run_vsim(orders_path, res_path, fills_path):
    cmd = [_vsim(), "--tl-dir", a.tl_dir, "--date", a.date,
           "--orders", orders_path, "--cache-dir", a.cache_dir,
           "--out", res_path, "--fills", fills_path]
    if a.trust_cache:
        cmd.append("--trust-cache")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr[-2000:]); sys.exit(1)

def rewrite(res):
    """R1: 平仓腿数量 ← 进场腿实际成交量。
    R2(两段式出场): P腿(被动平仓) qty ← E成交量; X腿(穿价扫尾) qty ← E成交 − P成交。"""
    filled = res.set_index("id").filled
    out = []
    for ln in rows:
        p = ln.split(",")
        oid, act = p[0], p[1]
        if oid.startswith("P") and act == "1":
            fq = int(filled.get("E" + oid[1:], 0))
            if fq <= 0:
                continue
            p[6] = str(fq)
            out.append(",".join(p))
        elif oid.startswith("X") and act == "1":
            eid, pidp = "E" + oid[1:], "P" + oid[1:]
            fq = int(filled.get(eid, 0))
            if pidp in filled.index:                       # 两段式: 扫尾 = 残量
                fq -= int(filled.get(pidp, 0))
            if fq <= 0:
                continue                      # 进场零成交/被动腿已全平 → 不发
            p[6] = str(fq)
            out.append(",".join(p))
        else:
            out.append(ln)
    return out

cur = rows
for it in range(1, MAX_ITER+1):
    op = f"{a.outdir}/orders_{a.tag}_it{it}.csv"
    rp, fp = f"{a.outdir}/res_{a.tag}.csv", f"{a.outdir}/fills_{a.tag}.csv"
    with open(op, "w") as h:
        h.write(hdr + "\n" + "\n".join(cur) + "\n")
    run_vsim(op, rp, fp)
    res = pd.read_csv(rp, dtype={"symbol": str})
    new = rewrite(res)
    print(f"[vloop] 第{it}轮: {len(cur)}行订单 → 重写后 {len(new)}行", file=sys.stderr)
    if new == cur:
        print(f"[vloop] ✅ 不动点收敛(第{it}轮), 最终: {rp}")
        break
    cur = new
else:
    sys.exit(f"❌ {MAX_ITER} 轮未收敛 — 条件耦合超预期, 需人工检查")
