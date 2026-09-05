cmake_minimum_required(VERSION 3.14)
project(vorder_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(Threads REQUIRED)

# 复用 datacheck 的 LOB 引擎(含 H5 写出)
# ── HDF5 ──────────────────────────────────────────────────────────────────────
# 优先用系统 HDF5:macOS `brew install hdf5`,WSL/Linux `apt install libhdf5-dev`。
# 找不到才退回仓库根 third_party/hdf5(三个 C++ 项目共用的一份本地化依赖)。
# 注意 third_party/hdf5/lib 里的 .so 是指向 *.so.103.0.0 的软链接, 目标未随仓库带过来,
# 所以那条回退路径只有在你自己补齐实体库时才可用 —— 正常情况装系统包即可。
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
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

add_executable(vsim
    vsim_main.cpp
    lob_builder.cpp
)
target_include_directories(vsim PRIVATE ${HDF5_INC})
target_link_libraries(vsim Threads::Threads ${HDF5_LIBS})
target_link_options(vsim PRIVATE ${HDF5_RPATH})

# ── 测试 ──────────────────────────────────────────────────────────────────────
# 两层验证, 都由 ctest 驱动:
#   vsim_test  内存里合成 UnifiedRecord 直喂 Builder, 覆盖排队/成交/竞价的场景与随机不变量
#   handcheck  手写 Flow CSV 走完整解析链路, 与人工推演出的 expected.csv 对答案
# handcheck 是唯一穿过真实 Flow 解析器的一层(列名映射/norm()/时间戳解析), 两层不可互相替代。
enable_testing()

add_executable(vsim_test
    vsim_test.cpp
    lob_builder.cpp
)
target_include_directories(vsim_test PRIVATE ${HDF5_INC})
target_link_libraries(vsim_test Threads::Threads ${HDF5_LIBS})
target_link_options(vsim_test PRIVATE ${HDF5_RPATH})

add_test(NAME vsim_test COMMAND vsim_test)

# run.sh 默认找 ./build/vsim, 这里显式把刚构建出的二进制传进去,
# 于是 build 目录叫什么名字、在不在源码树里都无所谓。
add_test(NAME handcheck
         COMMAND ${CMAKE_COMMAND} -E env VSIM=$<TARGET_FILE:vsim>
                 bash ${CMAKE_CURRENT_LIST_DIR}/handcheck/run.sh)
