#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 bt/fft_rhythm.npz 出正确的 FFT 节律图, 标注所有过 95% 置换阈值的峰。"""
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy.signal import find_peaks
from qr.paths import BT

d = np.load(BT + "/fft_rhythm.npz")
per, pw, prof = d["per"], d["pw"], d["prof"]
thr = float(d["thr"])
o = np.argsort(per); per, pw = per[o], pw[o]

# 过阈值的峰; prominence 去掉相邻 bin 重复标注
pk, _ = find_peaks(pw, height=thr, prominence=thr * 0.3)
pk = sorted(pk, key=lambda i: per[i])

def lab(p):
    if p < 60:   return f"{p:.0f}s"
    if p < 600:  return f"{p/60:.1f}min".replace(".0min", "min")
    return f"{p/60:.0f}min"

print(f"thr(95%置换) = {thr:.3e}")
print(f"共 {len(pk)} 个显著峰(按功率排序):")
for i in sorted(pk, key=lambda i: -pw[i]):
    print(f"  {lab(per[i]):8s}  {pw[i]:.3e}   {pw[i]/thr:5.1f}× 阈值")

SUR="#fcfcfb"; INK="#0b0b0b"; BLUE="#2a78d6"; RED="#e34948"; GRID="#e8e7e3"
plt.rcParams.update({"font.family":["DejaVu Sans","AR PL UMing CN"],"axes.unicode_minus":False,
 "figure.facecolor":SUR,"axes.facecolor":SUR,"text.color":INK,"axes.edgecolor":GRID,
 "axes.grid":True,"grid.color":GRID,"xtick.color":INK,"ytick.color":INK,"axes.labelcolor":INK,
 "axes.spines.top":False,"axes.spines.right":False})
fig,(ax1,ax2)=plt.subplots(2,1,figsize=(12,8.4),dpi=110)
fig.suptitle("A 股日内成交节律 — FFT 周期图(20 天全市场叠加)",fontsize=13,x=0.08,ha="left",color=INK)

t=np.arange(len(prof))/60
ax1.plot(t,prof,color=BLUE,lw=1.0); ax1.axvline(120,color=GRID,lw=1.5)
ax1.set_ylim(0,6000)   # 压掉开盘尖峰, 看清 U 型主体(开盘瞬时~5万被截断)
ax1.set_title("① 日内平均成交强度(笔/秒, 已挤掉午休)—— 这条 U 型就是被减掉的『基线』",loc="left",fontsize=10.5)
ax1.set_xlabel("会话分钟"); ax1.set_ylabel("笔/秒 (顶部已截断)")

ax2.loglog(per,pw,color=BLUE,lw=0.9)
ax2.axhline(thr,color=RED,ls="--",lw=1.2,label="置换检验 95% 阈值")
for k,i in enumerate(pk):
    p=per[i]
    ax2.plot(p,pw[i],'o',color=RED,ms=3.5)
    big = pw[i]/thr >= 3          # 主节拍(高亮), 其余小字
    ax2.annotate(lab(p),(p,pw[i]),color=RED,ha="center",va="bottom",
                 fontsize=(9 if big else 7),fontweight=("bold" if big else "normal"),
                 xytext=(0,5+(k%2)*9),textcoords="offset points")
ax2.set_title(f"② 去 U 型基线后残差的 FFT —— 全部 {len(pk)} 个过阈值峰(峰=算法时钟节拍)",loc="left",fontsize=10.5)
ax2.set_xlabel("周期(秒, 对数轴)"); ax2.set_ylabel("功率(对数)")
ax2.legend(frameon=False,fontsize=9,loc="lower left")
fig.tight_layout(rect=[0,0,1,0.96]); fig.savefig(BT + "/rhythm.png",facecolor=SUR)
print("[图] bt/rhythm.png 已生成")
