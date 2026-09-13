#!/bin/bash
# TL LOB Builder C++ 编译脚本
# 
# 功能:
#   - 自动检测依赖（CMake, Boost, HDF5等）
#   - 支持 Debug/Release 构建
#   - 增量编译和清理
#   - 编译后验证
# 
# 用法:
#   ./build.sh              # 默认 Release 构建
#   ./build.sh debug        # Debug 构建（包含符号，更慢但可调试）
#   ./build.sh release      # Release 构建（全优化）
#   ./build.sh clean        # 清理构建目录
#   ./build.sh fullclean    # 完全清理（包括CMake缓存）
#   ./build.sh check-deps   # 只检查依赖，不编译

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"

BUILD_TYPE="Release"
ENABLE_OPENMP="ON"
ENABLE_LTO="ON"
ENABLE_HDF5="ON"   # HDF5 输出支持 (与 Python 版本兼容)
ENABLE_ARROW="OFF"
CHECK_DEPS_ONLY="OFF"

# ============================================================
# 辅助函数
# ============================================================

error() {
    echo -e "${RED}❌ 错误: $1${NC}" >&2
    exit 1
}

warn() {
    echo -e "${YELLOW}⚠️  警告: $1${NC}" >&2
}

info() {
    echo -e "${BLUE}ℹ️  $1${NC}"
}

success() {
    echo -e "${GREEN}✓ $1${NC}"
}

check_command() {
    if ! command -v $1 &> /dev/null; then
        error "$1 not found. Please install it first."
    fi
}

check_library() {
    local lib=$1
    local var=$2
    if pkg-config --exists "$lib" 2>/dev/null; then
        success "$lib found"
        return 0
    else
        warn "$lib not found"
        return 1
    fi
}

# ============================================================
# 检查依赖
# ============================================================

check_dependencies() {
    echo ""
    echo "========================================="
    echo "Checking dependencies..."
    echo "========================================="
    
    # 检查必需工具
    check_command cmake
    check_command g++
    check_command make
    success "Build tools found (cmake, g++, make)"
    
    # 检查可选库
    echo ""
    info "Optional libraries:"
    HAS_HDF5=0
    HAS_BOOST=0
    HAS_OPENMP=0
    
    if pkg-config --exists hdf5 2>/dev/null; then
        success "HDF5 found"
        HAS_HDF5=1
    else
        warn "HDF5 not found - HDF5 output will be disabled"
    fi
    
    if pkg-config --exists boost 2>/dev/null; then
        success "Boost found"
        HAS_BOOST=1
    else
        warn "Boost not found"
    fi
    
    # OpenMP通常是编译器内置的，不需要pkg-config
    if echo | gcc -fopenmp -E - > /dev/null 2>&1; then
        success "OpenMP found"
        HAS_OPENMP=1
    else
        warn "OpenMP not found"
    fi
    
    echo ""
    
    # 如果只检查依赖，则返回
    if [ "$CHECK_DEPS_ONLY" = "ON" ]; then
        return 0
    fi
    
    # 根据实际情况调整编译选项
    if [ "$HAS_HDF5" = "0" ] && [ "$ENABLE_HDF5" = "ON" ]; then
        warn "HDF5 support disabled (library not found)"
        ENABLE_HDF5="OFF"
    fi
    
    if [ "$HAS_OPENMP" = "0" ] && [ "$ENABLE_OPENMP" = "ON" ]; then
        warn "OpenMP support disabled (library not found)"
        ENABLE_OPENMP="OFF"
    fi
}

# 解析参数
case "${1:-release}" in
    debug)
        BUILD_TYPE="Debug"
        ENABLE_LTO="OFF"
        info "Building in Debug mode"
        ;;
    release)
        BUILD_TYPE="Release"
        info "Building in Release mode with full optimizations"
        ;;
    check-deps|check-dep)
        CHECK_DEPS_ONLY="ON"
        info "Checking dependencies only (no build)"
        ;;
    clean)
        info "Cleaning build directory..."
        rm -rf build
        success "Build directory cleaned"
        exit 0
        ;;
    fullclean)
        info "Full clean (build + CMake cache)..."
        rm -rf build CMakeFiles CMakeCache.txt
        success "Full clean complete"
        exit 0
        ;;
    *)
        echo "Usage: $0 [debug|release|clean|fullclean|check-deps]"
        echo ""
        echo "Options:"
        echo "  release      Build in Release mode (default, with optimizations)"
        echo "  debug        Build in Debug mode (slower but debuggable)"
        echo "  clean        Remove build directory"
        echo "  fullclean    Remove all build artifacts and CMake cache"
        echo "  check-deps   Check dependencies without building"
        exit 1
        ;;
esac

# 检查依赖
check_dependencies

# 如果只检查依赖，则退出
if [ "$CHECK_DEPS_ONLY" = "ON" ]; then
    exit 0
fi

echo ""
echo "========================================="
echo "Building TL LOB Builder (C++)"
echo "========================================="
echo "Build Type:    ${BUILD_TYPE}"
echo "HDF5:          ${ENABLE_HDF5}"
echo "OpenMP:        ${ENABLE_OPENMP}"
echo "LTO:           ${ENABLE_LTO}"
echo "Arrow/Parquet: ${ENABLE_ARROW}"
echo "========================================="

# 检测 CPU 核心数 (默认使用20核)
if command -v nproc &> /dev/null; then
    NPROC=$(nproc)
else
    NPROC=20
fi

info "Using ${NPROC} parallel jobs"

# 创建构建目录
mkdir -p build
cd build

# 配置
echo ""
info "[1/2] Configuring..."
cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DWITH_HDF5=${ENABLE_HDF5} \
      -DWITH_OPENMP=${ENABLE_OPENMP} \
      -DWITH_LTO=${ENABLE_LTO} \
      -DWITH_ARROW=${ENABLE_ARROW} \
      ..

# 编译
echo ""
info "[2/2] Building with ${NPROC} parallel jobs..."
cmake --build . --config ${BUILD_TYPE} -j${NPROC}

echo ""
echo "========================================="
success "Build complete!"
echo "Binary: build/auction_lob_main"
echo ""
echo "Run with:"
echo "  ./build/auction_lob_main -d 20251231 -s 000001 -m sz   # 深圳单股票"
echo "  ./build/auction_lob_main -d 20251231 -s 600000 -m sh   # 上海单股票"
echo "  ./build/auction_lob_main -d 20251231 -m sz             # 深圳全市场"
echo "========================================="

# bash build.sh clean && bash build.sh