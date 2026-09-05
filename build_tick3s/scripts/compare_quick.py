#!/usr/bin/env python3
"""
compare_quick.py — 快速对比 lob_processor .bin 分片与参考 parquet

用法:
    python scripts/compare_quick.py [--bin-dir DIR] [--ref FILE] [--date DATE] [--cutoff HH:MM]
默认值:
    --bin-dir  <tick3s_bin>/20260611   # 默认取自 qr.toml 的 tick3s_bin
    --ref      /home/yhzhou/20260611_tick_3s.parquet
    --date     20260611
    --cutoff   14:57  (排除收盘竞价段)
"""
import argparse, struct
import numpy as np
import pyarrow.parquet as pq
import pandas as pd
from qr import paths

STOCKS = [
    ('sh600000', 600000., 'SH主板'),
    ('sh688981', 688981., 'SH科创'),
    ('sz000001', 1.,      'SZ主板'),
    ('sz300750', 300750., 'SZ创业'),
]

def read_bin(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'L3SBIN01', f"bad magic: {path}"
    o = 8
    nc, nr = struct.unpack_from('<II', b, o); o += 8
    cols = []
    for _ in range(nc):
        nl, = struct.unpack_from('<H', b, o); o += 2
        nm = b[o:o+nl].decode(); o += nl
        tc, = struct.unpack_from('<B', b, o); o += 1
        cols.append((nm, tc))
    dtype_map = {1: ('<i4', 4), 2: ('<i8', 8), 3: ('<f8', 8)}
    d = {}
    for nm, tc in cols:
        npd, sz = dtype_map[tc]
        d[nm] = np.frombuffer(b, npd, nr, o); o += nr * sz
    return pd.DataFrame(d)

def dt2tsec(v):
    s = str(int(v)).zfill(17)
    return int(s[8:10])*3600 + int(s[10:12])*60 + int(s[12:14])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin-dir', default=str(paths.get('tick3s_bin', '20260611')))
    ap.add_argument('--ref',     default=str(paths.get('tick3s_ref', '20260611_tick_3s.parquet')))
    ap.add_argument('--date',    default='20260611')
    ap.add_argument('--cutoff',  default='14:57')
    args = ap.parse_args()

    h, m = map(int, args.cutoff.split(':'))
    cutoff_sec = h * 3600 + m * 60

    ref_all = pq.read_table(args.ref).to_pandas()
    ref_all['tsec'] = ref_all['dateTime'].round().astype('int64').map(dt2tsec)

    for sym, iid, label in STOCKS:
        bin_path = f'{args.bin_dir}/{sym}.bin'
        try:
            gen = read_bin(bin_path)
        except FileNotFoundError:
            print(f"\n{label}: 跳过（{bin_path} 不存在）")
            continue

        gen['tsec'] = gen['dateTime'].astype('int64').map(dt2tsec)
        gen = gen[gen['tsec'] < cutoff_sec]
        ref = ref_all[ref_all['instrumentID'] == iid].copy()
        ref = ref[ref['tsec'] < cutoff_sec]
        m = gen.merge(ref, on='tsec', suffixes=('_gen', '_ref'))

        results = []
        for col in [c.replace('_gen', '') for c in m.columns if c.endswith('_gen')]:
            if col == 'tsec':
                continue
            gc = m[f'{col}_gen'].values.astype(float)
            rc = m[f'{col}_ref'].values.astype(float)
            denom = np.where(np.abs(rc) > 1e-9, np.abs(rc), 1.)
            pct = (np.abs(gc - rc) / denom < 0.01).mean() * 100
            results.append((col, pct, float(np.median(gc)), float(np.median(rc))))

        results.sort(key=lambda x: x[1])
        bad    = [x for x in results if x[1] < 90]
        g100   = sum(1 for x in results if x[1] == 100)
        g99    = sum(1 for x in results if x[1] >= 99)
        g90    = sum(1 for x in results if x[1] >= 90)
        avg    = float(np.mean([x[1] for x in results]))

        print(f"\n{'='*62}")
        print(f"{label}: {len(results)} cols  avg={avg:.1f}%  "
              f"100%={g100}  >=99%={g99}  >=90%={g90}  <90%={len(bad)}")
        print(f"  {'列名':<30} {'match':>7}  {'gen_med':>12}  {'ref_med':>12}")
        print(f"  {'-'*30} {'-'*7}  {'-'*12}  {'-'*12}")
        for col, pct, gm, rm in results:
            flag = ' !' if pct < 90 else ('  ' if pct == 100 else ' ~')
            print(f"{flag} {col:<30} {pct:>6.1f}%  {gm:>12.3f}  {rm:>12.3f}")

if __name__ == '__main__':
    main()
