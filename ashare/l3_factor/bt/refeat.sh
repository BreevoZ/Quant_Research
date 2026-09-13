#!/usr/bin/env bash
# 单日特征重算(13列统一面板, 原子落盘)。已是新版面板(含 n_ord 列)则跳过 → 可中断重启。
d=$1
P=data/panels/panel_id_$d.csv
if [ -f "$P" ] && head -1 "$P" | grep -q n_ord; then echo "[$d] ⏭ 已是13特征面板"; exit 0; fi
python factors/factor_id.py --cache-dir data/cache/$d --dump "$P" --no-ic --workers 12 2>&1 | tail -1 | sed "s/^/[$d] /"
