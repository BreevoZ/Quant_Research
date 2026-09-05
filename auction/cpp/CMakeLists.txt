cmake_minimum_required(VERSION 3.10)
project(AuctionLobBuilder VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
option(WITH_HDF5 "Enable HDF5 output support" ON)
option(WITH_OPENMP "Enable OpenMP parallel support" ON)
option(WITH_LTO "Enable Link Time Optimization" ON)

# 优化选项
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# LTO支持
if(WITH_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO enabled")
    else()
        message(WARNING "LTO not supported: ${lto_error}")
    endif()
endif()

# OpenMP 支持 (可选)
if(WITH_OPENMP)
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
        message(STATUS "OpenMP enabled")
    else()
        message(WARNING "OpenMP requested but not found")
    endif()
endif()

# Auction (集合竞价) 主程序
add_executable(auction_lob_main
    auction_lob_main.cpp
    auction_lob_builder.cpp
)

# std::filesystem: 只有 GCC 9 以前需要单独链 libstdc++fs。
# GCC 9+ 与 clang/libc++ 都内建, 无条件链会在 macOS 上报 library 'stdc++fs' not found。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(auction_lob_main stdc++fs)
endif()

# OpenMP 链接
if(WITH_OPENMP AND OpenMP_CXX_FOUND)
    target_link_libraries(auction_lob_main OpenMP::OpenMP_CXX)
endif()

# HDF5 支持 (auction_lob_builder.cpp 复用了 tl 的 writeH5, 需链接 HDF5)
# 优先用系统 HDF5:macOS `brew install hdf5`,WSL/Linux `apt install libhdf5-dev`。
# 找不到才退回仓库根 third_party/hdf5(与 datacheck/vorder_sim 共用同一份)。
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(WITH_HDF5)
    set(HDF5_DEPS ${REPO_ROOT}/third_party/hdf5)
    set(HDF5_LIB  ${HDF5_DEPS}/lib)
    find_package(HDF5 QUIET COMPONENTS C CXX)
    if(HDF5_FOUND)
        message(STATUS "HDF5: 使用系统安装 ${HDF5_VERSION}")
        set(AUC_HDF5_INC  ${HDF5_INCLUDE_DIRS})
        set(AUC_HDF5_LIBS ${HDF5_CXX_LIBRARIES} ${HDF5_C_LIBRARIES})
        set(AUC_HDF5_RPATH )
    elseif(EXISTS ${HDF5_LIB}/libhdf5_serial.so AND EXISTS ${HDF5_LIB}/libhdf5_cpp.so)
        message(STATUS "HDF5: 回退到 ${HDF5_DEPS}")
        set(AUC_HDF5_INC  ${HDF5_DEPS}/include)
        set(AUC_HDF5_LIBS ${HDF5_LIB}/libhdf5_serial.so ${HDF5_LIB}/libhdf5_cpp.so)
        set(AUC_HDF5_RPATH -Wl,-rpath,${HDF5_LIB})
    else()
        message(WARNING "未找到 HDF5(系统包与 ${HDF5_DEPS} 都没有), H5 输出将被禁用;"
                        "装 libhdf5-dev 或 brew install hdf5 后重新配置")
    endif()

    if(AUC_HDF5_LIBS)
        target_compile_definitions(auction_lob_main PRIVATE HAS_HDF5)
        target_include_directories(auction_lob_main PRIVATE ${AUC_HDF5_INC})
        # -Wl,--no-as-needed: LTO(CMAKE_INTERPROCEDURAL_OPTIMIZATION) 开启时, 链接器对
        # 整程序做可达性分析, 会错误判定 writeH5() 里对 libhdf5 的调用不可达而整体丢弃这两个
        # .so 的链接(实测: Release+LTO 下 ldd 看不到 libhdf5, HAS_HDF5 相关符号被裁掉;
        # 关闭 LTO 或加 --no-as-needed 后恢复正常)。强制保留, 不依赖可达性判断。
        # 这两个 flag 是 GNU ld 专有, mac 的 ld 不认, 故只在非 APPLE 上加。
        if(APPLE)
            target_link_libraries(auction_lob_main ${AUC_HDF5_LIBS})
        else()
            target_link_libraries(auction_lob_main
                -Wl,--no-as-needed ${AUC_HDF5_LIBS} -Wl,--as-needed)
        endif()
        target_link_options(auction_lob_main PRIVATE ${AUC_HDF5_RPATH})
    endif()
endif()

install(TARGETS auction_lob_main DESTINATION bin)
