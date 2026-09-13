#!/usr/bin/env python3
"""单日流水线:解压 SZ 两个文件 → vsim precache 转二进制缓存 → 抽特征落盘 → 删 CSV。

    python ashare/l3_factor/runday.py 20260707 [--keep-csv] [--drop-cache] [--workers N]

在任何工作目录下调用都一样。路径来自 ``qr/paths.py``,见仓库根的 ``qr.example.toml``。

磁盘:峰值约 60GB/天(CSV ≤28GB + 缓存 ~30GB);结束后只留缓存与面板(~120MB)。
这是 runday.sh 的移植版,原脚本依赖 GNU ``df --output`` 与外部 ``unzip``,mac 与
Windows 上都跑不了;这里全部改用标准库,三个平台行为一致。
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from qr import paths

SZ_FILES = ("mdl_6_33_0", "mdl_6_36_0")   # 委托 / 成交撤单
MIN_FREE_GB = 300                          # 磁盘余量刹车线


def unzip_one(zip_path: Path, stem: str, tmp: Path) -> None:
    """解压到临时子目录再改名,保证原子性。

    中断留下半截 csv 会被后续的"已存在"判断误认为解压完成,静默喂进截断数据。
    """
    target = tmp / f"{stem}.csv"
    if target.is_file():
        return
    staging = tmp / f".x_{stem}"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(staging)
    extracted = staging / f"{stem}.csv"
    if not extracted.is_file():                       # zip 内层目录名可能不同
        found = list(staging.rglob(f"{stem}.csv"))
        if not found:
            raise FileNotFoundError(f"{zip_path} 里找不到 {stem}.csv")
        extracted = found[0]
    extracted.replace(target)
    shutil.rmtree(staging, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("date", help="YYYYMMDD")
    ap.add_argument("--keep-csv", action="store_true", help="保留解压出的 CSV")
    ap.add_argument("--drop-cache", action="store_true",
                    help="抽完面板即删当日缓存(面板 ~120MB/天 vs 缓存 ~29GB/天)")
    ap.add_argument("--workers", type=int, default=60,
                    help="vsim precache 线程数(默认 60)")
    a = ap.parse_args()
    date = a.date

    base = paths.data()
    tmp, cache, panel = base / "tmp" / date, base / "cache", base / "panels"
    zip_dir = paths.get("tl_zip", date)
    for d in (tmp, cache, panel):
        d.mkdir(parents=True, exist_ok=True)

    order_zip = zip_dir / f"{date}_mdl_6_33_0.csv.zip"
    if not order_zip.is_file():
        print(f"❌ {date}: 无 SZ 委托 zip ({order_zip})", file=sys.stderr)
        return 1

    # 幂等:面板已在 → 整天跳过,过夜批中断后可直接重启
    panel_csv = panel / f"panel_id_{date}.csv"
    if panel_csv.is_file():
        print(f"[{date}] ⏭ 面板已存在, 跳过")
        return 0

    free_gb = shutil.disk_usage(base).free / 1024**3
    if free_gb < MIN_FREE_GB:
        print(f"❌ {date}: 磁盘仅剩 {free_gb:.0f}GB (<{MIN_FREE_GB}GB), 刹车", file=sys.stderr)
        return 2

    print(f"[{date}] 1/3 解压 SZ 委托+成交(双文件并行)...")
    with ThreadPoolExecutor(max_workers=len(SZ_FILES)) as pool:
        list(pool.map(lambda s: unzip_one(zip_dir / f"{date}_{s}.csv.zip", s, tmp), SZ_FILES))
    missing = [s for s in SZ_FILES if not (tmp / f"{s}.csv").is_file()]
    if missing:
        print(f"❌ {date} 解压失败: {missing}", file=sys.stderr)
        return 1

    print(f"[{date}] 2/3 vsim precache(TL→二进制缓存)...")
    subprocess.run([str(paths.vsim()), "--precache", "--tl-dir", str(tmp), "--date", date,
                    "--cache-dir", str(cache), "--workers", str(a.workers)], check=True)
    # 因子/回测只用 A股(00/30): 删非股票 bin(转债12/ETF15/LOF16/18/B股20), 省约 18% 缓存
    for pattern in ("tl_sz_1*.bin", "tl_sz_2*.bin"):
        for f in (cache / date).glob(pattern):
            f.unlink()

    print(f"[{date}] 3/3 抽特征 → 面板 ...")
    factor_id = Path(__file__).resolve().parent / "factors" / "factor_id.py"
    subprocess.run([sys.executable, str(factor_id), "--cache-dir", str(cache / date),
                    "--dump", str(panel_csv), "--no-ic"], check=True)

    if not a.keep_csv:
        shutil.rmtree(tmp, ignore_errors=True)
    if a.drop_cache:
        shutil.rmtree(cache / date, ignore_errors=True)

    size = panel_csv.stat().st_size if panel_csv.is_file() else 0
    print(f"[{date}] ✅ 完成: 面板 {size:,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
