#!/usr/bin/env bash
# 主链重启版: 前提(13f评估)已就绪, 直接 ②期限结构 → ③uniq全量 → ④三策略回测。逐步带时间戳。
set -u
cd /home/yhzhou/l3_factor
echo "[chain2] start $(date '+%m-%d %H:%M')"
python eval/ic_horizon.py --panels data/panels --days 20260608,20260609,20260610,20260611,20260612 \
  > bt/ic_horizon_13f.out 2>bt/ic_horizon_13f.err
echo "[chain2] ② 13因子期限结构完成 $(date +%H:%M)"
ls data/cache | grep -E '^2026' | while read d; do
  [ -f data/panels_uniq/panel_id_$d.csv ] || echo $d
done > bt/uniq_days.txt
echo "[chain2] ③ uniq 待抽 $(wc -l < bt/uniq_days.txt) 天"
xargs -P 4 -I{} sh -c 'python factors/factor_uniq.py --cache-dir data/cache/{} --dump data/panels_uniq/panel_id_{}.csv --workers 14 2>&1 | tail -1 | sed "s|^|[{}] |"' < bt/uniq_days.txt > bt/uniq_batch.log 2>&1
echo "[chain2] ③ uniq完成 $(ls data/panels_uniq | wc -l)/126 $(date +%H:%M)"
ls data/cache | grep -E '^2026' | xargs -P 3 -I{} bt/s123day.sh {} > bt/s123.log 2>&1
echo "[chain2] ④ 回测完成 ✅$(grep -c '✅' bt/s123.log) ❌$(grep -c '❌' bt/s123.log) $(date +%H:%M)"
echo "[chain2] ALL_DONE $(date '+%m-%d %H:%M')"
