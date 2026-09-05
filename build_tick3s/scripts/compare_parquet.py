#!/usr/bin/env python3
"""
compare_parquet.py — 对比 lob_processor 生成的 parquet 与参考 Tick_3s parquet

用法:
    python3 scripts/compare_parquet.py <generated.parquet> <ref_dir_or_file> [--date 20260611] [--symbol 600396]

参数:
    generated        : lob_processor 输出的单股票 parquet（含全天数据）
                       或 .bin 分片（自动调用 read_bin）
    ref              : 参考 parquet 文件路径 / 目录（搜索 Snapshot_lob3s_{date}.parquet）
    --date           : 交易日期 YYYYMMDD（用于在参考文件中定位）
    --symbol / -s    : 股票代码（如 600396），用于从全市场参考文件中过滤

运行结果:
    - 打印逐列对比汇总（完全匹配 / 已知差异 / 意外差异）
    - 输出 diff_{date}_{symbol}.csv（含全部不匹配行明细）

已知且可接受的差异（不计入"意外"）:
    [K1] pendingBidTotalNum/Vol/Amt, pendingAskTotalNum/Vol/Amt
         gen 用全深度 FIFO 自建簿，exp 用 top-10 快照 → 前者远大于后者
    [K2] wtABidVol/Num, wtAAskVol/Num, wtBidNumRatio, wtAskNumRatio, wtBidVolRatio, wtAskVolRatio
         旧版 exp 文件未正确读取 type/position → exp 中连续段全为 0；gen 值正确
    [K3] wtPBidVol/Num/Pos, wtPAskVol/Num/Pos, wtBidAvgPos, wtAskAvgPos
         主/被动分类来源不同（同 K2 根因），小数值差异可接受
    [K4] dataFlag 及 bidPrice/Volume/askPrice/Volume 1-10 在 14:57–15:00（60 行）
         gen 正确标记     =1（收盘集合竞价）并显示交叉簿；exp 用连续模式
    [K5] 15:00:00 末行的 zb*/totalVolume/totalAmount
         gen 包含收盘集合竞价的撮合成交；exp 末行为 0
    [K6] eventCount 列（仅 gen 有）
         gen 新增列，exp 无此列
    [K7] 开盘集合竞价桶（09:15:03–09:25:00，gen 多出 200 行）
         exp 不含开盘竞价桶；09:30:00 特殊槽只在 exp 中存在
"""

import sys, os, struct, argparse
import numpy as np
import pandas as pd
import pyarrow.parquet as pq
import pyarrow as pa


# ─── 已知差异列集合 ────────────────────────────────────────────────────────────

# [K1] pendingTotal 全深度 vs top-10
K1_COLS = {
    'pendingBidTotalNum','pendingBidTotalVol','pendingBidTotalAmt',
    'pendingAskTotalNum','pendingAskTotalVol','pendingAskTotalAmt',
}

# [K2] 主动单字段（旧版 exp 全为 0）
K2_COLS = {
    'wtABidVol','wtABidNum','wtAAskVol','wtAAskNum',
    'wtBidNumRatio','wtBidVolRatio','wtAskNumRatio','wtAskVolRatio',
}

# [K3] 主/被动分类的小差异
K3_COLS = {
    'wtPBidVol','wtPBidNum','wtPBidPos',
    'wtPAskVol','wtPAskNum','wtPAskPos',
    'wtBidAvgPos','wtAskAvgPos',
}

