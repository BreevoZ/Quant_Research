#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
类星体因子 · 安慰剂检验(用户点名: 就是 AGN, 不是别的)。

数据: ZTF/ALeRCE 25 个高置信 AGN 的真实光变曲线(magpsf, 逐夜)。
特征: ①agn_glow = 当夜全体 AGN 的平均增亮速率(-Δmag/Δday, 正=变亮)
      ②agn_disp = 当夜各 AGN 增亮速率的截面离散度
      ③每个 AGN 单独的增亮速率(×25, 多重检验演示组)
标签: 中证500 次一交易日 close→close 收益(信号夜 D → D+1 交易日, 无重叠)。
预注册预期: 真相关=0; 27个检验@5%显著水平, 预期 1-2 个假阳性 |t|>2; 聚合特征 |t|>3 ⇒ 管道有bug。
"""
import glob, json
import numpy as np, pandas as pd
from qr.paths import DATA

MJD_20260101 = 61041  # 2026-01-01


def mjd_to_date(m):
    return (pd.Timestamp("2026-01-01") + pd.Timedelta(days=float(m) - MJD_20260101)).strftime("%Y%m%d")


# ── AGN 光变 → 逐夜增亮速率 ──
rows = []
for fp in sorted(glob.glob(DATA + "/agn/lc_*.json")):
    oid = fp.split("lc_")[1][:-5]
    try:
        det = json.load(open(fp))
    except Exception:
        continue
    d = pd.DataFrame([(x["mjd"], x.get("magpsf_corr") or x.get("magpsf"), x.get("fid")) for x in det],
                     columns=["mjd", "mag", "fid"]).dropna()
    d = d[d.fid == 1] if (d.fid == 1).sum() > (d.fid == 2).sum() else d[d.fid == 2]   # 取观测多的波段
    d = d.sort_values("mjd").groupby(d.mjd.round(0)).mag.mean().reset_index()          # 同夜合并
    d["rate"] = -(d.mag.diff()) / d.mjd.diff()          # 增亮速率: mag 变小=变亮 → 取负
    d = d[(d.mjd.diff() <= 10)]                          # 间隔>10天的跳变丢弃
    d["date"] = d.mjd.apply(mjd_to_date)
    d["oid"] = oid
    rows.append(d[["date", "oid", "rate"]])
agn = pd.concat(rows, ignore_index=True).dropna()
agn = agn[agn.date >= "20251220"]

# ── 指数日收益(次日, 防泄漏) ──
idx = pd.read_csv(DATA + "/index_min_2026h1.csv")
idx["date"] = idx.d.str.replace("-", "")
close = idx[idx.symbol == "sh000905"].groupby("date").apply(lambda g: g.sort_values("hm").px.iloc[-1], include_groups=False)
close = close[close > 0]                     # 剔除假日零值行(如 20260223 全天 px=0)
tdays = sorted(close.index)
nxtret = {}
for i in range(len(tdays) - 1):
    nxtret[tdays[i]] = (close[tdays[i+1]] - close[tdays[i]]) / close[tdays[i]] * 1e4
# AGN 观测夜(自然日) D → 其后第一个有 nxtret 的交易日
def to_tday(d):
    for t in tdays:
        if t >= d and t in nxtret:
            return t
    return None
agn["tday"] = agn.date.apply(to_tday)
agn = agn.dropna(subset=["tday"])

def tstat(x, y):
    s = pd.concat([x, y], axis=1).dropna()
    if len(s) < 15:
        return np.nan, np.nan, len(s)
    r = s.iloc[:, 0].corr(s.iloc[:, 1])
    return r, r*np.sqrt((len(s)-2)/(1-r**2)), len(s)

lab = pd.Series(nxtret)
print("=" * 62)
print("  类星体 → 中证500 次日收益 · 安慰剂检验(2026H1)")
print("=" * 62)
g = agn.groupby("tday").rate
for name, ser in (("agn_glow(集体增亮)", g.mean()), ("agn_disp(增亮离散)", g.std())):
    r, t, n = tstat(ser, lab)
    print(f"  聚合特征 {name:<18} corr={r:+.3f}  t={t:+.2f}  n={n}")
print("-" * 62)
hits = 0; res = []
for oid, og in agn.groupby("oid"):
    ser = og.groupby("tday").rate.mean()
    r, t, n = tstat(ser, lab)
    if not np.isnan(t):
        res.append((oid, r, t, n)); hits += abs(t) > 2
res.sort(key=lambda x: -abs(x[2]))
print(f"  单体 AGN {len(res)} 个 × 次日收益: |t|>2 的有 {hits} 个(预注册预期: 5%水平≈{len(res)*0.05:.1f}个)")
for oid, r, t, n in res[:4]:
    star = "  ← 恭喜这颗类星体成为'显著'因子 🎉(然后呢?)" if abs(t) > 2 else ""
    print(f"    {oid}: corr={r:+.3f} t={t:+.2f} n={n}{star}")
print("=" * 62)
print("  结论模板: 聚合|t|<2 ⇒ 管道无泄漏; 单体假阳性≈预期 ⇒ 多重检验活体演示完成。")
