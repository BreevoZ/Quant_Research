#pragma once
/**
 * 仓库内 C++ 侧数据路径的唯一解析入口,与 qr/paths.py 同源。
 *
 * 解析顺序与 Python 完全一致:
 *   环境变量 QR_<KEY> → 仓库根的 qr.toml 的 [paths] 段 → 内置默认值
 * 相对路径一律锚定**仓库根**,不是当前工作目录,所以从哪个目录调用都一样。
 *
 * 用法:
 *     #include "qr_paths.h"
 *     std::string tl = qr::get("tl_zip");                  // 未配置就用默认值
 *     std::string out = qr::get("lob_root");
 *
 * header-only,只依赖标准库 (C++17 <filesystem>)。CMake 里加:
 *     target_include_directories(<target> PRIVATE ${REPO_ROOT}/qr)
 *     target_compile_definitions(<target> PRIVATE QR_REPO_ROOT_FALLBACK="${REPO_ROOT}")
 *
 * 改路径不要改代码:复制 qr.example.toml 成 qr.toml 后改配置,
 * 或临时用环境变量,例如 QR_LOB_ROOT=/Volumes/ext/Lob_new。
 */

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace qr {

namespace detail {

/// 内置默认值。相对路径锚定仓库根。必须与 qr/paths.py 的 DEFAULTS 保持一致。
inline const std::map<std::string, std::string>& defaults() {
    static const std::map<std::string, std::string> kDefaults = {
        {"data", "ashare/l3_factor/data"},
        {"bt", "ashare/l3_factor/bt"},
        {"lob_root", "data/Lob_new"},
        {"feat_root", "data/factor/features"},
        {"res_root", "data/factor/results"},
        {"crypto_data", "data/massive/Crypto_MIN"},
        {"crypto_feat", "data/factor/crypto_features"},
        {"crypto_ic", "data/factor/crypto_ic"},
        {"us_minute", "data/massive/unified/flatfiles/stocks/minute_aggs_v1"},
        {"us_research", "data/us_equity"},
        {"tick3s_ref", "data/tick_3s"},
        {"tick3s_bin", "data/tick_3s_staging"},
        {"tl_zip", "data/ashare/TL"},
        {"flow_root", "data/Flow_TL"},
        {"auction_mx", "data/Level2_MX"},
        {"lob_h5", "data/Lob_local"},
        {"lob_level2", "data/Level2"},
        {"lob_3s", "data/Lob_new_3s"},
        {"tick3s_parquet", "data/Lob_new_3s_MX"},
        {"vsim", "ashare/vorder_sim/build/vsim"},
    };
    return kDefaults;
}

inline std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

inline std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

/// 仓库根的判定标志。两个都在才算,避免走到无关的上级目录。
inline bool looks_like_repo_root(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / "pyproject.toml", ec) &&
           std::filesystem::exists(dir / "qr" / "paths.py", ec);
}

/// 只解析 qr.toml 里 `key = "value"` 这一种形式,够用且不引第三方 TOML 库。
/// 与 Python 端一致:有 [paths] 段就只取该段,没有就取顶层。
inline std::map<std::string, std::string> parse_config(const std::filesystem::path& file) {
    std::map<std::string, std::string> top, paths;
    bool saw_paths = false;
    std::ifstream fh(file);
    if (!fh) return {};

    std::string line, section;
    while (std::getline(fh, line)) {
        // 去掉引号外的行内注释
        bool in_quote = false;
        char quote = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (in_quote) {
                if (c == quote) in_quote = false;
            } else if (c == '"' || c == '\'') {
                in_quote = true;
                quote = c;
            } else if (c == '#') {
                line = line.substr(0, i);
                break;
            }
        }
        line = trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            const auto close = line.find(']');
            section = (close == std::string::npos) ? "" : trim(line.substr(1, close - 1));
            if (section == "paths") saw_paths = true;
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front()) {
            val = val.substr(1, val.size() - 2);
        } else {
            continue;  // 非字符串值(数字/数组/表)不是路径,跳过
        }
        if (key.empty()) continue;
        if (section == "paths") paths[key] = val;
        else if (section.empty()) top[key] = val;
        // 其它段(如未来的 [build])与路径无关,忽略
    }
    return saw_paths ? paths : top;
}

}  // namespace detail

/// 仓库根目录。QR_REPO_ROOT → 从当前目录逐级上找标志文件 → 构建时写入的兜底。
inline std::filesystem::path repo_root() {
    static const std::filesystem::path kRoot = []() -> std::filesystem::path {
        if (const char* env = std::getenv("QR_REPO_ROOT")) {
            if (*env) return std::filesystem::path(env);
        }
        std::error_code ec;
        auto dir = std::filesystem::current_path(ec);
        if (!ec) {
            for (; !dir.empty(); dir = dir.parent_path()) {
                if (detail::looks_like_repo_root(dir)) return dir;
                if (dir == dir.parent_path()) break;
            }
        }
#ifdef QR_REPO_ROOT_FALLBACK
        return std::filesystem::path(QR_REPO_ROOT_FALLBACK);
#else
        throw std::runtime_error(
            "找不到仓库根(向上没看到 pyproject.toml + qr/paths.py)。"
            "用 QR_REPO_ROOT 指定,或在仓库内运行。");
#endif
    }();
    return kRoot;
}

/// 已解析的 qr.toml。文件不存在就是空表,不报错。
inline const std::map<std::string, std::string>& config() {
    static const std::map<std::string, std::string> kConfig = [] {
        std::error_code ec;
        const auto file = repo_root() / "qr.toml";
        return std::filesystem::is_regular_file(file, ec)
                   ? detail::parse_config(file)
                   : std::map<std::string, std::string>{};
    }();
    return kConfig;
}

/**
 * 解析一个路径键,返回绝对路径字符串。
 *
 * 未知键抛 std::runtime_error,错误信息里列出所有已知键。
 */
inline std::string get(const std::string& key) {
    std::string raw;
    if (const char* env = std::getenv(("QR_" + detail::to_upper(key)).c_str())) {
        raw = env;
    }
    if (raw.empty()) {
        const auto it = config().find(key);
        if (it != config().end()) raw = it->second;
    }
    if (raw.empty()) {
        const auto it = detail::defaults().find(key);
        if (it == detail::defaults().end()) {
            std::string known;
            for (const auto& kv : detail::defaults()) {
                if (!known.empty()) known += ", ";
                known += kv.first;
            }
            throw std::runtime_error("未知路径键 '" + key + "';已知的有:" + known);
        }
        raw = it->second;
    }

    std::filesystem::path path(raw);
    if (!path.is_absolute()) path = repo_root() / path;
    return path.lexically_normal().string();
}

/// 读一个非路径的配置项(如数据库口令),只看环境变量。未设置时返回 fallback。
inline std::string env_or(const char* name, const std::string& fallback = {}) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

}  // namespace qr
