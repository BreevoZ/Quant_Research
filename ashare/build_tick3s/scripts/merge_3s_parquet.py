#!/usr/bin/env python3
"""把 lob_processor 的每股 .bin 分片合并成当日全市场 parquet。

lob_processor 用 ``--output parquet`` 跑完一天后, 分片落在
``<staging>/<YYYYMMDD>/<symbol>.bin``。本脚本把它们拼成一张表写出
``<out>/<YYYYMMDD>_tick.parquet``。

用法::

    python scripts/merge_3s_parquet.py -d 20260611
    python scripts/merge_3s_parquet.py -d 20260611 --staging /path/.staging --out /path
    python scripts/merge_3s_parquet.py -d 20260611 --clean    # 成功后删掉分片

不传 --staging/--out 时按 qr.toml 的 ``tick3s_parquet`` 解析
(环境变量 QR_TICK3S_PARQUET 优先), 与 lob_processor 的默认值一致。
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from qr import paths
from qr.shardbin import L3S_MAGIC, read_shard_dir

#: 排序键。同一只股票按时间递增, 全表按股票分组, 下游按 instrumentID 过滤最快。
SORT_KEYS = ["instrumentID", "dateTime"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-d", "--date", required=True, help="交易日 YYYYMMDD")
    ap.add_argument("--staging", help="分片根目录 (默认 <tick3s_parquet>/.staging)")
    ap.add_argument("--out", help="parquet 输出目录 (默认 <tick3s_parquet>)")
    ap.add_argument("--compression", default="zstd",
                    help="parquet 压缩算法 (默认 zstd)")
    ap.add_argument("--clean", action="store_true",
                    help="写出成功后删除该日的分片目录")
    args = ap.parse_args()

    base = paths.get("tick3s_parquet")
    staging = Path(args.staging) if args.staging else base / ".staging"
    out_dir = Path(args.out) if args.out else base

    shard_dir = staging / args.date
    if not shard_dir.is_dir():
        print(f"错误: 分片目录不存在 {shard_dir}", file=sys.stderr)
        print("先用 lob_processor --output parquet 跑这一天。", file=sys.stderr)
        return 1

    df = read_shard_dir(shard_dir, magic=L3S_MAGIC)
    # symbol 列只用来定位来源文件, instrumentID 已经带了股票标识, 不进 parquet。
    df = df.drop(columns=["symbol"])
    df = df.sort_values(SORT_KEYS, kind="stable", ignore_index=True)

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{args.date}_tick.parquet"
    pq.write_table(pa.Table.from_pandas(df, preserve_index=False),
                   out_path, compression=args.compression)

    size_mb = out_path.stat().st_size / 1024 / 1024
    n_sym = df["instrumentID"].nunique()
    print(f"[{args.date}] {len(df)} 行 / {n_sym} 只 -> {out_path} ({size_mb:.1f} MB)")

    if args.clean:
        shutil.rmtree(shard_dir)
        print(f"[{args.date}] 已删除分片目录 {shard_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
