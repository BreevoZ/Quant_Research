#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
读 build_tick3s 的 .bin 分片(自描述列式格式), 返回 DataFrame。

格式(README):
  [0..8)   magic "L3SBIN01"
  [8..12)  uint32 num_cols
  [12..16) uint32 num_rows
  每列:    uint16 name_len | name(UTF-8) | uint8 type_code (1=int32 2=int64 3=float64)
  body:    按列顺序, 每列 num_rows * sizeof(type) 字节
"""
import struct
import numpy as np, pandas as pd
from qr import paths

_DT = {1: np.int32, 2: np.int64, 3: np.float64}


def read_bin(path):
    with open(path, "rb") as f:
        buf = f.read()
    assert buf[:8] == b"L3SBIN01", f"magic 不符: {buf[:8]!r}"
    ncol, nrow = struct.unpack_from("<II", buf, 8)
    off = 16
    cols = []
    for _ in range(ncol):
        (nl,) = struct.unpack_from("<H", buf, off); off += 2
        name = buf[off:off+nl].decode("utf-8"); off += nl
        (tc,) = struct.unpack_from("<B", buf, off); off += 1
        cols.append((name, _DT[tc]))
    data = {}
    for name, dt in cols:
        n = nrow * np.dtype(dt).itemsize
        data[name] = np.frombuffer(buf, dtype=dt, count=nrow, offset=off)
        off += n
    return pd.DataFrame(data)


if __name__ == "__main__":
    import sys
    df = read_bin(sys.argv[1] if len(sys.argv) > 1
                  else str(paths.get("tick3s_bin", "20260713", "sz000001.bin")))
    print(f"{df.shape[0]} 行 × {df.shape[1]} 列")
    groups = {
        "激进度分类(type1-6)": [c for c in df.columns if "Type" in c],
        "近触价撤单": [c for c in df.columns if c.startswith("cl") and "1" in c],
        "全深度挂单": [c for c in df.columns if "pending" in c and "Total" in c],
        "档位分桶委托": [c for c in df.columns if c.startswith("wt") and ("2_4" in c or "5_10" in c or "11Inf" in c)],
        "主动/被动拆分": [c for c in df.columns if c.startswith(("wtA", "wtP"))],
    }
    for g, cs in groups.items():
        if cs:
            nz = df[cs].astype(float).abs().sum().gt(0).sum()
            print(f"\n[{g}] {len(cs)} 列, 非全零 {nz} 列")
            print("  " + ", ".join(cs[:6]) + (" ..." if len(cs) > 6 else ""))
            print(f"  样例: {df[cs[0]].describe()[['mean','max']].round(2).to_dict()}")
