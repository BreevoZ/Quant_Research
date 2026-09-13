#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BIN=${BIN:-"$SCRIPT_DIR/build/tl_to_flow_csv"}
# 路径统一由 qr/qr_paths.sh 解析(QR_* 环境变量 → qr.toml → 内置默认),不在这里写死。
# shellcheck source=../../qr/qr_paths.sh
source "$SCRIPT_DIR/../../qr/qr_paths.sh"
TL_ROOT=${TL_ROOT:-$(qr_path tl_zip)}
OUTPUT_ROOT=${OUTPUT_ROOT:-$(qr_path flow_root)}
SORT_TMP_DIR=${SORT_TMP_DIR:-${TMPDIR:-/tmp}}
SYMBOL_PREFIX_FILTER=${SYMBOL_PREFIX_FILTER:-"SH:6,SZ:0|3"}

usage() {
  cat <<USAGE
Usage:
  $0 YYYYMMDD
  $0 START_YYYYMMDD END_YYYYMMDD

Environment overrides:
  BIN=$BIN
  TL_ROOT=$TL_ROOT
  OUTPUT_ROOT=$OUTPUT_ROOT
  SORT_TMP_DIR=$SORT_TMP_DIR
  SYMBOL_PREFIX_FILTER=$SYMBOL_PREFIX_FILTER

Extra arguments after the date/date-range are passed through to tl_to_flow_csv.
USAGE
}

normalize_date() {
  local d=${1//-/}
  if [[ ! $d =~ ^[0-9]{8}$ ]]; then
    echo "invalid date: $1" >&2
    exit 2
  fi
  printf "%s" "$d"
}

# BSD date(macOS)和 GNU date(Linux/WSL)的参数不通用, 两种都试。
next_date() {
  date -d "$1 +1 day" +%Y%m%d 2>/dev/null || date -j -v+1d -f %Y%m%d "$1" +%Y%m%d
}

ensure_inputs() {
  local day=$1
  local day_dir="$TL_ROOT/$day"
  local required=(mdl_4_24_0.csv mdl_6_33_0.csv mdl_6_36_0.csv)

  if [[ ! -d $day_dir ]]; then
    echo "[$day] skip: input directory missing: $day_dir"
    return 1
  fi

  local missing=()
  local csv
  for csv in "${required[@]}"; do
    [[ -s "$day_dir/$csv" ]] || missing+=("$csv")
  done

  if (( ${#missing[@]} == 0 )); then
    echo "[$day] input CSV exists, skip unzip"
    return
  fi

  echo "[$day] missing CSV: ${missing[*]}"
  local csv zip candidates found_zip
  for csv in "${missing[@]}"; do
    candidates=(
      "$day_dir/${day}_${csv}.zip"
      "$day_dir/${day}_${csv%.csv}.CSV.zip"
      "$day_dir/${csv}.zip"
      "$day_dir/${csv%.csv}.CSV.zip"
    )
    found_zip=""
    for zip in "${candidates[@]}"; do
      if [[ -f $zip ]]; then
        found_zip=$zip
        break
      fi
    done
    if [[ -z $found_zip ]]; then
      echo "[$day] skip: missing $csv and required zip not found"
      return 1
    fi
    echo "[$day] unzip $(basename "$found_zip")"
    mkdir -p "$day_dir"
    unzip -n "$found_zip" -d "$day_dir"
  done

  missing=()
  for csv in "${required[@]}"; do
    [[ -s "$day_dir/$csv" ]] || missing+=("$csv")
  done
  if (( ${#missing[@]} > 0 )); then
    echo "error: still missing CSV files after unzip under $day_dir: ${missing[*]}" >&2
    exit 1
  fi
}

run_one_day() {
  local day=$1
  shift || true

  if ! ensure_inputs "$day"; then
    return 0
  fi

  echo "[$day] start tl_to_flow_csv"
  "$BIN" \
    -d "$day" \
    --tl-root "$TL_ROOT" \
    --output-root "$OUTPUT_ROOT" \
    --output-instruments-xml "$OUTPUT_ROOT/$day.xml" \
    --instruments-template "$SCRIPT_DIR/Instruments.xml" \
    --sort-key appseq \
    --symbol-prefix-filter "$SYMBOL_PREFIX_FILTER" \
    --sort-tmp-dir "$SORT_TMP_DIR" \
    --missing-policy fail \
    --number-format compact \
    --sh-turnover raw \
    --sh-trade-dir-mode zero \
    --sz-trade-turnover raw \
    --sz-cancel-turnover raw \
    --sz-ordtype-map 49:1,85:U,default:2 \
    --sh-price-type 0 \
    "$@"
  echo "[$day] done: $OUTPUT_ROOT/$day.csv"
  echo "[$day] xml:  $OUTPUT_ROOT/$day.xml"
}

if (( $# < 1 )); then
  usage
  exit 2
fi

START_DATE=$(normalize_date "$1")
shift
END_DATE=$START_DATE
if (( $# > 0 )) && [[ $1 =~ ^([0-9]{8}|[0-9]{4}-[0-9]{2}-[0-9]{2})$ ]]; then
  END_DATE=$(normalize_date "$1")
  shift
fi

if [[ $START_DATE > $END_DATE ]]; then
  echo "error: start date $START_DATE is after end date $END_DATE" >&2
  exit 2
fi

if [[ ! -x $BIN ]]; then
  echo "error: binary not executable: $BIN" >&2
  echo "build it first, e.g. g++ -O3 -std=c++17 -Wall -Wextra -o $BIN $SCRIPT_DIR/build/tl_to_flow_csv.cpp" >&2
  exit 1
fi

cur=$START_DATE
while :; do
  run_one_day "$cur" "$@"
  [[ $cur == "$END_DATE" ]] && break
  cur=$(next_date "$cur")
done
