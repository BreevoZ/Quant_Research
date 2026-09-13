#!/usr/bin/env python3
"""把 auction_lob_main 的每股 action 分片合并成当日全市场 parquet。

auction_lob_main 跑完一天后, 分片落在
``<staging>/<YYYYMMDD>/{sh|sz}{symbol}_action.bin``。本脚本把它们拼成一张表
写出 ``<out>/<YYYYMMDD>_auction.parquet``, 这是 auction 生产流程的第 3 步。

用法::

    python merge_action_parquet.py -d 20240130
    python merge_action_parquet.py -d 20240130 --staging /path/.auction --out /path
    python merge_action_parquet.py -d 20240130 --clean   # 成功后删掉分片

不传 --staging/--out 时按 qr.toml 的 ``auction_mx`` 解析
(环境变量 QR_AUCTION_MX 优先), 与 cpp/run.sh 的默认值一致。
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from qr import paths
from qr.shardbin import ACTION_MAGIC, read_shard_dir

#: 排序键。eventSeq 是逐事件撮合的序号, 同一只股票内严格递增。
SORT_KEYS = ["instrumentID", "eventSeq"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-d", "--date", required=True, help="交易日 YYYYMMDD")
    ap.add_argument("--staging", help="分片根目录 (默认 <auction_mx>/.auction)")
    ap.add_argument("--out", help="parquet 输出目录 (默认 <auction_mx>)")
    ap.add_argument("--compression", default="zstd",
                    help="parquet 压缩算法 (默认 zstd)")
    ap.add_argument("--clean", action="store_true",
                    help="写出成功后删除该日的分片目录")
    args = ap.parse_args()

    base = paths.get("auction_mx")
    staging = Path(args.staging) if args.staging else base / ".auction"
    out_dir = Path(args.out) if args.out else base

    shard_dir = staging / args.date
    if not shard_dir.is_dir():
        print(f"错误: 分片目录不存在 {shard_dir}", file=sys.stderr)
        print("先用 cpp/build/auction_lob_main -d <date> 跑这一天。", file=sys.stderr)
        return 1

    df = read_shard_dir(shard_dir, magic=ACTION_MAGIC, pattern="*_action.bin")

    # C++ 侧的 instrumentID 只是纯数字代码, 沪深会撞号(如 600000 与 000001 前缀不同
    # 但 000001 会被 stoll 成 1)。文件名带市场前缀, 是唯一可靠的市场来源。
    df.insert(1, "market", df["symbol"].str[:2].str.upper())
    df["tradedate"] = int(args.date)

    df = df.sort_values(["market"] + SORT_KEYS, kind="stable", ignore_index=True)

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{args.date}_auction.parquet"
    pq.write_table(pa.Table.from_pandas(df, preserve_index=False),
                   out_path, compression=args.compression)

    size_mb = out_path.stat().st_size / 1024 / 1024
    n_sym = df["symbol"].nunique()
    print(f"[{args.date}] {len(df)} 行 / {n_sym} 只 -> {out_path} ({size_mb:.1f} MB)")

    if args.clean:
        shutil.rmtree(shard_dir)
        print(f"[{args.date}] 已删除分片目录 {shard_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
