#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TESS 目标获取 —— 专扫【连续观测区 CVZ】: 黄极附近的天区被多个 sector 反复观测,
同一批星有跨 sector 的长时间覆盖 → 能对上更多交易日, 保证时间延续性。

绕开 MAST 大结果集不稳(>730KB 截断): 按小天区(半径 0.4°)分块查, 每次约 10 秒完整返回。
输出 data/tess_targets.csv(含每颗星在各 sector 的观测时段)。
"""
import json, subprocess, sys, time
import numpy as np, pandas as pd
from qr.paths import DATA

SECTORS = (88, 103)       # 放宽到近两年, 便于挑"被多次观测"的星
RADIUS = 0.4
API = "https://mast.stsci.edu/api/v0/invoke"
# 南黄极 CVZ 中心(黄纬 -90° 附近): RA≈90°, Dec≈-66.5°
SEC_RA, SEC_DEC = 90.0, -66.5


def query(ra, dec):
    req = json.dumps({"service": "Mast.Caom.Filtered.Position", "format": "json", "params": {
        "columns": "target_name,s_ra,s_dec,sequence_number,t_exptime,t_min,t_max",
        "filters": [{"paramName": "obs_collection", "values": ["TESS"]},
                    {"paramName": "dataproduct_type", "values": ["timeseries"]},
                    {"paramName": "t_exptime", "values": [{"min": 100, "max": 140}]},
                    {"paramName": "sequence_number", "values": [{"min": SECTORS[0], "max": SECTORS[1]}]}],
        "position": f"{ra}, {dec}, {RADIUS}"}})
    r = subprocess.run(["curl", "-s", "--max-time", "45", "-X", "POST", API,
                        "--data-urlencode", f"request={req}"], capture_output=True, text=True)
    try:
        return json.loads(r.stdout).get("data", [])
    except Exception:
        return []


def main():
    want = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    rng = np.random.default_rng(11)
    rows, tried = [], 0
    while len({r["target_name"] for r in rows}) < want and tried < 80:
        # 在 CVZ 中心周围 ±10° 随机取点(CVZ 半径约 12°)
        ra = SEC_RA + float(rng.uniform(-14, 14)) / max(np.cos(np.radians(SEC_DEC)), 0.2)
        dec = SEC_DEC + float(rng.uniform(-9, 9))
        got = query(ra % 360, max(min(dec, -50), -87))
        tried += 1
        if got:
            rows.extend(got)
            n = len({r["target_name"] for r in rows})
            print(f"  [{tried:2d}] RA={ra%360:6.1f} Dec={dec:6.1f} → +{len(got):3d} 条, 累计 {n} 颗", file=sys.stderr)
        time.sleep(0.2)

    df = pd.DataFrame(rows).drop_duplicates(subset=["target_name", "sequence_number"])
    # ★ 关键: 优先保留【被多个 sector 重复观测】的星 —— 时间延续性
    nsec = df.groupby("target_name").sequence_number.nunique().rename("n_sectors")
    df = df.merge(nsec, on="target_name")
    df = df.sort_values(["n_sectors", "target_name"], ascending=[False, True])
    df.to_csv(DATA + "/tess_targets.csv", index=False)

    print(f"\n[目标] {df.target_name.nunique()} 颗星 / {len(df)} 条观测 → data/tess_targets.csv")
    print("\n每颗星被观测的 sector 数分布:")
    print(nsec.value_counts().sort_index(ascending=False).head(8).to_string())
    top = df[df.n_sectors >= 3].target_name.nunique()
    print(f"\n被 ≥3 个 sector 重复观测的星: {top} 颗 ← 优先用这批(时间跨度长)")
    from datetime import datetime, timedelta
    d = lambda m: (datetime(1858, 11, 17) + timedelta(days=float(m))).strftime("%Y-%m-%d")
    print(f"时间覆盖: {d(df.t_min.min())} → {d(df.t_max.max())}")


if __name__ == "__main__":
    main()
