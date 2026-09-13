cmake_minimum_required(VERSION 3.14)
project(datacheck LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(Threads REQUIRED)

# ── HDF5 ──────────────────────────────────────────────────────────────────────
# 优先用系统 HDF5:macOS `brew install hdf5`,WSL/Linux `apt install libhdf5-dev`。
# 找不到才退回仓库根 third_party/hdf5(三个 C++ 项目共用的一份本地化依赖)。
# 注意 third_party/hdf5/lib 里的 .so 是指向 *.so.103.0.0 的软链接, 目标未随仓库带过来,
# 所以那条回退路径只有在你自己补齐实体库时才可用 —— 正常情况装系统包即可。
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(HDF5 QUIET COMPONENTS C CXX)
if(HDF5_FOUND)
  message(STATUS "HDF5: 使用系统安装 ${HDF5_VERSION}")
  set(HDF5_INC   ${HDF5_INCLUDE_DIRS})
  set(HDF5_LIBS  ${HDF5_CXX_LIBRARIES} ${HDF5_C_LIBRARIES})
  set(HDF5_RPATH )
else()
  set(HDF5_DEPS ${REPO_ROOT}/third_party/hdf5)
  set(HDF5_INC  ${HDF5_DEPS}/include)
  set(HDF5_LIB  ${HDF5_DEPS}/lib)
  set(HDF5_LIBS ${HDF5_LIB}/libhdf5_serial.so ${HDF5_LIB}/libhdf5_cpp.so)
  set(HDF5_RPATH -Wl,-rpath,${HDF5_LIB})
  message(WARNING "未找到系统 HDF5, 回退到 ${HDF5_DEPS};"
                  "若链接报找不到 libhdf5_serial.so.103 请先装 libhdf5-dev")
endif()

# ── datacheck ─────────────────────────────────────────────────────────────────
add_executable(datacheck
    datacheck_main.cpp
    lob_builder.cpp
)
target_include_directories(datacheck PRIVATE ${HDF5_INC})
target_link_libraries(datacheck Threads::Threads ${HDF5_LIBS})
target_link_options(datacheck PRIVATE ${HDF5_RPATH})

# ── lob_compare ───────────────────────────────────────────────────────────────
add_executable(lob_compare lob_compare.cpp)
target_include_directories(lob_compare PRIVATE ${HDF5_INC})
target_link_libraries(lob_compare Threads::Threads ${HDF5_LIBS})
target_link_options(lob_compare PRIVATE ${HDF5_RPATH})

# ── lob_builder 漂移检查 ──────────────────────────────────────────────────────
# 本目录是 lob_builder 的基准实现, vorder_sim 与 auction 各有一份副本。
# 这个测试把「当前已知的差异」和基线比对, 出现新差异就失败, 提醒三处同步。
# 不需要任何外部数据, 只要有 python3。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    enable_testing()
    add_test(NAME lob_builder_drift
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_LIST_DIR}/scripts/check_lob_drift.py)
else()
    message(STATUS "未找到 python3, 跳过 lob_builder 漂移检查测试")
endif()
