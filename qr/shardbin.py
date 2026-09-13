"""读取 C++ 侧写出的列主序二进制 shard。

两条链路用同一套布局,只有魔数不同:

* ``L3SBIN01`` —— ``build_tick3s`` 的 3 秒切片分片,由
  ``LobDataProcessor::saveToBinaryShard`` 写出。
* ``ACTBIN01`` —— ``auction`` 的集合竞价 action 分片,由
  ``TLLobBuilder::writeActionBin`` 写出。

布局(全部小端)::

    [0..8)    magic, 8 字节 ASCII
    [8..12)   uint32 num_cols
    [12..16)  uint32 num_rows
    每列:     uint16 name_len, name_len 字节列名, uint8 type_code
    Body:     按列顺序逐列写 num_rows 个元素

``type_code``: 1=int32, 2=int64, 3=float64。

用法::

    from qr.shardbin import read_shard, read_shard_dir
    df = read_shard("sh600000.bin")
    df = read_shard_dir("staging/20260611", magic=b"L3SBIN01")
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pandas as pd

#: type_code → (numpy dtype, 元素字节数)
_DTYPES: dict[int, tuple[str, int]] = {
    1: ("<i4", 4),
    2: ("<i8", 8),
    3: ("<f8", 8),
}

L3S_MAGIC = b"L3SBIN01"
ACTION_MAGIC = b"ACTBIN01"


def read_shard(path: str | Path, magic: bytes | None = None) -> pd.DataFrame:
    """读一个 shard 文件。

    ``magic`` 给定时校验魔数,不匹配就报错;不给就接受任何已知魔数。
    行数为 0 的 shard 返回一个只有列名、没有行的 DataFrame。
    """
    path = Path(path)
    raw = path.read_bytes()
    if len(raw) < 16:
        raise ValueError(f"{path} 太短({len(raw)} 字节),不是合法 shard")

    found = raw[:8]
    if magic is not None:
        if found != magic:
            raise ValueError(f"{path} 魔数是 {found!r},期望 {magic!r}")
    elif found not in (L3S_MAGIC, ACTION_MAGIC):
        raise ValueError(f"{path} 魔数 {found!r} 不认识")

    off = 8
    num_cols, num_rows = struct.unpack_from("<II", raw, off)
    off += 8

    cols: list[tuple[str, int]] = []
    for _ in range(num_cols):
        (name_len,) = struct.unpack_from("<H", raw, off)
        off += 2
        name = raw[off:off + name_len].decode()
        off += name_len
        (type_code,) = struct.unpack_from("<B", raw, off)
        off += 1
        if type_code not in _DTYPES:
            raise ValueError(f"{path} 列 {name} 的 type_code={type_code} 不认识")
        cols.append((name, type_code))

    frame: dict[str, np.ndarray] = {}
    for name, type_code in cols:
        dtype, size = _DTYPES[type_code]
        need = off + num_rows * size
        if need > len(raw):
            raise ValueError(
                f"{path} 数据被截断:读到列 {name} 时需要 {need} 字节,文件只有 {len(raw)}"
            )
        frame[name] = np.frombuffer(raw, dtype, num_rows, off)
        off += num_rows * size

    return pd.DataFrame(frame)


def read_shard_dir(
    directory: str | Path,
    magic: bytes | None = None,
    pattern: str = "*.bin",
    add_symbol: bool = True,
) -> pd.DataFrame:
    """读一个目录下的全部 shard 并纵向拼接。

    ``add_symbol=True`` 时按文件名补一列 ``symbol``(去掉 ``.bin`` 和
    ``_action`` 后缀),便于按股票分组。目录里没有 shard 时抛
    :class:`FileNotFoundError`。
    """
    directory = Path(directory)
    files = sorted(directory.glob(pattern))
    if not files:
        raise FileNotFoundError(f"{directory} 下没有匹配 {pattern} 的 shard")

    parts = []
    for f in files:
        df = read_shard(f, magic)
        if df.empty:
            continue
        if add_symbol:
            df.insert(0, "symbol", f.stem.replace("_action", ""))
        parts.append(df)

    if not parts:
        raise ValueError(f"{directory} 下 {len(files)} 个 shard 全是空的")
    return pd.concat(parts, ignore_index=True)
