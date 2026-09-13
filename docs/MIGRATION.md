# Directory migration · 目录迁移

[Repository / 全仓](../README.md) · [Setup / 环境](SETUP.md) · [Architecture / 结构](ARCHITECTURE.md)

## Source paths · 源码路径

| Previous / 原位置 | Current / 新位置 |
|---|---|
| `tltoflow/` | `ashare/tltoflow/` |
| `datacheck/` | `ashare/datacheck/` |
| `auction/` | `ashare/auction/` |
| `build_tick3s/` | `ashare/build_tick3s/` |
| `vorder_sim/` | `ashare/vorder_sim/` |
| `l3_factor/` | `ashare/l3_factor/` |
| `factor/ashare/` | `ashare/factor/` |
| `factor/crypto/` | `archive/crypto_minute/` |
| `factor/README.md` | `archive/factor_research_notes.zh.md` (historical notes / 历史记录) |
| `massive_down/` | `data_sources/massive/` |
| `us_equity/` | Unchanged / 不变 |

Directory moves preserve existing source changes. There are no compatibility symlinks at the old
top-level paths; commands and imports should use the new locations. `python -m us_equity` and
`from qr import paths` remain unchanged.

目录移动保留已有源码修改。旧顶层路径不保留兼容软链接，命令使用新位置；
`python -m us_equity` 和 `from qr import paths` 保持不变。

## Local data and secrets · 本地数据与密钥

On the reorganized machine, the following local directories moved with their contents:

本次整理的机器上，以下本地目录连同内容移动：

| Previous / 原位置 | Current / 新位置 |
|---|---|
| `TL/` | `data/ashare/TL/` |
| `_archive/` | `data/local_archives/` |
| `l3_factor/data/` and `l3_factor/bt/` | `ashare/l3_factor/data/` and `ashare/l3_factor/bt/` |
| `massive_down/KEYS.txt` | `data_sources/massive/KEYS.txt` (still ignored / 仍不入库) |

Git does not relocate ignored local data on other machines. After updating a checkout, either keep
those datasets at their old locations and configure absolute paths in `qr.toml`, or move them to the
new locations after checking for destination conflicts. Do not overwrite an existing destination.
Old shell batch scripts with project-relative paths may also need their local data layout restored.

Git 不会替其他机器迁移被忽略的本地数据。更新代码后，可以保留旧数据位置并在 `qr.toml` 中写绝对路径，
也可以先检查目标目录是否冲突，再移动到新位置；不要覆盖已有目录。
使用项目相对路径的旧 Shell 批跑脚本还需确认其本地数据布局。

The downloader keeps its CLI and file formats; only its source directory changed. Update any `--keys`
argument or scheduler entry that mentions `massive_down`. Credentials were moved without inspecting
their contents and remain excluded by `.gitignore`.

下载器 CLI 与文件格式不变，只改变源码目录。涉及 `massive_down` 的 `--keys` 参数和定时任务路径需更新。
密钥仅随目录搬迁，不读取内容，仍由 `.gitignore` 排除。

The subsequent pre-push audit compares local key values against Git candidates and history without
displaying them; see [Pre-push checks](PRE_PUSH.md).

后续提交前审查会在本地匹配密钥值与候选文件、历史内容，不显示密钥；见[提交前检查](PRE_PUSH.md)。

## Existing configuration · 已有配置

The default changes are `data`, `bt`, `vsim` and `tl_zip`; see [the template](../qr.example.toml).
Explicit `qr.toml` entries and `QR_*` environment variables still take precedence and are not rewritten
automatically. Other reconstructed-data defaults remain unchanged.

默认值变化涉及 `data`、`bt`、`vsim`、`tl_zip`，见[配置模板](../qr.example.toml)。
已有 `qr.toml` 与 `QR_*` 环境变量仍优先，不会被自动改写；其余重建数据默认值保持原样。

Rebuild CMake projects at the new paths. Old `CMakeCache.txt` files refer to their original source
locations. Preserve them outside the active build directory if needed, then configure a fresh build.

按新路径重新构建 CMake 项目。旧 `CMakeCache.txt` 指向原源码目录；需要保留时先移到构建目录之外，
再创建新构建。

## Environment and checks · 环境与检查

```bash
conda activate mx
python -m pip install -e ".[us,massive]"
python scripts/check_repository.py
python -m unittest discover -s us_equity/tests -v
python ashare/datacheck/scripts/check_lob_drift.py
```

The earlier local `.venv` is not required and is no longer the documented default. It was left intact;
all validation for this reorganization uses conda `mx` for Python.

之前创建的本地 `.venv` 不再是默认环境，也不再需要；它保留原样。
本次整理后的 Python 验证均使用 conda `mx`。

Generated `*.egg-info/` package metadata is no longer part of the source tree. Editable installation
can regenerate it; it is ignored together with download and build-package output directories.

生成的 `*.egg-info/` 包元数据不再作为源码保存；editable 安装可重新生成，
并与下载目录、构建打包产物一起由 Git 忽略。
