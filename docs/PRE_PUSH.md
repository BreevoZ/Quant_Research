# Pre-push checks · 提交前检查

[Repository / 全仓](../README.md) · [Setup / 环境](SETUP.md) · [Validation / 验证记录](VALIDATION.md)

## Inspect the candidate changes · 检查候选改动

Run from the repository root with conda `mx`. These commands do not download market data or push code.

在仓库根使用 conda `mx` 执行。以下命令不下载行情，也不推送代码。

```bash
conda activate mx
python scripts/check_repository.py
python scripts/check_push.py
python -m unittest discover -s scripts/tests -v
python -m unittest discover -s us_equity/tests -v
python ashare/datacheck/scripts/check_lob_drift.py
```

`check_push.py` inspects existing tracked files plus non-ignored untracked files. It also scans locally
reachable Git blobs, commit messages and annotated tags. It detects common token/private-key patterns,
private configuration filenames, conflict markers, nonportable symlinks and files of 50 MiB or more.
When a local Massive key file exists, its token values are compared against candidate and historical
content without printing the values. Files over the size limit are rejected without reading their payload.

`check_push.py` 检查现存已跟踪文件及未被忽略的新文件，同时扫描本地可达 Git 历史中的文件内容、
提交消息和附注标签。检查常见 token/私钥模式、本机配置文件名、冲突标记、不可迁移软链接及
不小于 50 MiB 的文件。若本机有 Massive 密钥文件，会对候选内容及历史进行密钥值匹配，
但不打印密钥。超限文件直接拒绝，不读取其内容。

This is a heuristic guard, not proof that every possible secret format is absent. It does not inspect
the remote server's state, permissions, branch protection, or objects unavailable in the local history.

这是启发式检查，不能证明所有形式的秘密都不存在；也不检查远端状态、推送权限、分支保护，
或本地历史中不存在的对象。

## Review what Git will commit · 查看实际暂存内容

The reorganization includes new files and moves across directories. Use `git add -A` after reviewing
the working tree; `git commit -am` alone would omit untracked files, including the new market entry points.

本次整理包含新文件及跨目录移动。查看工作区后使用 `git add -A`；仅使用 `git commit -am`
会漏掉未跟踪文件，包括新的市场入口。

```bash
git status --short
git add -A
python scripts/check_push.py --staged
git diff --cached --check
git diff --cached --stat
git diff --cached --name-status -M
# Review the actual content before committing / 提交前查看实际内容
git diff --cached
```

The `--staged` mode reads Git index blobs, so a secret staged earlier is still detected after it has
been removed only from the working file. Files already committed remain in history even if later deleted
or ignored. Do not bypass a failed check; inspect the reported path and rule first.

`--staged` 读取 Git 索引中的实际内容：即使工作文件已清理，先前暂存的密钥仍会被发现。
已提交过的内容在删除文件或补写 ignore 后仍存在于历史。检查失败时先查看报告的路径与规则。

Raw datasets, local `KEYS.txt` files, `.env`/`.env.*`, `qr.toml`, build output and new US runs are ignored.
Dummy `.env.example` and `.env.template` files may be committed. Existing A-share research CSV/PNG
snapshots under `ashare/l3_factor/bt/` are intentionally retained as historical evidence; inspect newly
generated A-share results before adding them. The LOB drift baseline stores literal diff context, so its
whitespace is preserved through a file-specific `.gitattributes` exception.

原始数据、本机密钥、dotenv 配置、`qr.toml`、编译产物及新美股运行结果均被忽略；
只含占位值的 `.env.example` / `.env.template` 可以提交。`ashare/l3_factor/bt/` 中已有的
CSV/PNG 研究快照有意保留为历史证据，新产生的 A 股结果在添加前需检查。
盘口漂移基线保存 diff 上下文，`.gitattributes` 仅对此文件豁免空白检查，保留基线原貌。

After review, commit the staged changes and push to your configured upstream. This audit does not
stage, commit, rewrite history or push on your behalf.

查看暂存差异后，再提交并推送至配置好的上游。本检查不会替你暂存、提交、重写历史或推送。
