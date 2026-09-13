cmake_minimum_required(VERSION 3.14)
project(tl_to_flow_csv VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        add_compile_options(/O2)
    else()
        add_compile_options(-O3 -march=native)
    endif()
endif()

# 共享路径解析(qr/qr_paths.h):默认输入输出目录来自 qr.toml / QR_* 环境变量
include(${CMAKE_CURRENT_LIST_DIR}/../../qr/qr_paths.cmake)

add_executable(tl_to_flow_csv tl_to_flow_csv.cpp)
qr_use_paths(tl_to_flow_csv)
install(TARGETS tl_to_flow_csv RUNTIME DESTINATION bin)
