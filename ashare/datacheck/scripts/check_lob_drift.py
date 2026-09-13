#!/usr/bin/env python3
"""lob_builder 三份副本的漂移检查。

`lob_builder.{h,cpp}` 没做成共享库,而是被拷进了另外两个项目:

* ``datacheck/lob_builder.*``          —— 基准实现
* ``vorder_sim/lob_builder.*``         —— 基准的拷贝 + 纯附加扩展
* ``auction/cpp/auction_lob_builder.*`` —— 独立实现, 靠逐处移植保持一致

在基准里修了规则,不会自动传到另外两处。这个脚本把「当前已知的差异」固化成基线,
以后任何一份改动只要产生了基线之外的新差异就报错,提醒你三处同步。

用法::

    python ashare/datacheck/scripts/check_lob_drift.py            # 检查, 有新漂移则退出码 1
    python ashare/datacheck/scripts/check_lob_drift.py --update   # 复核过后刷新基线

比对前会把每行行尾空白去掉,纯格式抖动不算漂移。
auction 那份是独立实现,逐行 diff 没有意义,只检查「移植自 datacheck」标注的条数。
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

ASHARE_ROOT = Path(__file__).resolve().parents[2]
BASELINE = ASHARE_ROOT / "datacheck" / "scripts" / "lob_builder_drift.baseline"

#: (基准, 拷贝) 成对比较的文件
PAIRS = [
    ("datacheck/lob_builder.h", "vorder_sim/lob_builder.h"),
    ("datacheck/lob_builder.cpp", "vorder_sim/lob_builder.cpp"),
]

#: auction 那份里标注移植来源的注释。只数「移植自」三个字, 因为有的标注换行了
#: (「移植自\n// datacheck_final」), 连着匹配会漏。这个文件里它只出现在移植标注里。
PORT_MARKER = "移植自"
AUCTION_FILE = "auction/cpp/auction_lob_builder.cpp"

_MARKER_LINE = re.compile(r"^# auction_port_markers:\s*(\d+)\s*$")


def normalize(path: Path) -> list[str]:
    """按行读取并去掉行尾空白,避免纯格式改动被当成漂移。"""
    return [line.rstrip() for line in path.read_text().splitlines()]


def build_report() -> str:
    """生成当前的漂移报告,内容与基线文件一一对应。"""
    marker_count = (ASHARE_ROOT / AUCTION_FILE).read_text().count(PORT_MARKER)
    out = [f"# auction_port_markers: {marker_count}"]
    for base_rel, copy_rel in PAIRS:
        base, copy = ASHARE_ROOT / base_rel, ASHARE_ROOT / copy_rel
        for p in (base, copy):
            if not p.is_file():
                raise SystemExit(f"缺文件: {p}")
        diff = difflib.unified_diff(
            normalize(base), normalize(copy),
            fromfile=base_rel, tofile=copy_rel, lineterm="", n=2,
        )
        out.extend(diff)
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--update", action="store_true",
                    help="把当前差异写成新基线(改动复核通过后再用)")
    args = ap.parse_args()

    report = build_report()

    if args.update:
        BASELINE.write_text(report)
        print(f"基线已刷新: {BASELINE.relative_to(ASHARE_ROOT)}")
        return 0

    if not BASELINE.is_file():
        print(f"错误: 基线不存在 {BASELINE}", file=sys.stderr)
        print("先跑一次 --update 建立基线。", file=sys.stderr)
        return 2

    expected = BASELINE.read_text()
    if report == expected:
        m = _MARKER_LINE.match(report.splitlines()[0])
        n = m.group(1) if m else "?"
        print(f"lob_builder 无新漂移(vorder_sim 差异与基线一致, auction 移植标注 {n} 处)")
        return 0

    print("检测到基线之外的新漂移。下面是「基线」与「当前」的差异:\n", file=sys.stderr)
    meta = difflib.unified_diff(
        expected.splitlines(), report.splitlines(),
        fromfile="基线", tofile="当前", lineterm="",
    )
    for line in meta:
        print(line, file=sys.stderr)
    print("\n请确认三份实现的重建规则仍然一致; 复核无误后跑 --update 刷新基线。",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
