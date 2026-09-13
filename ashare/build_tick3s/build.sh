#!/bin/bash
#
# LOB数据处理程序 - 自动构建打包脚本
# 
# 用法:
#   ./build.sh              # 编译并打包
#   ./build.sh clean        # 清理构建目录
#   ./build.sh deps         # 仅安装依赖
#   ./build.sh build        # 仅编译（不打包）
#   ./build.sh pack         # 仅打包（需要先编译）
#

set -e  # 出错时停止执行

# ============================================================
# 配置
# ============================================================
PROJECT_NAME="lob_processor"
VERSION="1.0.0"
BUILD_DIR="build"
PACK_DIR="dist"
INSTALL_PREFIX="/usr/local"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================
# 辅助函数
# ============================================================
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 获取脚本所在目录
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# ============================================================
# 检查依赖
# ============================================================
check_dependencies() {
    log_info "检查依赖..."
    
    local missing_deps=()
    
    # 检查cmake
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi
    
    # 检查g++
    if ! command -v g++ &> /dev/null; then
        missing_deps+=("g++")
    fi
    
    # 检查make
    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi
    
    # 检查HDF5
    if ! pkg-config --exists hdf5 2>/dev/null; then
        if [ ! -f "/usr/include/hdf5.h" ] && [ ! -f "/usr/local/include/hdf5.h" ] && \
           [ ! -f "/usr/include/hdf5/serial/hdf5.h" ]; then
            missing_deps+=("libhdf5-dev")
        fi
    fi
    
    # 检查abseil
    if [ ! -f "/usr/include/absl/numeric/int128.h" ] && \
       [ ! -f "/usr/local/include/absl/numeric/int128.h" ]; then
        missing_deps+=("libabsl-dev (abseil)")
    fi
    
    # 检查clickhouse-cpp
    if [ ! -f "/usr/local/include/clickhouse/client.h" ]; then
        missing_deps+=("clickhouse-cpp")
    fi
    
    if [ ${#missing_deps[@]} -gt 0 ]; then
        log_error "缺少以下依赖: ${missing_deps[*]}"
        log_info "请运行 './build.sh deps' 安装依赖"
        return 1
    fi
    
    log_success "依赖检查通过"
    return 0
}

# ============================================================
# 安装依赖
# ============================================================
install_dependencies() {
    log_info "安装依赖..."
    
    # 检测系统类型
    if [ -f /etc/debian_version ]; then
        # Debian/Ubuntu
        log_info "检测到 Debian/Ubuntu 系统"
        
        # apt-get update 可能因为某些源失效而报错，忽略错误继续
        sudo apt-get update || log_warn "apt-get update 有警告，继续安装..."
        
        # 安装基础依赖
        sudo apt-get install -y cmake g++ make libhdf5-dev git liblz4-dev libzstd-dev || true
        
        # Ubuntu 20.04 没有 libabsl-dev，需要从源码安装
        # 尝试安装，如果失败则从源码编译
        if ! sudo apt-get install -y libabsl-dev 2>/dev/null; then
            log_warn "libabsl-dev 不可用，将从源码安装 abseil..."
            install_abseil_from_source
        fi
        
        # 安装cityhash（通常需要从源码安装）
        install_cityhash_from_source
        
    elif [ -f /etc/redhat-release ]; then
        # CentOS/RHEL
        log_info "检测到 CentOS/RHEL 系统"
        sudo yum install -y cmake gcc-c++ make hdf5-devel git lz4-devel libzstd-devel
        # CentOS需要从源码安装abseil
        install_abseil_from_source
        install_cityhash_from_source
        
    else
        log_warn "未知系统类型，请手动安装依赖"
    fi
    
    # 再次检查abseil是否安装成功
    if [ ! -f "/usr/include/absl/numeric/int128.h" ] && [ ! -f "/usr/local/include/absl/numeric/int128.h" ]; then
        log_warn "abseil仍未安装，尝试从源码安装..."
        install_abseil_from_source
    fi
    
    # 安装 clickhouse-cpp
    install_clickhouse_cpp
    
    # 更新动态库缓存
    sudo ldconfig 2>/dev/null || true
    
    log_success "依赖安装完成"
}

# ============================================================
# 从源码安装abseil
# ============================================================
install_abseil_from_source() {
    log_info "从源码安装 abseil..."
    
    ABSEIL_DIR="$SCRIPT_DIR/abseil-cpp"
    
    if [ ! -d "$ABSEIL_DIR" ]; then
        git clone https://github.com/abseil/abseil-cpp.git "$ABSEIL_DIR"
    fi
    
    cd "$ABSEIL_DIR"
    # 使用稳定版本
    git fetch --tags 2>/dev/null || true
    git checkout 20230802.1 2>/dev/null || git checkout lts_2023_08_02 2>/dev/null || true
    
    rm -rf build
    mkdir -p build && cd build
    cmake .. \
        -DCMAKE_CXX_STANDARD=17 \
        -DABSL_BUILD_TESTING=OFF \
        -DABSL_USE_GOOGLETEST_HEAD=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX=/usr/local
    make -j$(nproc)
    sudo make install
    cd "$SCRIPT_DIR"
    
    # 更新库缓存
    sudo ldconfig 2>/dev/null || true
    
    log_success "abseil 安装完成"
}

# ============================================================
# 从源码安装cityhash
# ============================================================
install_cityhash_from_source() {
    if [ -f "/usr/local/lib/libcityhash.a" ] || [ -f "/usr/local/lib/libcityhash.so" ]; then
        log_info "cityhash 已安装"
        return 0
    fi
    
    log_info "从源码安装 cityhash..."
    
    CITYHASH_DIR="$SCRIPT_DIR/cityhash"
    
    if [ ! -d "$CITYHASH_DIR" ]; then
        git clone https://github.com/google/cityhash.git "$CITYHASH_DIR"
    fi
    
    cd "$CITYHASH_DIR"
    
    # cityhash使用autotools构建
    if [ ! -f "configure" ]; then
        ./configure || true
    fi
    
    # 如果没有configure，手动编译
    if [ ! -f "configure" ]; then
        log_info "手动编译cityhash..."
        g++ -O3 -c src/city.cc -o city.o
        ar rcs libcityhash.a city.o
        sudo cp libcityhash.a /usr/local/lib/
        sudo cp src/city.h src/citycrc.h /usr/local/include/ 2>/dev/null || true
        sudo cp src/city.h /usr/local/include/
    else
        ./configure --prefix=/usr/local
        make -j$(nproc)
        sudo make install
    fi
    
    cd "$SCRIPT_DIR"
    sudo ldconfig 2>/dev/null || true
    
    log_success "cityhash 安装完成"
}

# ============================================================
# 安装clickhouse-cpp
# ============================================================
install_clickhouse_cpp() {
    if [ -f "/usr/local/include/clickhouse/client.h" ]; then
        log_info "clickhouse-cpp 已安装"
        return 0
    fi
    
    log_info "安装 clickhouse-cpp..."
    
    CLICKHOUSE_CPP_DIR="$SCRIPT_DIR/clickhouse-cpp"
    
    if [ ! -d "$CLICKHOUSE_CPP_DIR" ]; then
        git clone https://github.com/ClickHouse/clickhouse-cpp.git "$CLICKHOUSE_CPP_DIR"
    fi
    
    cd "$CLICKHOUSE_CPP_DIR"
    rm -rf build
    mkdir -p build && cd build
    
    # 使用系统的abseil和lz4
    cmake .. \
        -DWITH_SYSTEM_ABSEIL=ON \
        -DWITH_SYSTEM_LZ4=ON \
        -DCMAKE_CXX_STANDARD=17
    make -j$(nproc)
    sudo make install
    cd "$SCRIPT_DIR"
    
    # 更新库缓存
    sudo ldconfig 2>/dev/null || true
    
    log_success "clickhouse-cpp 安装完成"
}

# ============================================================
# 清理构建目录
# ============================================================
clean() {
    log_info "清理构建目录..."
    
    rm -rf "$BUILD_DIR"
    rm -rf "$PACK_DIR"
    
    log_success "清理完成"
}

# ============================================================
# 编译
# ============================================================
build() {
    log_info "开始编译 $PROJECT_NAME v$VERSION..."
    
    # 检查依赖
    check_dependencies || exit 1
    
    # 创建构建目录
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # 配置CMake
    log_info "配置CMake..."
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DCLICKHOUSE_INCLUDE_DIR=/usr/local/include \
        -DCLICKHOUSE_LIB_DIR=/usr/local/lib
    
    # 编译
    log_info "编译中..."
    make -j$(nproc)
    
    cd "$SCRIPT_DIR"
    
    log_success "编译完成: $BUILD_DIR/$PROJECT_NAME"
}

# ============================================================
# 打包
# ============================================================
pack() {
    log_info "开始打包..."
    
    # 检查可执行文件是否存在
    if [ ! -f "$BUILD_DIR/$PROJECT_NAME" ]; then
        log_error "可执行文件不存在，请先编译: ./build.sh build"
        exit 1
    fi
    
    # 创建打包目录
    PACK_NAME="${PROJECT_NAME}-${VERSION}-linux-$(uname -m)"
    PACK_PATH="$PACK_DIR/$PACK_NAME"
    
    rm -rf "$PACK_PATH"
    mkdir -p "$PACK_PATH"
    mkdir -p "$PACK_PATH/bin"
    mkdir -p "$PACK_PATH/lib"
    mkdir -p "$PACK_PATH/config"
    
    # 复制可执行文件
    cp "$BUILD_DIR/$PROJECT_NAME" "$PACK_PATH/bin/"
    
    # 复制依赖库
    log_info "收集依赖库..."
    
    # 使用ldd获取依赖库
    ldd "$BUILD_DIR/$PROJECT_NAME" | grep "=> /" | awk '{print $3}' | while read lib; do
        # 只复制非系统库
        case "$lib" in
            /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*)
                # 跳过基础系统库
                if [[ "$lib" == *"libc.so"* ]] || \
                   [[ "$lib" == *"libm.so"* ]] || \
                   [[ "$lib" == *"libpthread"* ]] || \
                   [[ "$lib" == *"libdl.so"* ]] || \
                   [[ "$lib" == *"librt.so"* ]] || \
                   [[ "$lib" == *"ld-linux"* ]]; then
                    continue
                fi
                ;;
        esac
        
        # 复制HDF5和ClickHouse相关库
        if [[ "$lib" == *"hdf5"* ]] || \
           [[ "$lib" == *"clickhouse"* ]] || \
           [[ "$lib" == *"cityhash"* ]] || \
           [[ "$lib" == *"lz4"* ]] || \
           [[ "$lib" == *"zstd"* ]] || \
           [[ "$lib" == *"absl"* ]]; then
            cp -L "$lib" "$PACK_PATH/lib/" 2>/dev/null || true
        fi
    done
    
    # 创建启动脚本
    cat > "$PACK_PATH/bin/run.sh" << 'EOF'
#!/bin/bash
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export LD_LIBRARY_PATH="$SCRIPT_DIR/../lib:$LD_LIBRARY_PATH"

# 自动检测并设置 HDF5 blosc 压缩插件路径 (PyTables格式必需)
if [ -z "$HDF5_PLUGIN_PATH" ]; then
    PLUGIN_PATH=$(python3 -c "import hdf5plugin; print(hdf5plugin.PLUGINS_PATH)" 2>/dev/null)
    if [ -z "$PLUGIN_PATH" ]; then
        for p in /usr/local/hdf5/lib/plugin /usr/lib/x86_64-linux-gnu/hdf5/plugins /usr/lib/hdf5/plugin; do
            if [ -d "$p" ]; then PLUGIN_PATH="$p"; break; fi
        done
    fi
    if [ -n "$PLUGIN_PATH" ]; then
        export HDF5_PLUGIN_PATH="$PLUGIN_PATH"
        echo "[INFO] HDF5_PLUGIN_PATH=$PLUGIN_PATH"
    else
        echo "[WARN] blosc HDF5 plugin not found. Install with: pip install hdf5plugin"
    fi
fi

exec "$SCRIPT_DIR/lob_processor" "$@"
EOF
    chmod +x "$PACK_PATH/bin/run.sh"
    
    # 创建配置文件模板
    cat > "$PACK_PATH/config/config.example.sh" << 'EOF'
# LOB 数据处理程序的运行时配置。source 这个文件后再跑 bin/run.sh。
# 程序只从环境变量读配置, 不读 ini。口令不要提交进版本控制。

# ── ClickHouse 连接 ───────────────────────────────────────────────────────────
export QR_CH_HOST=127.0.0.1
export QR_CH_PORT=9000          # clickhouse-cpp 用 TCP 端口, 不是 HTTP 的 8123
export QR_CH_USER=default
export QR_CH_PASSWORD=          # 填你自己的口令
export QR_CH_DATABASE=Tick_TL

# ── 数据目录 ──────────────────────────────────────────────────────────────────
# 不设就按仓库根的 qr.toml / 内置默认解析, 见 qr/qr_paths.h。
# export QR_LOB_H5=/mnt/d/quant/Lob_local
# export QR_LOB_LEVEL2=/mnt/d/quant/Level2
# export QR_LOB_3S=/mnt/d/quant/Lob_new_3s
# export QR_TICK3S_PARQUET=/mnt/d/quant/Lob_new_3s_MX

# ── 处理参数 ──────────────────────────────────────────────────────────────────
# 这几项是命令行开关, 见 lob_processor --help:
#   --market / -s / -t / --time-start / --time-end / --output
EOF
    
    # 创建README
    cat > "$PACK_PATH/README.txt" << EOF
LOB数据1秒降频处理程序 v$VERSION
=====================================

用法:
  ./bin/run.sh                      # 处理当天数据
  ./bin/run.sh 20251225             # 处理指定日期
  ./bin/run.sh 20251201 20251225    # 处理日期范围

目录结构:
  bin/            可执行文件
  lib/            依赖库
  config/         配置文件模板

注意:
  1. 首次运行前 source config/config.example.sh 并按需修改(至少填 QR_CH_PASSWORD)
  2. 确保ClickHouse服务器可访问
  3. 确保H5数据目录存在且有读取权限

EOF
    
    # 打包为tar.gz
    log_info "创建压缩包..."
    cd "$PACK_DIR"
    tar -czvf "${PACK_NAME}.tar.gz" "$PACK_NAME"
    cd "$SCRIPT_DIR"
    
    # 计算MD5
    md5sum "$PACK_DIR/${PACK_NAME}.tar.gz" > "$PACK_DIR/${PACK_NAME}.tar.gz.md5"
    
    # 显示打包结果
    PACK_SIZE=$(du -h "$PACK_DIR/${PACK_NAME}.tar.gz" | cut -f1)
    
    log_success "打包完成!"
    echo ""
    echo "============================================"
    echo "  输出文件: $PACK_DIR/${PACK_NAME}.tar.gz"
    echo "  文件大小: $PACK_SIZE"
    echo "  MD5校验:  $PACK_DIR/${PACK_NAME}.tar.gz.md5"
    echo "============================================"
}

# ============================================================
# 安装到系统
# ============================================================
install() {
    log_info "安装到系统..."
    
    if [ ! -f "$BUILD_DIR/$PROJECT_NAME" ]; then
        log_error "可执行文件不存在，请先编译: ./build.sh build"
        exit 1
    fi
    
    cd "$BUILD_DIR"
    sudo make install
    cd "$SCRIPT_DIR"
    
    log_success "安装完成: $INSTALL_PREFIX/bin/$PROJECT_NAME"
}

# ============================================================
# 显示帮助
# ============================================================
show_help() {
    echo "LOB数据处理程序构建脚本"
    echo ""
    echo "用法: ./build.sh [命令]"
    echo ""
    echo "命令:"
    echo "  (无参数)    编译并打包"
    echo "  deps        安装依赖"
    echo "  build       仅编译"
    echo "  pack        仅打包"
    echo "  install     安装到系统"
    echo "  clean       清理构建目录"
    echo "  help        显示帮助"
    echo ""
    echo "示例:"
    echo "  ./build.sh deps     # 首次使用，安装依赖"
    echo "  ./build.sh          # 编译并打包"
    echo "  ./build.sh clean    # 清理后重新编译"
}

# ============================================================
# 主函数
# ============================================================
main() {
    echo ""
    echo "========================================"
    echo "  $PROJECT_NAME 构建脚本 v$VERSION"
    echo "========================================"
    echo ""
    
    case "${1:-}" in
        deps)
            install_dependencies
            ;;
        build)
            build
            ;;
        pack)
            pack
            ;;
        install)
            install
            ;;
        clean)
            clean
            ;;
        help|--help|-h)
            show_help
            ;;
        "")
            # 默认：编译并打包
            build
            pack
            ;;
        *)
            log_error "未知命令: $1"
            show_help
            exit 1
            ;;
    esac
}

main "$@"
