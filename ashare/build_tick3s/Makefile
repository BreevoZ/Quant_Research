# ============================================================
# lob_data_processor  —  build / test / compare
# ============================================================
#
# 快速开始:
#   make               # 编译
#   make run-4stocks   # 对 20260611 四只股票生成 .bin
#   make compare       # 对比 gen vs ref
#   make clean         # 清理 .o / 可执行文件
#
# 首次使用（HDF5 不在 /tmp/hdf5_extracted）:
#   make setup-hdf5    # 自动从系统 deb 包解压到 /tmp/hdf5_extracted
#
# 覆盖 HDF5 路径:
#   make HDF5_BASE=/your/path build
#
# ============================================================

# ---------- 路径 ----------
HDF5_BASE := /tmp/hdf5_extracted/usr
HDF5_INC  := $(HDF5_BASE)/include/hdf5/serial
HDF5_LIB  := $(HDF5_BASE)/lib/x86_64-linux-gnu

# ClickHouse 桩头（已提交到仓库根 third_party/，无需额外安装）
CH_INC    := ../../third_party

# 输出
OUT_DIR   := build
BIN       := $(OUT_DIR)/lob_processor

# 测试数据。REF_PQ 走 qr 路径解析(QR_TICK3S_REF / qr.toml / 内置默认), 不写死。
TEST_OUT  := $(if $(TMPDIR),$(TMPDIR),/tmp)/out_test
QR_PATH   := PYTHONPATH=../.. python3 -c 'import sys; from qr import paths; print(paths.get(sys.argv[1]))'
DATE      := 20260611
REF_PQ    := $(shell $(QR_PATH) tick3s_ref)/$(DATE)_tick_3s.parquet
TEST_SYMS := 600000 688981 000001 300750

# ---------- 编译选项 ----------
CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter
INCS     := -I$(CH_INC) -I../../qr -I$(HDF5_INC)
CXXFLAGS += -DQR_REPO_ROOT_FALLBACK='"$(abspath ../..)"'
LIBS     := -L$(HDF5_LIB) -lhdf5_cpp -lhdf5_serial -lsz \
            -Wl,-rpath,$(HDF5_LIB) -lpthread

SRCS     := lob_data_processor.cpp main.cpp
OBJS     := $(patsubst %.cpp,$(OUT_DIR)/%.o,$(SRCS))

# ---------- 运行时环境 ----------
PLUGIN   := $(shell python3 -c "import hdf5plugin; print(hdf5plugin.PLUGINS_PATH)" 2>/dev/null)
RUN_ENV  := LD_LIBRARY_PATH=$(HDF5_LIB) HDF5_PLUGIN_PATH=$(PLUGIN)

# ============================================================
.PHONY: all build run-4stocks compare clean setup-hdf5 help

all: build

# ---------- 编译 ----------
build: $(BIN)

$(BIN): $(OBJS) | $(OUT_DIR)
	$(CXX) $^ -o $@ $(LIBS)
	@echo "==> Built: $@"

$(OUT_DIR)/%.o: %.cpp | $(OUT_DIR)
	$(CXX) $(CXXFLAGS) $(INCS) -c $< -o $@

$(OUT_DIR):
	mkdir -p $@

# ---------- 生成测试数据 ----------
run-4stocks: build
	mkdir -p $(TEST_OUT)
	@for sym in $(TEST_SYMS); do \
	    echo "--- $$sym ---"; \
	    $(RUN_ENV) $(BIN) --output parquet --parquet-out-dir $(TEST_OUT) -s $$sym $(DATE) || exit 1; \
	done
	@echo "==> Shards: $(TEST_OUT)/.staging/$(DATE)/"
	@ls $(TEST_OUT)/.staging/$(DATE)/

# ---------- 对比 gen vs ref ----------
compare:
	$(RUN_ENV) python3 scripts/compare_quick.py \
	    --bin-dir $(TEST_OUT)/.staging/$(DATE) \
	    --ref     $(REF_PQ) \
	    --date    $(DATE)

# ---------- 清理 ----------
clean:
	rm -f $(OBJS) $(BIN)
	@echo "==> Cleaned"

# ---------- 一键准备 HDF5（首次使用）----------
setup-hdf5:
	@if [ -d "$(HDF5_BASE)" ]; then \
	    echo "==> HDF5 already at $(HDF5_BASE)"; \
	else \
	    echo "==> Extracting HDF5 from apt..."; \
	    mkdir -p /tmp/_hdf5_pkg; \
	    cd /tmp/_hdf5_pkg && \
	    apt-get download libhdf5-serial-dev libhdf5-cpp-103 libhdf5-serial-103-1 libsz2 2>/dev/null; \
	    for deb in /tmp/_hdf5_pkg/*.deb; do dpkg-deb -x "$$deb" /tmp/hdf5_extracted; done; \
	    rm -rf /tmp/_hdf5_pkg; \
	    echo "==> Done: $(HDF5_BASE)"; \
	fi

help:
	@echo "Targets:"
	@echo "  build        编译 $(BIN)"
	@echo "  run-4stocks  对 $(DATE) 四只股票生成 .bin（输出 $(TEST_OUT)）"
	@echo "  compare      对比 gen vs ref"
	@echo "  clean        清理编译产物"
	@echo "  setup-hdf5   从 apt 解压 HDF5 到 $(HDF5_BASE)（首次使用）"
