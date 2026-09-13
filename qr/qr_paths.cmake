# 把仓库共享的路径解析头 qr/qr_paths.h 接到一个 C++ target 上。
#
# 用法(在各子项目的 CMakeLists.txt 里):
#     include(${CMAKE_CURRENT_LIST_DIR}/../../qr/qr_paths.cmake)
#     qr_use_paths(my_target)
#
# QR_REPO_ROOT_FALLBACK 是构建时写进二进制的仓库根,只在运行时找不到仓库根
# (二进制被拷到别处、cwd 在仓库外且没设 QR_REPO_ROOT)时兜底。

get_filename_component(_qr_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(QR_REPO_ROOT "${_qr_repo_root}" CACHE INTERNAL "Quant_Research 仓库根")

function(qr_use_paths target)
    target_include_directories(${target} PRIVATE "${QR_REPO_ROOT}/qr")
    target_compile_definitions(${target} PRIVATE
        QR_REPO_ROOT_FALLBACK="${QR_REPO_ROOT}")
endfunction()
