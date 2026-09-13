# third_party · Shared build dependencies / 共享构建依赖

[Repository / 全仓](../README.md) · [A-shares / A 股](../ashare/README.md) · [Setup / 环境](../docs/SETUP.md)

## English

System HDF5 is preferred by the A-share CMake projects. The bundled HDF5 headers and Linux library
links are a historical fallback; some link targets are absent and the binaries are not portable to
macOS. Install system HDF5 with `brew install hdf5` or `sudo apt install libhdf5-dev`.

`clickhouse/` contains no-op client stubs for local compilation. They do not write to a database.
`ashare/build_tick3s/Makefile` retains its older Linux `/tmp/hdf5_extracted/usr` convention;
use the documented CMake path for portable builds.

Regular A-share components reach the repository root through `../..`; `ashare/auction/cpp` uses
`../../..`. All builds must be checked after shared dependency changes.

## 中文

# third_party — 本地化的第三方依赖

这里放**不是本仓库写的、但构建时需要的**头文件与库。整合前每个 C++ 项目各存一份，
三份 HDF5 逐字节相同（含软链接），合计 342 个重复头文件。现在只有一份。

| 目录 | 内容 | 谁在用 |
|---|---|---|
| `hdf5/` | HDF5 1.10 头文件 + Linux 动态库 | `datacheck`、`vorder_sim`、`auction` |
| `clickhouse/` | clickhouse-cpp 的空实现桩头 | `build_tick3s` |

## HDF5 只是回退

三个项目的 CMake 都先 `find_package(HDF5)` 找系统安装，找不到才用这里的一份。
正常情况装系统包即可，这个目录不会被碰到：

```bash
brew install hdf5              # macOS
sudo apt install libhdf5-dev   # WSL / Linux
```

`hdf5/lib/` 里是 **Linux ELF**，而且 `libhdf5_serial.so` 等几个是指向 `*.so.103.0.0`
的软链接，目标文件没有随仓库带过来。所以回退路径只有在你自己补齐实体库时才可用。
留着它是为了保住这份头文件与当年服务器版本的对应关系。

`build_tick3s` 是例外，它的 `Makefile` 从 `/tmp/hdf5_extracted/usr` 取 HDF5，
用 `make setup-hdf5` 从系统 deb 解压，可以用 `make HDF5_BASE=/your/path` 覆盖。

## 引用方式

各项目的 CMakeLists 用 `CMAKE_CURRENT_LIST_DIR` 往上找仓库根，不依赖调用时的工作目录：

```cmake
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)   # ashare/auction/cpp 用 ../../..
find_package(HDF5 QUIET COMPONENTS C CXX)
if(NOT HDF5_FOUND)
  set(HDF5_DEPS ${REPO_ROOT}/third_party/hdf5)
endif()
```

改动这里之前先确认三个 C++ 项目都还能编。
