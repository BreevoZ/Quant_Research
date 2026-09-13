#!/usr/bin/env bash
# 组合策略单日回测: S1(组合信号) S2(老信号+执行包) S3(组合信号+执行包)
# 用法: s123day.sh YYYYMMDD ; 幂等(res_*_pnl 存在即跳过)
set -u
d=$1
# 切到 l3_factor 项目根(本脚本在 bt/ 下), 下面的相对路径都以此为基准。
cd "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=bt/s123
mkdir -p $OUT
P13=data/panels/panel_id_$d.csv
PUQ=data/panels_uniq/panel_id_$d.csv
[ -f "$P13" ] || { echo "[$d] ❌无13f面板"; exit 1; }
run(){ # tag strategy panel sets...
  local tag=$1 stg=$2 panel=$3; shift 3
  [ -f "$OUT/res_${tag}_${d}_pnl.csv" ] && return 0
  python backtest/vgen.py --strategy $stg --panel "$panel" --outdir $OUT --tag ${tag}_$d "$@" >/dev/null 2>&1 || { echo "[$d $tag] ❌vgen"; return 1; }
  mv $OUT/orders_${tag}_${d}_$d.csv $OUT/orders_${tag}_$d.csv 2>/dev/null || true
  mv $OUT/meta_${tag}_${d}_$d.csv $OUT/meta_${tag}_$d.csv 2>/dev/null || true
  python backtest/vloop.py --orders $OUT/orders_${tag}_$d.csv --meta $OUT/meta_${tag}_$d.csv \
      --tl-dir data/tmp/$d --date $d --cache-dir data/cache --outdir $OUT --tag ${tag}_$d \
      --trust-cache >/dev/null 2>&1 || { echo "[$d $tag] ❌vloop"; return 1; }
  python backtest/pnl.py --res $OUT/res_${tag}_$d.csv --meta $OUT/meta_${tag}_$d.csv >/dev/null 2>&1 || { echo "[$d $tag] ❌pnl"; return 1; }
  rm -f $OUT/orders_${tag}_${d}_it*.csv $OUT/fills_${tag}_$d.csv $OUT/orders_${tag}_$d.csv
}
# S2 只要 13f 面板; S1/S3 要合并面板
run s2 backtest/strategies/eat_age_rev.py "$P13" --set hold=8 --set entry=improve --set exit=twostage --set pxmin=15 || true
if [ -f "$PUQ" ]; then
  run s1 backtest/strategies/combo_rev.py "$P13,$PUQ" --set pxmin=3 --set exit=cross || true
  run s3 backtest/strategies/combo_rev.py "$P13,$PUQ" || true
else
  echo "[$d] ⚠无uniq面板, 跳过S1/S3"
fi
echo "[$d] ✅"
