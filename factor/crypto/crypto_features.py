#!/usr/bin/env python3
"""
Crypto 分钟因子计算，输出格式对齐 LOB features。

数据源: /home/yhzhou/massive_data/FLATFILE/Crypto_MIN/{year}/{mm}/{date}.csv
输出:   /home/yhzhou/factor/crypto_features/{date}/{coin}.csv
        每文件行=分钟时间点，列=因子，和 LOB features 格式相同

用法:
    python3 crypto/crypto_features.py                  # 全量（2023-2026）
    python3 crypto/crypto_features.py 2024-01          # 某月
    python3 crypto/crypto_features.py 2024-01 2024-03  # 月份区间
"""

import os, sys, glob
import numpy as np
import pandas as pd
from pathlib import Path
from qr import paths

# ── 配置 ────────────────────────────────────────────────────────────────────
DATA_ROOT = str(paths.get('crypto_data'))
OUT_ROOT  = str(paths.get('crypto_feat'))

# 只处理这些主流币（对应文件里的 ticker）
COINS = [
    'X:BTC-USD', 'X:ETH-USD', 'X:SOL-USD', 'X:BNB-USD',
    'X:XRP-USD', 'X:ADA-USD', 'X:DOGE-USD', 'X:LINK-USD',
    'X:LTC-USD', 'X:AVAX-USD',
]
# 短名，用作文件名
COIN_NAME = {c: c.replace('X:','').replace('-USD','').lower() for c in COINS}

# 因子计算需要的历史回看窗口（分钟）
LOOKBACK = 1440  # 1天，用于 vol_ratio_1d 等

# ── 数据加载 ─────────────────────────────────────────────────────────────────
def list_files(start_ym: str = None, end_ym: str = None):
    """返回按时间排序的所有 CSV 路径列表，可按年月过滤。"""
    files = sorted(glob.glob(f'{DATA_ROOT}/*/*/*.csv'))
    if start_ym:
        files = [f for f in files if os.path.basename(f)[:7] >= start_ym]
    if end_ym:
        files = [f for f in files if os.path.basename(f)[:7] <= end_ym]
    return files

def load_file(path: str) -> pd.DataFrame:
    df = pd.read_csv(path, usecols=['ticker','window_start','open','high','low','close','volume','transactions'],
                     dtype={'ticker': str})
    df = df[df['ticker'].isin(COINS)]
    df['ts'] = pd.to_datetime(df['window_start'], unit='ns', utc=True)
    df = df.drop(columns='window_start')
    # flat file 无 vwap，用 (open+close)/2 近似
    df['vwap'] = (df['open'] + df['close']) / 2
    return df

