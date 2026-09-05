#!/usr/bin/env bash
# 端到端人工核对: 手写 Flow 记录 + 手写虚拟订单 → 人工推演出预期 → 跑程序对答案。
#
# 这是唯一一层【穿过真实 Flow 解析器】的验证(vsim_test 用的是内存里合成的 UnifiedRecord,
# 绕过了 CSV 解析、列名映射、订单号取模 norm()、时间戳解析这一整段)。
# expected.csv 里的数字是【先人工推演出来的】, 不是把程序输出抄下来的 —— 详见 README.md。
set -u
cd "$(dirname "$0")/.."
# 默认用源码树里的 build/vsim; ctest 会用 VSIM 覆盖成实际构建出的那个二进制。
VSIM=${VSIM:-./build/vsim}
[ -x "$VSIM" ] || { echo "先构建: cmake --build build -j"; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for c in 1 2 3 4 5 6 7 8 9 10 11; do
    $VSIM --flow handcheck/case${c}_flow.csv --date 20260707 \
          --orders handcheck/case${c}_orders.csv --out "$TMP/out${c}.csv" >/dev/null 2>&1 \
      || { echo "❌ case$c 运行失败"; exit 1; }
done

python3 - "$TMP" <<'PY'
import csv, sys, os
tmp = sys.argv[1]
exp = {}
for r in csv.DictReader(l for l in open("handcheck/expected.csv") if not l.startswith("#")):
    exp[(r["case"], r["id"])] = r

FIELDS = [("filled","filled"),("filled_pess","filled_pess"),("filled_opt","filled_opt"),
          ("avg_price","avg_price"),("through","through_filled"),("state","state"),
          ("ahead_rem","ahead_remaining"),("displaced","displaced"),("slippage","slippage")]
bad = 0; n = 0
for c in ("1","2","3","4","5","6","7","8","9","10","11"):
    for r in csv.DictReader(open(os.path.join(tmp, f"out{c}.csv"))):
        e = exp.get((c, r["id"]))
        if e is None:
            print(f"❌ case{c} {r['id']}: expected.csv 里没有这条"); bad += 1; continue
        for ek, ak in FIELDS:
            pv, av = e[ek], r[ak]
            try:    ok = abs(float(pv) - float(av)) < 1e-6
            except ValueError: ok = (pv == av)
            n += 1
            if not ok:
                bad += 1
                print(f"❌ case{c} {r['id']}.{ek}: 人工推演={pv}  程序输出={av}")
print(f"\n{'✅ 人工推演 vs 程序输出: %d/%d 字段全部一致' % (n-bad, n) if bad==0 else '❌ %d/%d 字段不符' % (bad, n)}")
sys.exit(1 if bad else 0)
PY
