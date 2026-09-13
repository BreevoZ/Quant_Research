cmake_minimum_required(VERSION 3.15)
project(LobDataProcessor VERSION 1.0.0 LANGUAGES CXX)

# C++17标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(-Wall -Wextra -O3 -march=native)
endif()

# 查找依赖库
find_package(HDF5 REQUIRED COMPONENTS CXX)
find_package(Threads REQUIRED)

# ClickHouse C++客户端（需要手动安装或指定路径）
# 方法1：使用find_package（如果安装了clickhouse-cpp的CMake配置）
# find_package(clickhouse-cpp-lib REQUIRED)

# 方法2：手动指定头文件和库文件路径
set(CLICKHOUSE_INCLUDE_DIR "/usr/local/include" CACHE PATH "ClickHouse include directory")
set(CLICKHOUSE_LIB_DIR "/usr/local/lib" CACHE PATH "ClickHouse library directory")

# 方法3：找不到真正的 clickhouse-cpp 时，回退到 third_party/clickhouse 下的空实现桩。
# 此时只是"能编译能跑 H5 流程"，ClickHouse 写入是 no-op，不会真的落库。
if(EXISTS "${CLICKHOUSE_INCLUDE_DIR}/clickhouse/client.h")
    set(CLICKHOUSE_STUB OFF)
else()
    set(CLICKHOUSE_STUB ON)
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(CLICKHOUSE_INCLUDE_DIR "${REPO_ROOT}/third_party")
    message(WARNING
        "未找到 clickhouse-cpp，已回退到 third_party/clickhouse 空实现桩："
        "lob_processor 能编译、H5 流程正常，但 ClickHouse 写入是 no-op，不会真的落库。")
endif()

# 查找abseil库
find_package(absl CONFIG QUIET)
if(NOT absl_FOUND)
    message(STATUS "absl not found via find_package, will link manually")
    set(ABSL_LIBS
        absl_int128
        absl_strings
        absl_str_format_internal
        absl_raw_logging_internal
        absl_throw_delegate
        absl_base
        absl_spinlock_wait
    )
else()
    message(STATUS "Found absl via find_package")
    set(ABSL_LIBS
        absl::int128
        absl::strings
        absl::str_format
        absl::raw_logging_internal
        absl::throw_delegate
        absl::base
    )
endif()

# 设置RPATH，让可执行文件运行时能找到/usr/local/lib下的动态库
set(CMAKE_INSTALL_RPATH "/usr/local/lib")
set(CMAKE_BUILD_RPATH "/usr/local/lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HDF5_INCLUDE_DIRS}
    ${CLICKHOUSE_INCLUDE_DIR}
)

# 链接目录
link_directories(
    ${CLICKHOUSE_LIB_DIR}
)

# 源文件
set(SOURCES
    lob_data_processor.cpp
    main.cpp
)

# 头文件
set(HEADERS
    lob_data_processor.h
)

# 共享路径解析(qr/qr_paths.h):默认输入输出目录来自 qr.toml / QR_* 环境变量
include(${CMAKE_CURRENT_LIST_DIR}/../../qr/qr_paths.cmake)

# 生成可执行文件
add_executable(lob_processor ${SOURCES} ${HEADERS})
qr_use_paths(lob_processor)

# 链接库
# 用空实现桩时不需要 clickhouse/absl；libc++(mac) 也没有独立的 stdc++fs
if(CLICKHOUSE_STUB)
    set(CLICKHOUSE_LINK_LIBS )
else()
    set(CLICKHOUSE_LINK_LIBS clickhouse-cpp-lib cityhash zstd lz4 ${ABSL_LIBS})
endif()
if(APPLE)
    set(FS_LINK_LIBS )
else()
    set(FS_LINK_LIBS stdc++fs)  # C++17 filesystem库（GCC 8及以下需要）
endif()

target_link_libraries(lob_processor
    ${HDF5_LIBRARIES}
    ${HDF5_CXX_LIBRARIES}
    Threads::Threads
    ${CLICKHOUSE_LINK_LIBS}
    ${FS_LINK_LIBS}
)

# 安装规则
install(TARGETS lob_processor
    RUNTIME DESTINATION bin
)

# 打印配置信息
message(STATUS "")
message(STATUS "LOB Data Processor Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  HDF5 Include: ${HDF5_INCLUDE_DIRS}")
message(STATUS "  HDF5 Libraries: ${HDF5_LIBRARIES}")
message(STATUS "  ClickHouse Include: ${CLICKHOUSE_INCLUDE_DIR}")
message(STATUS "  ClickHouse Lib Dir: ${CLICKHOUSE_LIB_DIR}")
message(STATUS "")

# 可选：添加测试
enable_testing()
# add_subdirectory(tests)
