#!/bin/bash
# LOB数据处理程序 - 运行脚本
# 自动检测并设置HDF5 blosc压缩插件路径

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# 自动检测并设置 HDF5 blosc 压缩插件路径 (PyTables格式必需)
if [ -z "$HDF5_PLUGIN_PATH" ]; then
    # 方案1: 从hdf5plugin包获取 (最可靠)
    PLUGIN_PATH=$(python3 -c "import hdf5plugin; print(hdf5plugin.PLUGINS_PATH)" 2>/dev/null)
    if [ -z "$PLUGIN_PATH" ]; then
        # 方案2: 检查系统默认插件目录
        for p in /usr/local/hdf5/lib/plugin /usr/lib/x86_64-linux-gnu/hdf5/plugins /usr/lib/hdf5/plugin; do
            if [ -d "$p" ]; then
                PLUGIN_PATH="$p"
                break
            fi
        done
    fi
    if [ -n "$PLUGIN_PATH" ]; then
        export HDF5_PLUGIN_PATH="$PLUGIN_PATH"
        echo "[INFO] HDF5_PLUGIN_PATH=$PLUGIN_PATH"
    else
        echo "[WARN] blosc HDF5 plugin not found. SZ market PyTables files may fail."
        echo "[WARN] Install with: pip install hdf5plugin"
    fi
fi

exec "$SCRIPT_DIR/build/lob_processor" "$@"
# export HDF5_PLUGIN_PATH=$(python3 -c "import hdf5plugin; print(hdf5plugin.PLUGINS_PATH)")
# ./build/lob_processor -t Tick_1s_data --time-start 09:00:00 --time-end 11:00:00 20260101 20260131