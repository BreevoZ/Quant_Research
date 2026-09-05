#!/usr/bin/env bash
cd /home/yhzhou/l3_factor
# 等下载收尾: 连续 3 次探测文件数不再增长即认为完成(避免死等失败的少数)
prev=0; stable=0
for i in $(seq 1 120); do
  n=$(find data/tess_ffi -name "*_lc.fits" | wc -l)
  [ "$n" -ge 3300 ] && break
  if [ "$n" -eq "$prev" ]; then stable=$((stable+1)); else stable=0; fi
  [ "$stable" -ge 3 ] && break
  prev=$n; sleep 60
done
echo "[复核] 下载收尾: $(find data/tess_ffi -name '*_lc.fits' | wc -l) 个文件"
python explore/tess_placebo.py --n-stars 2800 --n-maps 20 --days 25 > bt/tess_placebo_full.out 2>bt/tess_placebo_full.err
echo "RECHECK_DONE"
