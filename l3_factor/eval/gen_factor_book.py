#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
因子手册生成器: 从评估产物自动生成 FACTOR_BOOK.md(每因子一张卡: 长期证据/衰减/尾部)。
数据源: bt/daily_ic_2026h1_13f.csv(124天) bt/daily_ic_uniq.csv(5天)
        bt/ic_horizon_13f.out bt/ic_horizon_uniq.out(期限结构文本表)
用法: python eval/gen_factor_book.py > FACTOR_BOOK.md
"""
import re
import numpy as np, pandas as pd
from qr.paths import BT

HORIZONS = [1, 2, 4, 8, 15, 30, 60]

DEF = {  # 因子: (中文名, 定义, 直觉)
 "eat_age_imb": ("被吃单年龄失衡", "买方所吃卖单 vs 卖方所吃买单的量加权log挂龄差; 挂龄=|buyId−sellId|(事件时钟)", "谁在啃穿耐心资金的老单, 谁就是强弩之末 → 反转"),
 "old_eat_imb": ("老单被吃失衡", "只统计挂龄≥当日中位的老单被吃量的买卖差", "eat_age_imb 的锐化版, 剔除新单互打噪音"),
 "trd_imb": ("主动量失衡", "买方主动成交量−卖方主动成交量(大号=主动方), 无年龄加权", "经典成交流失衡; 无年龄基线对照组"),
 "eat_age_med": ("被吃单年龄中位", "本桶全部被吃单的log挂龄中位数(无向)", "市场在深挖老簿(慢钱换手)还是新单互打"),
 "cxl_life_med": ("撤单寿命中位", "被撤单从挂出到被撤经历的市场事件数(log中位)", "撤老单=耐心资金撤退; 撤新单=高频调仓"),
 "flash_cxl_ratio": ("闪撤强度", "寿命≤当日q10的闪撤量占撤单量比", "spoofing/探价活动强度"),
 "flash_cxl_imb": ("闪撤方向失衡", "闪撤量的买卖方向差(+=买侧在虚张声势)", "哪边在假装有需求"),
 "mean_size": ("平均单笔委托量", "桶内新到委托的平均股数", "大单=知情资金"),
 "n_ord": ("委托到达强度", "桶内新到委托笔数", "关注度/活跃度; 热闹→随后跑输(半年翻案)"),
 "ord_imb_cnt": ("委托笔数失衡", "(买笔−卖笔)/(总笔)", "挂单意愿的数量差"),
 "ord_imb_vol": ("委托量失衡", "(买量−卖量)/(总量)", "挂单压力的规模差"),
 "cxl_ratio": ("撤单率", "撤单笔数/挂单笔数", "撤单活动强度"),
 "large_share": ("大单占比", "≥当日p90的大委托量占比(⚠全日分位前视)", "大单参与度"),
 "kept_rem_imb": ("残量保留失衡", "主动单扫完后残量继续挂着(vs撤掉)的量, 按方向做差", "扫完还挂=没吃饱的存量需求"),
 "chain_imb": ("拆单链方向失衡", "订单号近连续+同向+相近size的委托链, 链上量的方向差", "机构母单正在哪个方向执行"),
 "chain_share": ("拆单链活跃度", "链上量占全部委托量比(无向)", "算法执行活动总强度"),
 "agg_hhi": ("主动方集中度", "主动方订单ID的赫芬达尔指数(买卖均值)", "一条鲸鱼在扫 vs 一群散户蜂拥"),
 "agg_hhi_imb": ("集中度买卖差", "买方主动HHI − 卖方主动HHI", "哪边是鲸鱼哪边是羊群"),
 "book_age_mean": ("簿龄均值", "当前存活挂单的量加权年龄(买卖均值)", "簿是老钱垒的还是高频闪的"),
 "book_age_imb": ("墙龄差", "买墙平均年龄 − 卖墙平均年龄", "哪边的支撑是真墙"),
 "chase_fwd_imb": ("追价失衡", "撤单后短窗内同向更激进重挂的量, 买卖差", "撤了往前凑=急迫升级"),
 "refill_imb": ("补墙失衡", "被对手吃掉的价位同桶被己方新委托填回的比例差", "哪边在拼命防守"),
}
VERDICT = {
 "eat_age_imb": "**主力快枪**: 半年124/124天同号、t≈−50, A股1分钟结构性规律。独立死刑(尾部8~11bp<门票25bp), 执行择时核心。",
 "old_eat_imb": "与主力高度重叠, 备胎。",
 "trd_imb": "被 eat_age_imb 完全包含(控制后偏IC t−0.06), 仅作基线。",
 "eat_age_med": "广而浅慢信号(IC增长但尾部仅3.6bp@30min)→ 不能独立选股; 合成/调度原料。曾被4天样本误杀, 半年翻案。",
 "cxl_life_med": "广而浅, 同上更弱; 原料。",
 "flash_cxl_ratio": "半年翻案的慢信号(单日误判无信号); 广而浅。",
 "flash_cxl_imb": "**陷阱盖棺**: next t28.9 vs skip同号51%(抛硬币)。skip检验教科书案例。周数据h=8-15有异动, 仅留观。",
 "mean_size": "弱但真(半年90%同号); 跨版本锚点。",
 "n_ord": "**半年翻案**: 单日看是零, 半年skip t−17/96%同号且衰减曲线走深(−.036@30)。热闹→跑输的慢规律; 广而浅。",
 "ord_imb_cnt": "快衰减+长h形态怪, 归档。",
 "ord_imb_vol": "反转陷阱(next+.023 → skip变号−.005), 归档。",
 "cxl_ratio": "弱, 归档。",
 "large_share": "**陷阱正式下葬**: 单日t7.5→skip t0.7 的教科书案例, 半年确认。",
 "kept_rem_imb": "**全项目最强即时信号**(next IC .088), 1分钟内死透 → 执行择时主武器。",
 "chain_imb": "首个动量信号: 即时+.033 且 30-60min 迟到续力 → 执行择时二号位; 母单持续冲击的形态证据。",
 "chain_share": "广而浅(尾部1.3bp@30), 母单活动=随后跑输; 原料。",
 "agg_hhi": "广而浅的典型标本: IC增长到.038@30, 尾部仅2.6bp。原料。",
 "agg_hhi_imb": "量级小但极稳(skip t11.8); 合成原料。",
 "book_age_mean": "弱慢, 原料。",
 "book_age_imb": "弱, 归档。",
 "chase_fwd_imb": "弱+变号形态, 归档。",
 "refill_imb": "双面体(即时+.020延续 → 隔分钟−.015反转), 全期限平坦负; 结构有趣, 留观。",
}
TAIL = {  # 极端3%多空纸面alpha(bp), h: 8/15/30/60, 2026-07-16 实测(0608-0612)
 "eat_age_med": {8: 0.1, 15: 1.1, 30: 3.6, 60: 4.5},
 "cxl_life_med": {8: -0.3, 15: -0.1, 30: 1.0, 60: 2.2},
 "agg_hhi": {8: -0.2, 15: 0.8, 30: 2.6, 60: 0.2},
 "chain_share": {8: 0.1, 15: 0.3, 30: 1.3, 60: 2.5},
}
GROUPS = [
 ("一、主力与快枪(执行择时弹药)", ["eat_age_imb", "kept_rem_imb", "chain_imb", "old_eat_imb", "trd_imb"]),
 ("二、慢信号原料(广而浅: IC增长但尾部薄)", ["eat_age_med", "agg_hhi", "n_ord", "flash_cxl_ratio", "cxl_life_med", "chain_share", "agg_hhi_imb", "book_age_mean", "mean_size"]),
 ("三、陷阱标本(skip检验的战利品)", ["flash_cxl_imb", "large_share", "ord_imb_vol"]),
 ("四、归档与留观", ["refill_imb", "ord_imb_cnt", "cxl_ratio", "book_age_imb", "chase_fwd_imb"]),
]


def load_daily(path, ndays):
    d = pd.read_csv(path)
    out = {}
    for (f, lab), g in d.groupby(["feature", "label"]):
        v = g.ic.dropna().values
        if len(v) < 2:
            continue
        m, s = v.mean(), v.std(ddof=1)
        out[(f, lab)] = dict(ic=m, ir=m/s if s else np.nan, t=m/s*np.sqrt(len(v)) if s else np.nan,
                             sign=(np.sign(v) == np.sign(m)).mean()*100, n=len(v))
    return out


def parse_horizon(path):
    out = {}
    pat = re.compile(r"([+-]?\d+\.\d{4})\[\s*([+-]?\d+\.?\d*|-)\s*\]")
    for ln in open(path):
        toks = ln.split()
        if not toks or toks[0] in ("feature", "=====", "读法:") or ln.startswith(("=", "读", "⚠")):
            continue
        cells = pat.findall(ln)
        if len(cells) == len(HORIZONS):
            out[toks[0]] = [(float(ic), (float(t) if t not in ("-",) else np.nan)) for ic, t in cells]
    return out


daily = {}
daily.update({(f, l, "124天"): v for (f, l), v in load_daily(BT + "/daily_ic_2026h1_13f.csv", 124).items()})
daily.update({(f, l, "5天"): v for (f, l), v in load_daily(BT + "/daily_ic_uniq.csv", 5).items()})
hor = parse_horizon(BT + "/ic_horizon_13f.out")
hor.update(parse_horizon(BT + "/ic_horizon_uniq.out"))

print("# 因子手册(自动生成 · 数据截至 2026-07-17)\n")
print("> 生成: `python eval/gen_factor_book.py > FACTOR_BOOK.md`。口径速查:")
print("> **IC**=截面rank相关(日均); **IR**=mean(日IC)/std(日IC); **t**=IR×√天数; **同号%**=与全期均值同向的天数占比。")
print("> 长期证据: 13f 因子=124天, uniq 因子=5天(⚠自由度4)。衰减表=skip进场持有h分钟(5天, IR/t为日间)。")
print("> 尾部alpha=极端3%多空纸面收益(bp), 不含成本; 门票参考线: 独立往返 ~25bp。\n")

for gtitle, feats in GROUPS:
    print(f"\n## {gtitle}\n")
    for f in feats:
        cn, defi, intu = DEF[f]
        print(f"### `{f}` — {cn}\n")
        print(f"**定义**: {defi}  \n**直觉**: {intu}\n")
        # 长期证据
        rows = [(lab, per) for (ff, lab, per), v in [] ]
        print("| 长期证据 | IC | IR(日) | t | 同号% | 样本 |")
        print("|---|---|---|---|---|---|")
        for lab in ("ret_next", "ret_skip"):
            for per in ("124天", "5天"):
                v = daily.get((f, lab, per))
                if v:
                    print(f"| {lab}·{per} | {v['ic']:+.4f} | {v['ir']:+.2f} | {v['t']:+.1f} | {v['sign']:.0f}% | {v['n']} |")
                    break
        # 衰减
        if f in hor:
            h = hor[f]
            print("\n| 衰减(h分钟) | " + " | ".join(str(x) for x in HORIZONS) + " |")
            print("|---|" + "---|"*len(HORIZONS))
            print("| IC | " + " | ".join(f"{ic:+.4f}" for ic, _ in h) + " |")
            print("| IR(日) | " + " | ".join((f"{t/np.sqrt(5):+.2f}" if not np.isnan(t) else "-") for _, t in h) + " |")
            print("| t | " + " | ".join((f"{t:+.1f}" if not np.isnan(t) else "-") for _, t in h) + " |")
        if f in TAIL:
            ta = TAIL[f]
            print("\n| 尾部alpha(bp) | h=8 | h=15 | h=30 | h=60 |")
            print("|---|---|---|---|---|")
            print("| 极端3%多空 | " + " | ".join(f"{ta[k]:+.1f}" for k in (8, 15, 30, 60)) + " |")
        print(f"\n**裁决**: {VERDICT[f]}\n")