# ── 因子计算 ─────────────────────────────────────────────────────────────────
def compute_factors(df: pd.DataFrame) -> pd.DataFrame:
    """
    输入: 单币的分钟 OHLCV，按 ts 升序，可含多天。
    输出: 同 index，列为各因子。
    """
    c = df['close']
    h = df['high']
    l = df['low']
    v = df['volume']
    tr = df['transactions']
    vwap = df['vwap']

    ret1 = c.pct_change(1)

    feat = pd.DataFrame(index=df.index)
    feat['ts'] = df['ts']

    # ── 收益率动量 ──
    feat['ret_1m']  = ret1
    feat['ret_5m']  = c.pct_change(5)
    feat['ret_15m'] = c.pct_change(15)
    feat['ret_1h']  = c.pct_change(60)
    feat['ret_4h']  = c.pct_change(240)
    feat['ret_1d']  = c.pct_change(1440)

    # ── 已实现波动率 ──
    feat['rv_1h']  = ret1.rolling(60).std()  * np.sqrt(60)
    feat['rv_4h']  = ret1.rolling(240).std() * np.sqrt(240)
    feat['rv_1d']  = ret1.rolling(1440).std()* np.sqrt(1440)

    # ── 价格区间（Parkinson proxy）──
    log_hl = np.log(h / l.replace(0, np.nan))
    feat['range_pct'] = (h - l) / c.replace(0, np.nan)
    feat['parkinson'] = log_hl ** 2 / (4 * np.log(2))

    # ── 成交量 ──
    vol_ma60   = v.rolling(60, min_periods=1).mean()
    vol_ma1440 = v.rolling(1440, min_periods=1).mean()
    feat['vol_ratio_1h'] = v / vol_ma60.replace(0, np.nan)
    feat['vol_ratio_1d'] = v / vol_ma1440.replace(0, np.nan)
    feat['avg_trade_sz'] = v / tr.replace(0, np.nan)  # 平均单笔规模

    # ── VWAP 偏离 ──
    feat['vwap_dev'] = (c - vwap) / vwap.replace(0, np.nan)

    # ── 价格位置（过去4h高低点区间）──
    lo240 = l.rolling(240, min_periods=1).min()
    hi240 = h.rolling(240, min_periods=1).max()
    rng   = (hi240 - lo240).replace(0, np.nan)
    feat['price_pos_4h'] = (c - lo240) / rng

    # ── 成交量加速度（近1h vs 近4h的量比）──
    vol_1h  = v.rolling(60, min_periods=1).sum()
    vol_4h  = v.rolling(240, min_periods=1).sum()
    feat['vol_accel'] = (vol_1h / 60) / (vol_4h / 240).replace(0, np.nan)

    # ── Amihud 非流动性 ──
    feat['amihud'] = ret1.abs() / v.replace(0, np.nan)

    return feat

# ── 主流程 ──────────────────────────────────────────────────────────────────
def process(start_ym=None, end_ym=None):
    files = list_files(start_ym, end_ym)
    print(f"共 {len(files)} 个文件，处理币种: {list(COIN_NAME.values())}")

    # 每次加载 [i-1, i] 两个文件，保证 lookback 连续
    # 按月分批加载，减少内存占用

    # 收集所有数据（分币种）
    coin_buf = {coin: [] for coin in COINS}

    for i, path in enumerate(files):
        date_str = os.path.basename(path)[:10]  # 2023-01-01
        print(f"[{i+1}/{len(files)}] {date_str}", end='\r')

        df = load_file(path)
        for coin in COINS:
            sub = df[df['ticker'] == coin].copy()
            if not sub.empty:
                coin_buf[coin].append(sub)

        # 每处理完一天，输出这天的因子（需要前一天数据做 lookback）
        # 实际：收集完所有数据再统一处理，内存允许的话更简洁
        # 总数据量估算：10币 × 1440min × 1270天 ≈ 1800万行，约 1.5GB，可接受

    print(f"\n加载完成，开始计算因子...")

    for coin in COINS:
        name = COIN_NAME[coin]
        if not coin_buf[coin]:
            print(f"  {name}: 无数据，跳过")
            continue

        full = pd.concat(coin_buf[coin]).sort_values('ts').reset_index(drop=True)
        print(f"  {name}: {len(full):,} 行 ({full['ts'].iloc[0].date()} → {full['ts'].iloc[-1].date()})")

        feat = compute_factors(full)

        # 按日期拆分输出
        feat['date'] = feat['ts'].dt.strftime('%Y-%m-%d')
        # 只输出有完整 lookback 的日期（跳过第一天）
        dates = feat['date'].unique()
        for date in dates[1:]:  # 跳第一天（没有前日 lookback）
            day = feat[feat['date'] == date].copy()
            day = day.drop(columns=['date'])
            day = day.set_index('ts')
            day.index = day.index.strftime('%H:%M')
            day.index.name = 'time'

            out_dir = os.path.join(OUT_ROOT, date.replace('-',''))
            os.makedirs(out_dir, exist_ok=True)
            out_path = os.path.join(out_dir, f'{name}.csv')
            day.to_csv(out_path, float_format='%.6f')

    print(f"\n完成 → {OUT_ROOT}")

if __name__ == '__main__':
    start = sys.argv[1] if len(sys.argv) > 1 else None
    end   = sys.argv[2] if len(sys.argv) > 2 else None
    process(start, end)
