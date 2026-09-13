#!/usr/bin/env bash
# 过夜主链: ①等37天补跑+13f评估 → ②老因子期限结构 → ③uniq特征126天 → ④S1/S2/S3半年回测 → ⑤出图
set -u
# 切到 l3_factor 项目根(本脚本在 bt/ 下), 下面的相对路径都以此为基准。
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
echo "[chain] start $(date +%H:%M)"
# ① 等 13f 评估产物(bs383pxyo 链的终点)
n=0
until [ -s bt/eval_h1_13f.out ]; do sleep 120; n=$((n+1)); [ $n -gt 90 ] && { echo "[chain] ❌ 等13f超时"; exit 3; }; done
echo "[chain] ① 13f评估已出 $(date +%H:%M)"
# ② 老因子全量期限结构(一周)
python eval/ic_horizon.py --panels data/panels --days 20260608,20260609,20260610,20260611,20260612 \
  > bt/ic_horizon_13f.out 2>bt/ic_horizon_13f.err
echo "[chain] ② 期限结构完成 $(date +%H:%M)"
# ③ uniq 特征全量(幂等跳过已有)
ls data/cache | grep -E '^2026' | while read d; do
  [ -f data/panels_uniq/panel_id_$d.csv ] && continue; echo $d
done > bt/uniq_days.txt
xargs -P 3 -I{} sh -c '[ -f data/panels_uniq/panel_id_{}.csv ] || python factors/factor_uniq.py --cache-dir data/cache/{} --dump data/panels_uniq/panel_id_{}.csv --workers 16 2>&1 | tail -1 | sed "s/^/[{}] /"' < bt/uniq_days.txt > bt/uniq_batch.log 2>&1
echo "[chain] ③ uniq抽取完成 $(ls data/panels_uniq | wc -l)/126 $(date +%H:%M)"
# ④ 三策略半年回测
ls data/cache | grep -E '^2026' | xargs -P 3 -I{} bt/s123day.sh {} > bt/s123.log 2>&1
echo "[chain] ④ 回测完成: $(grep -c '✅' bt/s123.log) 天 $(date +%H:%M)"
# ⑤ 出图
for s in s1 s2 s3; do
  python - <<PY 2>/dev/null
import glob, re, pandas as pd
fs = glob.glob("bt/s123/res_${s}_*_pnl.csv")
print("${s}:", len(fs), "天")
PY
done
echo "[chain] ALL_DONE $(date +%H:%M)"
