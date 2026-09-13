# Setup · 环境配置

[Repository / 全仓](../README.md) · [A-shares / A 股](../ashare/README.md) ·
[US equities / 美股](../us_equity/README.md)

## Existing conda environment · 复用现有环境

Use `mx` for this repository. The inspected local environment has Python 3.12 and the core scientific
stack. The US calendar dependency is included in the `us` extra. There is no need to create a new venv.

本仓库优先使用 `mx`。已检查的本机环境包含 Python 3.12 和核心科学计算依赖，
美股日历依赖由 `us` extra 提供，无需再创建 venv。

```bash
conda activate mx
python -m pip install -e ".[us,massive]"
python -c "import sys; print(sys.executable)"
```

In scripts or shells without activation, prefix commands with `conda run -n mx`.
This selects the intended interpreter explicitly.

脚本或未激活环境的终端可加 `conda run -n mx`，明确选择解释器：

```bash
conda run -n mx python -m us_equity demo
conda run -n mx python ashare/datacheck/scripts/check_lob_drift.py
```

Optional extras / 可选依赖：

| Install / 安装 | Purpose / 用途 |
|---|---|
| `python -m pip install -e .` | Shared scientific dependencies / 共享科学计算依赖 |
| `python -m pip install -e ".[us,massive]"` | US research and downloader / 美股研究与下载器 |
| `python -m pip install -e ".[ml]"` | A-share ML combinations / A 股机器学习组合 |
| `python -m pip install -e ".[explore]"` | Historical astronomy placebo work / 历史天文安慰剂研究 |

## New machine · 新机器

Run from the repository root. `environment.yml` names the environment `mx`; an existing environment
with that name should be reused using the instructions above. To create a differently named environment,
pass `--name quant-research` to `conda env create` and activate that name instead.

在仓库根执行。`environment.yml` 默认环境名为 `mx`；已有同名环境时按上节复用。
如需其他名字，在创建命令后加 `--name quant-research`，之后激活对应名字。

```bash
conda env create -f environment.yml
conda activate mx
```

Python 3.11+ is required; the recipe selects Python 3.12. Editable installation makes `qr` and
`us_equity` importable outside the repository. The older A-share projects remain script entry points.

需要 Python 3.11+，环境模板选择 3.12。editable 安装后，仓库外也能导入 `qr` 和 `us_equity`；
旧 A 股项目继续以脚本形式运行。

## C++ prerequisites · C++ 依赖

The A-share programs use C++17, CMake and system HDF5. The US minute pipeline does not need a C++ build.
Use the same compiler/HDF5 architecture throughout a build (for example, arm64 on Apple Silicon).

A 股程序使用 C++17、CMake 和系统 HDF5；美股分钟研究不需要构建 C++。
编译器与 HDF5 的架构应一致，例如 Apple Silicon 使用 arm64。

```bash
# macOS
brew install cmake hdf5

# Ubuntu / WSL
sudo apt install build-essential cmake libhdf5-dev
```

If CMake cannot locate an installed HDF5, pass its prefix using `-DCMAKE_PREFIX_PATH=/path/to/hdf5`.
Conda Python and a system C++ toolchain can coexist; Python's `h5py` does not by itself prove that
CMake can locate HDF5 headers and libraries.

CMake 找不到已安装 HDF5 时，可传 `-DCMAKE_PREFIX_PATH=/path/to/hdf5`。
conda Python 可以配合系统 C++ 工具链；Python 能导入 `h5py`，不代表 CMake 一定能找到 HDF5 开发文件。

The `third_party/hdf5` copy is a historical fallback, not a portable binary distribution.
The ClickHouse fallback is a **no-op stub**, sufficient for local H5 processing but not database writes.
Use CMake for portable builds; the old `build_tick3s/Makefile` targets a Linux HDF5 layout.

`third_party/hdf5` 是历史回退，不是跨平台二进制发行包。ClickHouse 回退是**空实现桩**，
可以验证本地 H5 流程，但不会写入数据库。跨平台优先使用 CMake；旧 Makefile 面向 Linux HDF5 布局。

## Data paths · 数据路径

Create `qr.toml` from [the template](../qr.example.toml) only when no local file exists.
Keep existing external-disk settings. The order is `QR_<KEY>` → `qr.toml` → defaults;
relative paths are always resolved from the repository root.

没有 `qr.toml` 时才从[模板](../qr.example.toml)创建；已有外部数据盘配置请保留。
优先级为 `QR_<KEY>` → `qr.toml` → 默认值，相对路径始终锚定仓库根。

```bash
python -c "from qr import paths; print(paths.get('data')); print(paths.get('us_minute'))"
```

The downloader reads credentials from a file selected by `--keys`; it does not read API keys from
environment variables. Keep that file at the ignored `data_sources/massive/KEYS.txt`, or outside the
repository. See the [downloader guide](../data_sources/massive/README.md) for its format.

下载器通过 `--keys` 指定的文件读取密钥，不读取 API 密钥环境变量。
密钥文件放在已忽略的 `data_sources/massive/KEYS.txt`，或仓库外；格式见[下载器说明](../data_sources/massive/README.md)。

## Offline validation · 离线验证

```bash
conda activate mx
python scripts/check_repository.py
python scripts/check_push.py
python -m unittest discover -s scripts/tests -v
python -m unittest discover -s us_equity/tests -v
python -m us_equity demo
cmake -S ashare/vorder_sim -B ashare/vorder_sim/build
cmake --build ashare/vorder_sim/build -j4
ctest --test-dir ashare/vorder_sim/build --output-on-failure
python ashare/datacheck/scripts/check_lob_drift.py
```

These validate software and source contracts without downloading market data. Full A-share production
runs and US strategy conclusions require separate validation on real data.

这些检查无需下载行情，验证的是软件与数据口径。A 股生产链路和美股策略结论需另行使用真实数据验证。

Before committing, also follow the [staged-change review](PRE_PUSH.md).
提交前另按[提交说明](PRE_PUSH.md)核对暂存内容。
