#!/usr/bin/env bash
# 半年批量回测·单日: vgen(策略→订单) → vloop(trust-cache, 平仓量=实际成交) → pnl(逐对明细落盘)
# 用法: h1day.sh YYYYMMDD   (幂等: 已有 res_h1_$d_pnl.csv 则跳过)
set -u
d=$1
# 切到 l3_factor 项目根(本脚本在 bt/ 下), 下面的相对路径都以此为基准。
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=bt/h1bt
[ -f "$OUT/res_h1_$d""_pnl.csv" ] && { echo "[$d] ⏭"; exit 0; }
[ -f "data/panels/panel_id_$d.csv" ] || { echo "[$d] ❌ 无面板"; exit 1; }
python backtest/vgen.py --strategy backtest/strategies/eat_age_rev.py --panel data/panels/panel_id_$d.csv \
    --outdir $OUT --tag h1 --set hold=8 --set entry=improve >/dev/null 2>&1 \
    || { echo "[$d] ❌ vgen"; exit 1; }
mv $OUT/orders_h1_$d.csv $OUT/orders_h1in_$d.csv; mv $OUT/meta_h1_$d.csv $OUT/meta_h1in_$d.csv
python backtest/vloop.py --orders $OUT/orders_h1in_$d.csv --meta $OUT/meta_h1in_$d.csv \
    --tl-dir data/tmp/$d --date $d --cache-dir data/cache --outdir $OUT --tag h1_$d \
    --trust-cache >/dev/null 2>&1 || { echo "[$d] ❌ vloop"; exit 1; }
python backtest/pnl.py --res $OUT/res_h1_$d.csv --meta $OUT/meta_h1in_$d.csv >/dev/null 2>&1 \
    || { echo "[$d] ❌ pnl"; exit 1; }
rm -f $OUT/orders_h1_${d}_it*.csv $OUT/fills_h1_$d.csv $OUT/orders_h1in_$d.csv
echo "[$d] ✅"
