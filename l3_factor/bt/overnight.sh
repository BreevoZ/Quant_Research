#!/usr/bin/env bash
# 过夜主链: 历史回填(样本外验证的前提) —— 幂等、磁盘守卫、分阶段
# 注: 不用 pgrep 做等待判据 —— pgrep -f 会匹配到创建本脚本的那条 shell 命令自身,
#     造成"自己等自己"永久阻塞(已踩过)。判据一律用产物文件。
set -u
cd /home/yhzhou/l3_factor
log(){ echo "[$(date '+%m-%d %H:%M')] $*"; }

log "开始回填"
log "阶段1: 30 天(留缓存, 供执行择时样本外回测)"
KEEP_CACHE=1 VSIM_WORKERS=60 xargs -P 3 -I{} python runday.py {} < bt/oos_cache_days.txt > bt/oos1.log 2>&1
log "阶段1 完成: $(grep -c '完成' bt/oos1.log)/30 天, 磁盘剩 $(df -h /home/yhzhou|tail -1|awk '{print $4}')"

log "阶段2: 90 天(仅面板, 因子样本外验证; 抽完即删缓存)"
KEEP_CACHE=0 VSIM_WORKERS=60 xargs -P 3 -I{} python runday.py {} < bt/oos_panel_days.txt > bt/oos2.log 2>&1
log "阶段2 完成: $(grep -c '完成' bt/oos2.log)/90 天"

log "阶段3: 22 因子样本外评估(2023-2025 净土年份)"
DAYS=$(cat bt/oos_panel_days.txt | tr '\n' ',' | sed 's/,$//')
python eval/eval_multiday.py --days "$DAYS" --dump bt/daily_ic_oos.csv > bt/eval_oos.out 2>&1
log "OVERNIGHT_ALL_DONE 面板总数 $(ls data/panels/*.csv | wc -l)"