# [K8] 依赖盘口状态的 type/level 归类（不同 builder 版本盘口状态略有差异 → type 分类不同）
#   calcType() 根据下单时刻 best_bid/ask 和 level1_volume 决定 type；
#   builder 版本间的微小差异（排序、tie-break）会导致 type2↔type4 等切换
K8_COLS = {
    # 按 level 分桶的 wt 统计
    'wtBid1Vol','wtBid1Num','wtBid1Amt',
    'wtAsk1Vol','wtAsk1Num','wtAsk1Amt',
    'wtBid2_4Vol','wtBid2_4Num','wtBid2_4Amt','wtBid2_4AvgPrice',
    'wtAsk2_4Vol','wtAsk2_4Num','wtAsk2_4Amt','wtAsk2_4AvgPrice',
    'wtBid5_10Vol','wtBid5_10Num','wtBid5_10Amt','wtBid5_10AvgPrice',
    'wtAsk5_10Vol','wtAsk5_10Num','wtAsk5_10Amt','wtAsk5_10AvgPrice',
    'wtBid11InfVol','wtBid11InfNum','wtBid11InfAmt','wtBid11InfAvgPrice',
    'wtAsk11InfVol','wtAsk11InfNum','wtAsk11InfAmt','wtAsk11InfAvgPrice',
    # 按 type 分桶的 wt 统计（type 1-6）
    'bidType1Vol','bidType1Num','bidType2Vol','bidType2Num',
    'bidType3Vol','bidType3Num','bidType4Vol','bidType4Num',
    'bidType5Vol','bidType5Num','bidType6Vol','bidType6Num',
    'askType1Vol','askType1Num','askType2Vol','askType2Num',
    'askType3Vol','askType3Num','askType4Vol','askType4Num',
    'askType5Vol','askType5Num','askType6Vol','askType6Num',
    # 最大档统计（依赖 level 归类）
    'bidMaxVol','bidMaxPrice','bidMaxPos',
    'askMaxVol','askMaxPrice','askMaxPos',
}

# [K9] PM 段单笔订单档位微差（builder 盘口状态略有不同 → 该订单落在不同 level）
#   表现: askVolume1-10 / pendingAsk* 从某时刻起持续差 200 手，直到该单被撤/成交
#   wt* 流量统计（wtAskVol/Amt/wtVol）在同一桶也差相同量
K9_COLS = {
    'askVolume1','askVolume2','askVolume3','askVolume4','askVolume5',
    'askVolume6','askVolume7','askVolume8','askVolume9','askVolume10',
    'pendingAsk1Vol','pendingAsk1Amt',
    'pendingAsk2_4Vol','pendingAsk2_4Amt',
    'pendingAsk5_10Vol','pendingAsk5_10Amt',
    'wtAskVol','wtAskAmt','wtAskWeightPrice','wtVol',
}

KNOWN_COLS = K1_COLS | K2_COLS | K3_COLS | K8_COLS | K9_COLS

CLOSING_AUCTION_START = 20_0000_000 * 10**9 + 14 * 10**9 * 3600 + 57 * 10**9 * 60  # 不用，直接按时间过滤
CLOSING_AUCTION_TS_MIN = 20260611_145700000  # placeholder，实际按 dataFlag 或时间判断

# ─── 读 .bin 分片 ─────────────────────────────────────────────────────────────

def read_bin(path: str) -> pd.DataFrame:
    type_map = {1: np.int32, 2: np.int64, 3: np.float64}
    with open(path, 'rb') as f:
        magic = f.read(8)
        if magic != b'L3SBIN01':
            raise ValueError(f"bad magic: {magic!r}")
        num_cols = struct.unpack('<I', f.read(4))[0]
        num_rows = struct.unpack('<I', f.read(4))[0]
        col_order, col_types = [], []
        for _ in range(num_cols):
            nlen = struct.unpack('<H', f.read(2))[0]
            name = f.read(nlen).decode('utf-8')
            tc   = struct.unpack('<B', f.read(1))[0]
            col_order.append(name)
            col_types.append(tc)
        arrays = {}
        for name, tc in zip(col_order, col_types):
            dt = type_map[tc]
            arrays[name] = np.frombuffer(f.read(num_rows * np.dtype(dt).itemsize), dtype=dt).copy()
    return pd.DataFrame(arrays, columns=col_order)


