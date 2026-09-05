#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
订单流声化(Sonification)—— 把一天的 L3 逐笔成交流变成一段可听的音频。
借鉴天文光变曲线声化: 五声音阶映射(怎么弹都不难听)+ 多层叠加(每层=市场一个维度)。

声学映射:
  · 成交主动方向(买-卖净额) → 音高(五声音阶, 买盘上行/卖盘下行)
  · 成交强度(暴发)          → 音符密度 + 响度
  · 单笔大单                 → 低音"鼓点"(闷响)
  · 主动买卖失衡             → 左右声道(买偏右/卖偏左, 立体声)
  · 成交价趋势               → 背景持续音的缓慢移调(嗡鸣底噪)

用法: python explore/sonify.py <TIC> [date] [--secs 30] [--out out.wav]
"""
import argparse, sys
import numpy as np
from scipy.io import wavfile
from qr import l3cache
from qr.paths import BT, DATA

SR = 44100                        # 采样率
AM = (9*3600+30*60, 11*3600+30*60)
PM = (13*3600, 14*3600+57*60)
# ── 音高 = 相对开盘的涨跌幅(跨股可比, 开盘价=固定锚点) ──
# 中心 A4=440Hz 对应涨跌幅 0; ±LIMIT(涨跌停)映射到人耳敏感区两端, 跨 3 八度。
A4 = 440.0
LIMIT = 0.10                      # ±10% 涨跌停 → 满量程
SPAN_OCT = 1.5                    # 中心上下各 1.5 八度 → 总跨 3 八度(220Hz~1760Hz)
# 五声音阶(相对半音): 大调/小调各一套, 用于把连续音高量化到"好听"的音上
_MAJ_ST = np.array([0, 2, 4, 7, 9])       # 大调五声(宫): C D E G A
_MIN_ST = np.array([0, 3, 5, 7, 10])      # 小调五声(羽): 含小三度/小七度(忧郁)


_CAND_MAJ = np.array([o*12 + s for o in range(-3, 4) for s in _MAJ_ST])   # 预计算候选半音
_CAND_MIN = np.array([o*12 + s for o in range(-3, 4) for s in _MIN_ST])


def ret_to_freq(ret, major):
    """涨跌幅 → 音高(Hz), 量化到五声音阶。ret 裁剪到 ±LIMIT, 映射到中心±SPAN_OCT 八度。"""
    r = min(max(ret / LIMIT, -1.0), 1.0)
    semis = r * SPAN_OCT * 12                        # 目标半音偏移(相对 A4)
    cand = _CAND_MAJ if major else _CAND_MIN
    nearest = cand[np.argmin(np.abs(cand - semis))]
    return A4 * 2**(nearest/12)


def adsr(n, a=0.008, d=0.04, s=0.6, r=0.08):
    """音符包络(避免爆音)。短音符时按比例压缩各段, 保证 a+d+r ≤ n。"""
    na, nd, nr = int(a*SR), int(d*SR), int(r*SR)
    if na+nd+nr >= n:                                # 太短 → 各段等比缩到占满
        scale = max(n-1, 1) / (na+nd+nr+1)
        na, nd, nr = int(na*scale), int(nd*scale), int(nr*scale)
    ns = n - na - nd - nr
    return np.concatenate([
        np.linspace(0, 1, na) if na else np.array([]),
        np.linspace(1, s, nd) if nd else np.array([]),
        np.full(max(ns, 0), s),
        np.linspace(s, 0, nr) if nr else np.array([]),
    ])[:n]


def note(freq, dur, amp):
    n = int(dur*SR)
    t = np.arange(n)/SR
    # 基频 + 少量泛音(更"乐器"), 加 adsr
    w = np.sin(2*np.pi*freq*t) + 0.3*np.sin(2*np.pi*2*freq*t) + 0.15*np.sin(2*np.pi*3*freq*t)
    return w * adsr(n) * amp


def kick(dur, amp):
    """低音鼓(大单): 频率下扫 + 慢衰减 + 下坠尾音, 更有分量"""
    n = int(dur*SR); t = np.arange(n)/SR
    f = 90*np.exp(-18*t) + 35                         # 下扫到 35Hz 的低频, 更"砸"
    body = np.sin(2*np.pi*f*t) * np.exp(-6*t)
    click = np.sin(2*np.pi*180*t) * np.exp(-80*t)*0.4  # 敲击感, 帮助穿透
    # 下坠尾音: 一个继续下滑的低频拖尾(余韵/分量感)
    ft = 60*np.exp(-4*t) + 30
    tail = np.sin(2*np.pi*ft*t) * np.exp(-4*t) * 0.35
    return (body + click + tail) * amp


def reverb(x, sr, decay=0.28, mix=0.22):
    """轻混响(几个衰减抽头叠加, 制造厅堂空间感)。x: mono 或 stereo。"""
    delays_ms = [37, 53, 71, 97, 113]
    out = x.copy().astype(float)
    for j, dm in enumerate(delays_ms):
        d = int(dm/1000*sr)
        g = decay * (0.7**j)
        if d < len(x):
            out[d:] += g * x[:-d]
    return (1-mix)*x + mix*out


def load_trades(tic, date):
    a = l3cache.load(f"{DATA}/cache/{date}/tl_sz_{tic}.bin")
    sec = a["timeSeconds"]; at = a["actionType"]
    m = (at == 2) & (a["buyId"] > 0) & (a["sellId"] > 0) & \
        (((sec >= AM[0]) & (sec < AM[1])) | ((sec >= PM[0]) & (sec < PM[1])))
    t = sec[m]; vol = a["volume"][m].astype(float); px = a["price"][m]
    baggr = a["buyId"][m] > a["sellId"][m]     # 买方主动
    return t, vol, px, baggr


def sonify(tic, date, secs, out):
    t, vol, px, baggr = load_trades(tic, date)
    n = len(t)
    print(f"{tic} {date}: {n:,} 笔成交 → {secs}s 音频", file=sys.stderr)
    # 时间压缩: 连续段拼接, 映射到 [0, secs)
    t2 = t - t.min()
    # 把午休 gap 挤掉(平移 PM)
    am_end = t2[t < PM[0]].max() if (t < PM[0]).any() else 0
    pm_start = t2[t >= PM[0]].min() if (t >= PM[0]).any() else am_end
    t2 = np.where(t >= PM[0], t2 - (pm_start - am_end) - 0.001, t2)
    audio_t = t2 / t2.max() * secs

    L = np.zeros(int(secs*SR)+SR)             # 左右声道
    R = np.zeros(int(secs*SR)+SR)

    # ★ 保留节奏: 音符落在【真实成交时刻】上, 密就是密、疏就是疏(不再固定网格)。
    #   为免几万音符糊成一团, 按【等量】抽稀: 累计成交量每达到一个配额发一个音符
    #   → 暴发(量大)时短时间内多个音符=急促, 平静时稀疏。
    pxmin, pxmax = px.min(), px.max()
    px_open = px[0]                                  # 开盘价(判涨跌 → 大/小调)
    v995, v95 = np.percentile(vol, 99.5), np.percentile(vol, 95)   # 真·大单才砸鼓
    # ★ 声像 = 滚动净买卖压力(缓慢移动, 承载"资金往哪边走"): 对量加权方向做指数平滑
    signed = np.where(baggr, vol, -vol)
    ema = np.zeros(n); a_ema = 0.02                  # 慢平滑 → 声像缓移而非抖动
    acc_e = 0.0
    for i in range(n):
        acc_e = (1-a_ema)*acc_e + a_ema*np.sign(signed[i])*np.log1p(abs(signed[i]))
        ema[i] = acc_e
    pan_series = 0.5 + 0.48*np.tanh(ema / (np.abs(ema).std()+1e-9))  # [0.02,0.98] 分离更明显
    quota = vol.sum() / (secs * 5)                  # 目标 ~5 音符/秒(耳朵能分开, 留白明显)
    acc = 0.0
    for i in range(n):
        acc += vol[i]
        big = vol[i] > v995                         # 只有极大单额外强调(不再豁免抽稀→密集股不会全是鼓)
        if acc < quota:
            continue
        acc = 0.0
        at = audio_t[i]
        pos = int(at * SR)
        if pos >= len(L)-1:
            continue
        # 局部密度 → 音符时长(近端成交越密, 音越短促)
        lo = np.searchsorted(audio_t, at-0.3); hi = np.searchsorted(audio_t, at+0.001)
        local_rate = (hi-lo) / 0.3                    # 事件/秒(音频时间)
        dur = float(np.clip(0.30 - local_rate*0.004, 0.05, 0.30))
        # ★ 音高 = 相对开盘涨跌幅(跨股可比, 开盘=中心音 A4); 涨→大调 跌→小调
        ret_now = (px[i] - px_open) / px_open
        freq = ret_to_freq(ret_now, major=(ret_now >= 0))
        dir_i = 1 if baggr[i] else -1                 # 瞬时主动方向仍控声道
        amp = float(np.clip(0.08 + 0.18*np.log1p(vol[i])/6, 0.06, 0.32))
        nt = note(freq, dur, amp)
        pan = float(pan_series[i])                     # 声像 = 滚动净买卖压力(资金流向)
        m2 = min(len(nt), len(L)-pos)
        L[pos:pos+m2] += nt[:m2]*(1-pan); R[pos:pos+m2] += nt[:m2]*pan
        # 大单鼓点: 仅 p99.5 起(真·大单), 越大越响越长
        if big:
            scale = np.clip((vol[i]-v995)/(v995+1e-9), 0.5, 2.5)
            k = kick(0.3+0.1*min(scale,2), 0.5+0.3*min(scale,2))
            mk = min(len(k), len(L)-pos)
            L[pos:pos+mk] += k[:mk]; R[pos:pos+mk] += k[:mk]

    # (去掉背景嗡鸣: 70-105Hz 移调人耳听不出价格、还糊掉鼓点+填满留白。
    #  价格趋势改由【大调/小调 + 音区高低】表达, 辨识度更高。)

    # 轻混响(厅堂空间感) → 动态压缩 → 归一化
    L = reverb(L, SR); R = reverb(R, SR)
    stereo = np.stack([L, R], axis=1)
    stereo = stereo / (np.abs(stereo).max()+1e-9)          # 先归一到 [-1,1]
    stereo = np.tanh(stereo * 1.8)                          # 软压缩(温和, 保留留白不提亮静默)
    stereo = stereo / (np.abs(stereo).max()+1e-9) * 0.97
    wavfile.write(out, SR, (stereo*32767).astype(np.int16))
    print(f"[音频] {len(L)/SR:.1f}s stereo → {out}", file=sys.stderr)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("tic"); ap.add_argument("date", nargs="?", default="20260713")
    ap.add_argument("--secs", type=int, default=30)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or f"{BT}/sonify_{a.tic}_{a.date}.wav"
    sonify(a.tic, a.date, a.secs, out)
