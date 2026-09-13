#!/bin/bash
#
# 集合竞价 action 数据生产流程 (独立于 tl_data_save, 直接吃原始 mdl 行情)
#   1) 按日解压通联 mdl 行情 zip
#   2) auction_lob_main 重建全深度订单簿 + 集合竞价虚拟撮合 -> 每股 *_action.bin
#   3) merge_action_parquet.py 合并当日全市场 -> <auction_mx>/<date>_auction.parquet
#
# 用法: 编辑下方 start_date/end_date 后  ./run.sh

set -u

# 处理日期区间 (YYYYMMDD)
start_date="20240130"
end_date="20241231"

# 路径: cpp 上一级即 auction 项目根
cpp_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(dirname "$cpp_dir")"

# 路径统一由 qr/qr_paths.sh 解析(QR_* 环境变量 → qr.toml → 内置默认),不在这里写死。
# shellcheck source=../../../qr/qr_paths.sh
source "$project_dir/../../qr/qr_paths.sh"

# 通联数据根目录 (每日: $tl_base_dir/<date>/mdl_*.csv[.zip])
tl_base_dir="${tl_base_dir:-$(qr_path tl_zip)}"
parquet_out_dir="${parquet_out_dir:-$(qr_path auction_mx)}"   # 当日 parquet: <date>_auction.parquet
# 每股 shard 暂存: $staging_dir/<date>/<symbol>_action.bin (main 内部拼 /<date>)
staging_dir="${staging_dir:-$parquet_out_dir/.auction}"
merge_script="$project_dir/merge_action_parquet.py"

# Python 解释器 (需含 numpy/pyarrow; 可用 PYTHON 环境变量覆盖)
PYTHON="${PYTHON:-python3}"

# 并行进程数
workers=40

# 运行前确保这些 CSV 可用 (缺失时尝试解压对应 zip)
# 深圳: mdl_6_33(委托) + mdl_6_36(成交/撤单); 上海: mdl_4_24(逐笔全息)
required_files=(
    "mdl_4_24_0.csv"
    "mdl_6_33_0.csv"
    "mdl_6_36_0.csv"
)

unzip_target_for_file() {
    local date_dir="$1"
    local target_file="$2"

    if [[ -f "$date_dir/$target_file" ]]; then
        return 0
    fi

    local base_name="${target_file%.csv}"
    local zip_path=""

    if [[ -f "$date_dir/${target_file}.zip" ]]; then
        zip_path="$date_dir/${target_file}.zip"
    elif [[ -f "$date_dir/${base_name}.zip" ]]; then
        zip_path="$date_dir/${base_name}.zip"
    else
        zip_path=$(find "$date_dir" -maxdepth 1 -type f -name "*${base_name}*.zip" | head -n 1)
    fi

    if [[ -z "$zip_path" ]]; then
        return 1
    fi

    echo "  解压: $(basename "$zip_path")"
    unzip -oq "$zip_path" -d "$date_dir"

    if [[ -f "$date_dir/$target_file" ]]; then
        return 0
    fi

    local extracted
    extracted=$(find "$date_dir" -type f -name "$target_file" | head -n 1)
    if [[ -n "$extracted" && "$extracted" != "$date_dir/$target_file" ]]; then
        mv -f "$extracted" "$date_dir/$target_file"
    fi

    [[ -f "$date_dir/$target_file" ]]
}

# BSD date(macOS)和 GNU date(Linux/WSL)的参数不通用, 两种都试。
next_date() {
    date -d "$1 + 1 day" +"%Y%m%d" 2>/dev/null || date -j -v+1d -f %Y%m%d "$1" +%Y%m%d
}

if [[ ! -f "$merge_script" ]]; then
    echo "错误: 找不到合并脚本 $merge_script" >&2
    echo "第 2 步(auction_lob_main 出 .bin 分片)可以单独跑, 第 3 步合并 parquet 缺这个脚本。" >&2
    exit 1
fi

current_date="$start_date"
while [[ "$current_date" -le "$end_date" ]]
do
    echo "正在处理日期: $current_date"

    date_dir="$tl_base_dir/$current_date"
    if [[ ! -d "$date_dir" ]]; then
        echo "警告：目录不存在，跳过 $date_dir"
        current_date=$(next_date "$current_date")
        continue
    fi

    echo "  检查并解压当日必要数据..."
    for f in "${required_files[@]}"
    do
        if unzip_target_for_file "$date_dir" "$f"; then
            echo "  就绪: $f"
        else
            echo "  未找到对应 zip 或解压失败: $f"
        fi
    done

    # 1) 重建订单簿 + 集合竞价撮合 -> 每股 *_action.bin (沪深全市场)
    # 注: auction_lob_main 内部会自动拼 <基目录>/<date>, 故 -f/-o 传基目录
    "$cpp_dir/build/auction_lob_main" -d "$current_date" -w "$workers" -f "$tl_base_dir" -o "$staging_dir"
    if [ $? -ne 0 ]; then
        echo "警告：auction_lob_main 处理 $current_date 失败。"
        current_date=$(next_date "$current_date")
        continue
    fi

    # 2) 合并当日全市场 shard -> $parquet_out_dir/<date>_auction.parquet
    "$PYTHON" "$merge_script" -d "$current_date" \
        --staging "$staging_dir" --out "$parquet_out_dir"
    if [ $? -ne 0 ]; then
        echo "警告：merge_action_parquet 处理 $current_date 失败。"
    fi

    current_date=$(next_date "$current_date")
done

echo "所有日期处理完毕。"