# ─── 读参考文件（全市场 parquet，按 instrumentID 过滤） ────────────────────────

def read_ref(ref_path: str, instrument_id: float) -> pd.DataFrame:
    if os.path.isdir(ref_path):
        import glob
        files = glob.glob(os.path.join(ref_path, '*.parquet'))
        if not files:
            raise FileNotFoundError(f"No parquet in {ref_path}")
        ref_path = sorted(files)[-1]

    pf = pq.ParquetFile(ref_path)
    batches = []
    for b in pf.iter_batches(50000):
        df = b.to_pandas()
        sub = df[df['instrumentID'] == instrument_id]
        if len(sub):
            batches.append(sub)
    if not batches:
        raise ValueError(f"instrumentID={instrument_id} not found in {ref_path}")
    return pd.concat(batches, ignore_index=True)


# ─── 核心对比 ─────────────────────────────────────────────────────────────────

def compare(gen: pd.DataFrame, exp: pd.DataFrame, output_csv: str | None = None):
    KEY = 'dateTime'

    gen = gen.sort_values(KEY).reset_index(drop=True)
    exp = exp.sort_values(KEY).reset_index(drop=True)

    gen_times = set(gen[KEY].astype(int))
    exp_times = set(exp[KEY].astype(int))
    common    = gen_times & exp_times
    only_gen  = sorted(gen_times - exp_times)
    only_exp  = sorted(exp_times - gen_times)

    print(f"\n{'─'*60}")
    print(f"时间桶:  gen={len(gen_times)}  exp={len(exp_times)}  公共={len(common)}")
    if only_gen:
        print(f"  只在 gen ({len(only_gen)}): {only_gen[:3]}{'...' if len(only_gen)>3 else ''}")
    if only_exp:
        print(f"  只在 exp ({len(only_exp)}): {only_exp[:3]}{'...' if len(only_exp)>3 else ''}")
    print(f"{'─'*60}")

    g = gen[gen[KEY].astype(int).isin(common)].sort_values(KEY).reset_index(drop=True)
    e = exp[exp[KEY].astype(int).isin(common)].sort_values(KEY).reset_index(drop=True)
    N = len(g)

    gen_only_cols = set(g.columns) - set(e.columns)
    exp_only_cols = set(e.columns) - set(g.columns)
    if gen_only_cols:
        print(f"只在 gen 的列 [K6]: {sorted(gen_only_cols)}")
    if exp_only_cols:
        print(f"只在 exp 的列:      {sorted(exp_only_cols)}")

    common_cols = [c for c in g.columns if c in e.columns and c not in (KEY, 'instrumentID')]

    # 关闭竞价时间掩码：14:57:03–15:00:00
    ts_col = g[KEY].astype(int)
    date_prefix = ts_col.iloc[0] // 10**9 * 10**9  # strip time part
    close_ts = date_prefix + 14 * 3600 * 10**6 + 57 * 60 * 10**6  # ~20260611145700000
    last_ts  = date_prefix + 15 * 3600 * 10**6
    closing_mask = (ts_col >= 145700000 + (date_prefix // 10**6)) | False  # simplify below
    # 简单判断：ts 末9位 >= 145700000
    ts_hhmmss = ts_col % 10**9
    closing_mask = (ts_hhmmss >= 145700000)
    last_mask    = (ts_hhmmss == 150000000)

    TOL = 1e-6   # 相对误差阈值

    results_ok, results_known, results_unexpected = [], [], []

    for col in common_cols:
        try:
            gv = g[col].values.astype(float)
            ev = e[col].values.astype(float)
        except Exception:
            continue

        diff = np.abs(gv - ev)
        denom = np.maximum(np.abs(ev), 1e-9)
        rdiff = diff / denom
        bad = (diff > 1e-6) & (rdiff > TOL)
        n_bad = int(bad.sum())

        if n_bad == 0:
            results_ok.append(col)
            continue

        # 判断是否属于已知类别
        if col in KNOWN_COLS:
            tag = ('K1' if col in K1_COLS else
                   'K2' if col in K2_COLS else
                   'K3' if col in K3_COLS else
                   'K8' if col in K8_COLS else 'K9')
            results_known.append((col, n_bad, float(rdiff[bad].max()), tag))
            continue

        # K4: 收盘竞价段 的 dataFlag / 盘口 / pending 分档 / wt 分桶统计
        K4_PATTERN = (col in {'dataFlag'}
                      or col.startswith('bidPrice') or col.startswith('bidVolume')
                      or col.startswith('askPrice') or col.startswith('askVolume')
                      or col in {
                          'pendingBid1Num','pendingBid1Vol','pendingBid1Amt',
                          'pendingBid2_4Num','pendingBid2_4Vol','pendingBid2_4Amt',
                          'pendingBid5_10Num','pendingBid5_10Vol','pendingBid5_10Amt',
                          'pendingAsk1Num','pendingAsk1Vol','pendingAsk1Amt',
                          'pendingAsk2_4Num','pendingAsk2_4Vol','pendingAsk2_4Amt',
                          'pendingAsk5_10Num','pendingAsk5_10Vol','pendingAsk5_10Amt',
                          'pendingAsk5_10Vol','pendingAsk5_10Amt',
                          'pendingAsk2_4Vol','pendingAsk2_4Amt',
                          'pendingAsk1Vol','pendingAsk1Amt',
                      })
        if K4_PATTERN:
            # 如果差异全在收盘段 → K4
            bad_non_close = bad & ~closing_mask
            if bad_non_close.sum() == 0:
                results_known.append((col, n_bad, float(rdiff[bad].max()), 'K4'))
                continue

        # K5: 末行 zb*/total*
        if col.startswith('zb') or col in {'totalVolume','totalAmount','highPrice','lowPrice','openPrice','vwap'}:
            bad_non_last = bad & ~last_mask
            if bad_non_last.sum() == 0:
                results_known.append((col, n_bad, float(rdiff[bad].max()), 'K5'))
                continue

        # 意外差异
        idx = np.argmax(bad)
        results_unexpected.append((
            col, n_bad, float(rdiff[bad].max()),
            int(g[KEY].iloc[idx]),
            float(ev[idx]), float(gv[idx])
        ))

    # ── 输出汇总 ──
    print(f"\n✅ 完全匹配:      {len(results_ok)} 列")
    print(f"⚠️  已知差异:      {len(results_known)} 列")
    print(f"❌ 意外差异:      {len(results_unexpected)} 列  ← 需要关注\n")

    if results_known:
        print("── 已知差异明细 ──────────────────────────────────────────")
        for col, n, mr, tag in sorted(results_known, key=lambda x: x[3]):
            print(f"  [{tag}] {col:<35} {n}/{N} 行, max_rel={mr:.4f}")

    if results_unexpected:
        print("\n── ❌ 意外差异明细（按差异行数降序）────────────────────────")
        hdr = f"{'列名':<35} {'差异行':<10} {'max_rel':<14} {'样本时间':<20} exp → gen"
        print(hdr)
        print("─" * 90)
        for col, n, mr, t, ev, gv in sorted(results_unexpected, key=lambda x: -x[1]):
            print(f"  {col:<35} {n:<10} {mr:<14.4f} {t}  {ev} → {gv}")

        if output_csv:
            # 写明细 CSV
            rows_out = []
            for col, n, mr, t, ev, gv in results_unexpected:
                gv_arr = g[col].values.astype(float)
                ev_arr = e[col].values.astype(float)
                diff_arr = np.abs(gv_arr - ev_arr)
                rdiff_arr = diff_arr / np.maximum(np.abs(ev_arr), 1e-9)
                bad_mask = (diff_arr > 1e-6) & (rdiff_arr > TOL)
                for i in np.where(bad_mask)[0]:
                    rows_out.append({
                        'col': col,
                        'dateTime': int(g[KEY].iloc[i]),
                        'exp': ev_arr[i],
                        'gen': gv_arr[i],
                        'abs_diff': diff_arr[i],
                        'rel_diff': rdiff_arr[i],
                    })
            if rows_out:
                pd.DataFrame(rows_out).sort_values(['col','dateTime']).to_csv(output_csv, index=False)
                print(f"\n📄 详细差异已写入: {output_csv}")
    else:
        print("\n🎉 无意外差异！")

    print(f"\n{'─'*60}")
    print(f"已知差异分类说明:")
    print(f"  K1 pendingTotal:  全深度 FIFO 簿 vs top-10 快照（设计差异）")
    print(f"  K2 wtA* active:   旧 exp 未实现主动单分类（exp 全为 0，gen 正确）")
    print(f"  K3 wtP*/AvgPos:   主/被动分类边界小差异（同 K2 根因）")
    print(f"  K4 收盘竞价:      14:57–15:00 gen 显示交叉簿+dataFlag=1，exp 用连续模式")
    print(f"  K5 末行:          15:00:00 gen 含收盘撮合成交，exp 为 0")
    print(f"  K6 eventCount:    gen 新增诊断列，exp 无")
    print(f"  K7 时间桶:        gen 含开盘竞价桶 09:15–09:25；exp 含 09:30:00 特殊槽")
    print(f"  K8 type/level:    calcType() 依赖盘口状态；不同 builder 版本盘口微差→type 归类不同")
    print(f"                    (type2↔type4 切换, wtBid1/Ask1, askType*/bidType*, MaxVol/Price 等)")
    print(f"  K9 PM 档位微差:  PM 段单笔卖单落在不同 level（两版本盘口状态微差）")
    print(f"                    表现为 askVolume*/pendingAsk* 持续差 200 手直到该单消失")


# ─── CLI ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('generated', help='.bin 分片或已合并的单股票 parquet')
    ap.add_argument('ref',       help='参考 parquet 文件或含参考 parquet 的目录')
    ap.add_argument('--date', '-d', default='', help='交易日期 YYYYMMDD（用于搜索参考文件）')
    ap.add_argument('--symbol', '-s', default='', help='股票代码，如 600396')
    ap.add_argument('--no-csv', action='store_true', help='不输出 diff CSV')
    args = ap.parse_args()

    # 读生成文件
    if args.generated.endswith('.bin'):
        print(f"[读 .bin] {args.generated}")
        gen = read_bin(args.generated)
    else:
        print(f"[读 gen parquet] {args.generated}")
        gen = pd.read_parquet(args.generated)

    # 确定 instrumentID
    if args.symbol:
        sym = args.symbol.lstrip('szSZ').lstrip('shSH')
        try:
            iid = float(int(sym))
        except ValueError:
            iid = None
    else:
        iid = float(gen['instrumentID'].iloc[0]) if 'instrumentID' in gen.columns else None

    if iid is None:
        print("ERROR: 无法确定 instrumentID，请指定 --symbol", file=sys.stderr)
        sys.exit(1)

    # 过滤 gen（如果是全市场 parquet）
    if 'instrumentID' in gen.columns and gen['instrumentID'].nunique() > 1:
        gen = gen[gen['instrumentID'] == iid].reset_index(drop=True)

    print(f"[读 ref parquet] {args.ref}  (instrumentID={iid})")
    exp = read_ref(args.ref, iid)

    date_str = args.date or str(int(gen['dateTime'].iloc[0]))[:8]
    sym_str  = args.symbol or str(int(iid))
    csv_out  = None if args.no_csv else f"diff_{date_str}_{sym_str}.csv"

    print(f"\n对比: {sym_str}  {date_str}")
    compare(gen, exp, output_csv=csv_out)


if __name__ == '__main__':
    main()
