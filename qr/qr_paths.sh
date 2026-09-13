# shellcheck shell=bash
#
# 给 shell 脚本用的路径解析,与 qr/paths.py、qr/qr_paths.h 同源同优先级:
#     环境变量 QR_<KEY> → 仓库根 qr.toml 的 [paths] → 内置默认值
#
# 用法:
#     source "$(dirname "${BASH_SOURCE[0]}")/../qr/qr_paths.sh"
#     tl_root=$(qr_path tl_zip)
#
# 默认值只有 qr/paths.py 一份,这里通过它解析,不再复制一遍。
# 未安装 qr 包也能用:临时把仓库根加进 PYTHONPATH。

qr_repo_root() {
    if [[ -n ${QR_REPO_ROOT:-} ]]; then
        printf '%s' "$QR_REPO_ROOT"
        return
    fi
    # BASH_SOURCE 只有 bash 有。zsh 下为空会解析到错误的目录, 所以下面要校验。
    local self=${BASH_SOURCE[0]:-$0} dir
    dir=$(cd -- "$(dirname -- "$self")/.." 2>/dev/null && pwd)
    if [[ -z $dir || ! -f $dir/qr/paths.py ]]; then
        echo "qr_paths.sh: 找不到仓库根(从 '$self' 推得 '$dir')。" >&2
        echo "  用 bash 运行, 或先 export QR_REPO_ROOT=/path/to/Quant_Research" >&2
        return 1
    fi
    printf '%s' "$dir"
}

# 未安装 qr 包时, 让 source 之后的所有 python 调用都能 import qr。
# 已安装(pip install -e .)时这一行也无害。
QR_REPO_ROOT="${QR_REPO_ROOT:-$(qr_repo_root)}"
export QR_REPO_ROOT
export PYTHONPATH="$QR_REPO_ROOT${PYTHONPATH:+:$PYTHONPATH}"

qr_path() {
    local key=$1 root
    if [[ -z $key ]]; then
        echo "qr_path: 需要一个路径键, 例如 qr_path tl_zip" >&2
        return 2
    fi
    root=$(qr_repo_root)
    PYTHONPATH="$root${PYTHONPATH:+:$PYTHONPATH}" \
        "${PYTHON:-python3}" -c 'import sys; from qr import paths; print(paths.get(sys.argv[1]))' "$key"
}
