"""仓库内所有数据路径的唯一解析入口。

解析顺序:环境变量 ``QR_<KEY>`` → 仓库根的 ``qr.toml`` → 内置默认值。
相对路径一律锚定到**仓库根**,不是当前工作目录,所以脚本在哪个目录调用都一样。
路径全部用 :class:`pathlib.Path`,macOS 与 WSL 通用。

用法::

    from qr import paths
    panels = sorted(paths.data("panels").glob("panel_id_*.csv"))
    fig.savefig(paths.bt("ic.png"))          # bt/ 目录自动创建

改路径不要改代码,复制 ``qr.example.toml`` 成 ``qr.toml`` 后改配置,
或临时用环境变量,例如 ``QR_DATA=/Volumes/ext/l3data``。
"""

from __future__ import annotations

import os
from pathlib import Path

#: 仓库根目录。qr/ 就在根下,所以往上一层即可;editable 安装后依然指向源码树。
REPO_ROOT = Path(__file__).resolve().parents[1]

CONFIG_NAME = "qr.toml"

#: 内置默认值。相对路径锚定 REPO_ROOT。
#: 这些默认值只保证"能跑起来",真实数据放哪儿请写进 qr.toml。
DEFAULTS: dict[str, str] = {
    # l3_factor 主线
    "data":        "ashare/l3_factor/data",   # cache/ panels/ tmp/ agn/ tess/ 都在其下
    "bt":          "ashare/l3_factor/bt",     # 评估与回测产物(图/csv)
    # factor(早期 A股)
    "lob_root":    "data/Lob_new",
    "feat_root":   "data/factor/features",
    "res_root":    "data/factor/results",
    # factor(早期 crypto)
    "crypto_data": "data/massive/Crypto_MIN",
    "crypto_feat": "data/factor/crypto_features",
    "crypto_ic":   "data/factor/crypto_ic",
    # US equity minute research
    "us_minute":   "data/massive/unified/flatfiles/stocks/minute_aggs_v1",
    "us_research": "data/us_equity",
    # build_tick3s 对比脚本
    "tick3s_ref":  "data/tick_3s",
    "tick3s_bin":  "data/tick_3s_staging",
    # 原始数据
    "tl_zip":      "data/ashare/TL",               # 通联逐笔 zip, 按日期分子目录
    # C++ 链路(tltoflow / auction / build_tick3s), 见 qr/qr_paths.h
    "flow_root":      "data/Flow_TL",       # tltoflow 输出
    "auction_mx":     "data/Level2_MX",     # auction 竞价 parquet, 其下 .auction/ 是 shard 暂存
    "lob_h5":         "data/Lob_local",     # build_tick3s 输入: 逐笔 LOB 的 H5
    "lob_level2":     "data/Level2",        # build_tick3s 输入: Level2 快照
    "lob_3s":         "data/Lob_new_3s",    # build_tick3s 输出: 3 秒切片 csv
    "tick3s_parquet": "data/Lob_new_3s_MX", # build_tick3s 输出: parquet shard, 其下 .staging/
    # 可执行文件
    "vsim":        "ashare/vorder_sim/build/vsim",
}


def _load_config() -> dict[str, str]:
    """读取仓库根的 qr.toml。文件不存在就返回空表,不报错。"""
    cfg = REPO_ROOT / CONFIG_NAME
    if not cfg.is_file():
        return {}
    try:
        import tomllib
    except ModuleNotFoundError as exc:  # Python < 3.11
        raise RuntimeError(
            f"读取 {cfg} 需要 Python 3.11+ 的 tomllib;"
            "或改用环境变量 QR_<KEY> 指定路径。"
        ) from exc
    with cfg.open("rb") as fh:
        raw = tomllib.load(fh)
    section = raw.get("paths", raw)
    return {k: str(v) for k, v in section.items() if isinstance(v, str)}


_CONFIG = _load_config()


def get(key: str, *parts: str) -> Path:
    """解析一个路径键,可继续拼接子路径。

    ``parts`` 用 ``/`` 拼在结果后面,由 pathlib 处理平台分隔符。
    """
    raw = os.environ.get("QR_" + key.upper()) or _CONFIG.get(key) or DEFAULTS.get(key)
    if raw is None:
        known = ", ".join(sorted(DEFAULTS))
        raise KeyError(f"未知路径键 {key!r};已知的有:{known}")
    path = Path(raw).expanduser()
    if not path.is_absolute():
        path = REPO_ROOT / path
    return path.joinpath(*parts) if parts else path


def data(*parts: str) -> Path:
    """l3_factor 数据根,例如 ``paths.data("cache", date)``。"""
    return get("data", *parts)


def bt(*parts: str) -> Path:
    """回测/评估产物目录。目录会被自动创建,可直接往里写文件。"""
    target = get("bt", *parts)
    (target.parent if parts else target).mkdir(parents=True, exist_ok=True)
    return target


def out(key: str, *parts: str) -> Path:
    """通用输出路径:同 :func:`get`,但确保父目录存在。"""
    target = get(key, *parts)
    (target.parent if parts else target).mkdir(parents=True, exist_ok=True)
    return target


def vsim() -> Path:
    """vorder_sim 可执行文件。

    自动适配 Windows 的 ``.exe`` 后缀,以及 MSVC 多配置生成器把产物放进
    ``build/Release/`` 的习惯。找不到就报一条能照着做的错误。
    """
    base = get("vsim")
    candidates = [base, base.with_name(base.name + ".exe")]
    for cfg in ("Release", "Debug"):
        candidates += [
            base.parent / cfg / base.name,
            base.parent / cfg / (base.name + ".exe"),
        ]
    for cand in candidates:
        if cand.is_file():
            return cand
    raise FileNotFoundError(
        f"找不到 vsim 可执行文件,已尝试:\n  "
        + "\n  ".join(str(c) for c in candidates)
        + "\n先构建:cmake -S ashare/vorder_sim -B ashare/vorder_sim/build && "
          "cmake --build ashare/vorder_sim/build -j\n"
          "或用 QR_VSIM 指向已有的二进制。"
    )


# ── 给脚本用的字符串常量 ──────────────────────────────────────────────────────
# 研究脚本里大量拼接路径, 用常量比函数调用改动更小、可读性更好:
#     glob.glob(f"{DATA}/panels/panel_id_*.csv")
#     fig.savefig(f"{BT}/ic.png")
DATA = str(get("data"))
BT = str(get("bt"))

# bt/ 是产物目录, 脚本直接往里 savefig/to_csv, 先确保存在。
# 只读位置下静默跳过, 不让 import 失败。
try:
    get("bt").mkdir(parents=True, exist_ok=True)
except OSError:
    pass
