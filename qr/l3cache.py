#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
读 vsim 的 CachedRec 二进制缓存(ashare/vorder_sim/rec_cache.h 落盘的解析结果)。

【为什么单独一层】: 因子层只该依赖一个"稳定契约", 不该硬扛 UnifiedRecord 的内部布局。
这里把布局 + 版本校验集中在一处; 缓存格式一变, 这里报错(而不是让上层静默读到错位垃圾)。

文件头(28B): MAGIC(8) + parserVersion(uint32,4) + fingerprint(uint64,8) + count(int64,8)
记录区: count × 106B 的 CachedRec(packed)。
"""
import os
import numpy as np

MAGIC = b"VSIMCA02"          # rec_cache.h: VSIM_CACHE_MAGIC(缓存格式标识)
EXPECTED_VERSION = 2         # rec_cache.h: VSIM_PARSER_VERSION —— 变了要人工核对 rec_cache.h 再改这里
HDR = 28

# 106B packed, 逐字段镜像 rec_cache.h 的 struct CachedRec
DT = np.dtype([
    ("seqNo", "<i8"), ("timeSeconds", "<f8"), ("actionType", "<i4"),
    ("direction", "S1"), ("priceType", "S1"), ("tradeDirection", "<i4"),
    ("sysid", "<i8"), ("buyId", "<i8"), ("sellId", "<i8"),
    ("price", "<f8"), ("turnover", "<f8"), ("intPrice", "<i8"), ("volume", "<i8"),
    ("time", "S24"),
], align=False)
assert DT.itemsize == 106, f"CachedRec 布局漂移: itemsize={DT.itemsize} != 106"


def load(path):
    """读一个 .bin -> 结构化 numpy 数组。任何格式不符都抛异常, 绝不静默返回错位数据。"""
    filesz = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(HDR)
        if len(head) < HDR:
            raise ValueError(f"{path}: 文件过短({len(head)}B < {HDR}), 非法缓存")
        if head[:8] != MAGIC:
            raise ValueError(f"{path}: MAGIC={head[:8]!r} != {MAGIC!r} —— 不是 vsim 缓存或格式已变")
        ver = int(np.frombuffer(head[8:12], "<u4")[0])
        if ver != EXPECTED_VERSION:
            raise ValueError(
                f"{path}: 缓存 parserVersion={ver} != 期望 {EXPECTED_VERSION} —— "
                f"缓存疑被新解析器重生成, 语义/布局可能已变; 核对 rec_cache.h 后再更新 EXPECTED_VERSION")
        n = int(np.frombuffer(head[20:28], "<i8")[0])
        if filesz != HDR + n * 106:
            raise ValueError(f"{path}: 文件总长 {filesz} != {HDR}+{n}*106 —— 记录尺寸漂移/文件截断")
        body = f.read(n * 106)
    return np.frombuffer(body, dtype=DT, count=n)
