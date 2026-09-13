#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "qr_paths.h"

namespace {

constexpr int64_t kIdMultiplier = 1000000000000LL;
// 默认路径由 qr/qr_paths.h 解析:环境变量 QR_TL_ZIP / QR_FLOW_ROOT → qr.toml → 内置默认。
// 用函数而非全局常量,避免静态初始化期抛异常时拿不到错误信息。
const std::string& defaultTlRoot() {
    static const std::string v = qr::get("tl_zip");
    return v;
}
const std::string& defaultOutputRoot() {
    static const std::string v = qr::get("flow_root");
    return v;
}

struct Options {
    std::string date;
    std::string input_dir;
    std::string output_file;
    std::string output_instruments_xml;
    std::string instruments_template;
    std::string market;
    std::string symbol;
    std::string symbol_prefix_filter_text;
    std::string start_time;
    std::string end_time;
    std::string tl_root = defaultTlRoot();
    std::string output_root = defaultOutputRoot();
    std::string sz_ordtype_map_text = "49:1,85:U,default:2";
    std::string sh_price_type = "0";
    std::string sort_key = "appseq";
    std::string sort_tmp_dir;
    bool keep_temp = false;
    std::string missing_policy = "fail";
    std::string number_format = "compact";
    std::string sh_turnover = "raw";
    std::string sh_trade_dir_mode = "zero";
    std::string sz_trade_turnover = "raw";
    std::string sz_cancel_turnover = "raw";
    std::map<std::string, std::string> sz_ordtype_map;
    std::map<std::string, std::vector<std::string>> symbol_prefix_filter;
};

static Options g_options;

struct FlowRow {
    std::string line;
    std::string instrument;
    int local_key = 0;
    int exchange_key = 0;
    int64_t seq_no = 0;
    int source_order = 0;
};

static std::string trim(std::string_view v) {
    size_t b = 0;
    size_t e = v.size();
    while (b < e && std::isspace(static_cast<unsigned char>(v[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1]))) --e;
    return std::string(v.substr(b, e - b));
}

static std::vector<std::string_view> split_csv_line(std::string_view line) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos <= line.size()) {
        size_t comma = line.find(',', pos);
        if (comma == std::string_view::npos) {
            out.emplace_back(line.substr(pos));
            break;
        }
        out.emplace_back(line.substr(pos, comma - pos));
        pos = comma + 1;
        if (pos == line.size()) {
            out.emplace_back(std::string_view{});
            break;
        }
    }
    return out;
}

static int64_t parse_i64(std::string_view v) {
    std::string s = trim(v);
    if (s.empty()) return 0;
    return std::strtoll(s.c_str(), nullptr, 10);
}

static std::string normalize_date(const std::string& raw) {
    std::string d;
    for (char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c))) d.push_back(c);
    }
    if (d.size() != 8) {
        throw std::runtime_error("date must contain 8 digits, got: " + raw);
    }
    return d;
}

static std::string date_prefix(const std::string& raw) {
    std::string d = normalize_date(raw);
    return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
}

static std::string normalize_symbol(std::string_view raw) {
    std::string s = trim(raw);
    bool numeric = !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
    if (numeric && s.size() < 6) {
        s.insert(s.begin(), 6 - s.size(), '0');
    }
    return s;
}

static std::string strip_suffix_symbol(std::string s) {
    size_t dot = s.find('.');
    if (dot != std::string::npos) s.resize(dot);
    return normalize_symbol(s);
}

static std::string normalize_time_ms(std::string_view raw) {
    std::string s = trim(raw);
    if (s.empty()) return "00:00:00.000";
    size_t sp = s.find(' ');
    if (sp != std::string::npos) s = s.substr(sp + 1);
    size_t dot = s.find('.');
    if (dot == std::string::npos) return s + ".000";
    std::string head = s.substr(0, dot);
    std::string frac = s.substr(dot + 1);
    if (frac.size() > 3) frac.resize(3);
    while (frac.size() < 3) frac.push_back('0');
    return head + "." + frac;
}

static int time_key_ms(std::string_view raw) {
    std::string s = normalize_time_ms(raw);
    if (s.size() < 12) return 0;
    int hh = std::atoi(s.substr(0, 2).c_str());
    int mm = std::atoi(s.substr(3, 2).c_str());
    int ss = std::atoi(s.substr(6, 2).c_str());
    int ms = std::atoi(s.substr(9, 3).c_str());
    return ((hh * 60 + mm) * 60 + ss) * 1000 + ms;
}

static std::string flow_time(const std::string& prefix, std::string_view raw) {
    return prefix + " " + normalize_time_ms(raw) + "000000";
}

static std::string compact_number(std::string_view raw) {
    std::string s = trim(raw);
    if (s.empty()) return "0";
    size_t dot = s.find('.');
    if (dot == std::string::npos) return s;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty() || s == "-0") return "0";
    return s;
}




static std::string compact_general_six(std::string_view raw) {
    std::string s = trim(raw);
    if (s.empty()) return "0";
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s.erase(s.begin());
    }
    size_t dot = s.find('.');
    std::string ip = dot == std::string::npos ? s : s.substr(0, dot);
    std::string fp = dot == std::string::npos ? std::string{} : s.substr(dot + 1);
    if (ip.empty()) ip = "0";
    size_t nz = ip.find_first_not_of('0');
    int int_digits = nz == std::string::npos ? 0 : static_cast<int>(ip.size() - nz);
    if (int_digits == 0) {
        std::string tmp = (neg ? std::string("-") : std::string()) + s;
        double x = std::strtod(tmp.c_str(), nullptr);
        std::ostringstream os;
        os.precision(6);
        os << x;
        return compact_number(os.str());
    }
    if (int_digits > 6) {
        std::string digits = ip + fp;
        while (digits.size() <= 6) digits.push_back('0');
        std::string kept = digits.substr(0, 6);
        char round_digit = digits[6];
        bool round_up = round_digit > '5';
        if (round_digit == '5') {
            bool rest_nonzero = false;
            for (size_t j = 7; j < digits.size(); ++j) {
                if (digits[j] != '0') { rest_nonzero = true; break; }
            }
            round_up = rest_nonzero || (!kept.empty() && ((kept.back() - '0') % 2 == 1));
        }
        if (round_up) {
            int i = static_cast<int>(kept.size()) - 1;
            while (i >= 0 && kept[i] == '9') { kept[i] = '0'; --i; }
            if (i >= 0) kept[i] += 1;
            else kept = std::string("1") + kept;
        }
        int exponent = int_digits - 1;
        if (kept.size() > 6) {
            kept.resize(6);
            exponent += 1;
        }
        std::string mant = kept.substr(0, 1);
        std::string rest = kept.substr(1);
        while (!rest.empty() && rest.back() == '0') rest.pop_back();
        if (!rest.empty()) mant += "." + rest;
        std::ostringstream eos;
        eos << mant << "e+";
        if (exponent < 10) eos << '0';
        eos << exponent;
        return neg ? "-" + eos.str() : eos.str();
    }
    int keep_frac = std::max(0, 6 - int_digits);
    while (static_cast<int>(fp.size()) <= keep_frac) fp.push_back('0');
    std::string kept = ip + fp.substr(0, keep_frac);
    char round_digit = fp[keep_frac];
    bool round_up = round_digit > '5';
    if (round_digit == '5') {
        bool rest_nonzero = false;
        for (size_t j = static_cast<size_t>(keep_frac + 1); j < fp.size(); ++j) {
            if (fp[j] != '0') { rest_nonzero = true; break; }
        }
        round_up = rest_nonzero || (!kept.empty() && ((kept.back() - '0') % 2 == 1));
    }
    if (round_up) {
        int i = static_cast<int>(kept.size()) - 1;
        while (i >= 0 && kept[i] == '9') {
            kept[i] = '0';
            --i;
        }
        if (i >= 0) {
            kept[i] += 1;
        } else {
            kept.insert(kept.begin(), '1');
        }
    }
    int decimal_pos = static_cast<int>(ip.size());
    if (static_cast<int>(kept.size()) > static_cast<int>(ip.size()) + keep_frac) {
        decimal_pos += 1;
    }
    if (keep_frac == 0 || decimal_pos >= static_cast<int>(kept.size())) {
        if (decimal_pos > static_cast<int>(kept.size())) {
            kept.append(decimal_pos - kept.size(), '0');
        } else if (decimal_pos < static_cast<int>(kept.size())) {
            kept.resize(decimal_pos);
        }
        std::string out = kept;
        size_t first = out.find_first_not_of('0');
        out = first == std::string::npos ? "0" : out.substr(first);
        return neg && out != "0" ? "-" + out : out;
    }
    std::string out = kept.substr(0, decimal_pos) + "." + kept.substr(decimal_pos);
    out = compact_number(out);
    return neg && out != "0" ? "-" + out : out;
}

static std::string compact_fixed_one(std::string_view raw) {
    double x = std::strtod(std::string(trim(raw)).c_str(), nullptr);
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << x;
    return compact_number(os.str());
}

static std::string compact_double(double x) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(6);
    os << x;
    return compact_number(os.str());
}

static std::string fixed6_double(double x) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(6);
    os << x;
    return os.str();
}

static std::string format_source_number(std::string_view raw) {
    std::string s = trim(raw);
    if (s.empty()) return "0";
    if (g_options.number_format == "raw") return s;
    if (g_options.number_format == "fixed6") {
        double x = std::strtod(s.c_str(), nullptr);
        return fixed6_double(x);
    }
    return compact_number(s);
}

static std::string format_computed_number(double x) {
    if (g_options.number_format == "fixed6") return fixed6_double(x);
    return compact_double(x);
}

static std::string format_volume(std::string_view raw) {
    return compact_number(raw);
}

static std::map<std::string, std::string> parse_ordtype_map(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t comma = text.find(',', pos);
        std::string item = comma == std::string::npos ? text.substr(pos) : text.substr(pos, comma - pos);
        item = trim(item);
        if (!item.empty()) {
            size_t colon = item.find(':');
            if (colon == std::string::npos) throw std::runtime_error("invalid --sz-ordtype-map item: " + item);
            std::string key = trim(std::string_view(item).substr(0, colon));
            std::string value = trim(std::string_view(item).substr(colon + 1));
            if (key.empty() || value.empty()) throw std::runtime_error("invalid --sz-ordtype-map item: " + item);
            out[key] = value;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (!out.count("default")) out["default"] = "2";
    return out;
}

static std::map<std::string, std::vector<std::string>> parse_symbol_prefix_filter(const std::string& text) {
    std::map<std::string, std::vector<std::string>> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t comma = text.find(',', pos);
        std::string item = comma == std::string::npos ? text.substr(pos) : text.substr(pos, comma - pos);
        item = trim(item);
        if (!item.empty()) {
            size_t colon = item.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("invalid --symbol-prefix-filter item: " + item);
            }
            std::string market = trim(std::string_view(item).substr(0, colon));
            std::transform(market.begin(), market.end(), market.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (market != "SH" && market != "SZ") {
                throw std::runtime_error("invalid --symbol-prefix-filter market: " + market);
            }
            std::string prefixes = trim(std::string_view(item).substr(colon + 1));
            if (prefixes.empty()) {
                throw std::runtime_error("empty --symbol-prefix-filter prefix list for: " + market);
            }
            size_t p = 0;
            while (p <= prefixes.size()) {
                size_t bar = prefixes.find('|', p);
                std::string prefix = bar == std::string::npos ? prefixes.substr(p) : prefixes.substr(p, bar - p);
                prefix = trim(prefix);
                if (prefix.empty()) {
                    throw std::runtime_error("empty --symbol-prefix-filter prefix in: " + item);
                }
                bool numeric = std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) {
                    return std::isdigit(c);
                });
                if (!numeric) {
                    throw std::runtime_error("non-numeric --symbol-prefix-filter prefix: " + prefix);
                }
                out[market].push_back(prefix);
                if (bar == std::string::npos) break;
                p = bar + 1;
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

static bool accept_symbol_prefix(const std::string& market, const std::string& symbol) {
    if (g_options.symbol_prefix_filter.empty()) return true;
    auto it = g_options.symbol_prefix_filter.find(market);
    if (it == g_options.symbol_prefix_filter.end()) return false;
    for (const auto& prefix : it->second) {
        if (symbol.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

static void require_one_of(const std::string& value, const std::string& name, const std::vector<std::string>& allowed) {
    for (const auto& v : allowed) {
        if (value == v) return;
    }
    std::ostringstream os;
    os << name << " must be one of";
    for (const auto& v : allowed) os << " " << v;
    os << ", got: " << value;
    throw std::runtime_error(os.str());
}

static std::string turnover_by_mode(const std::string& mode,
                                    std::string_view raw_turnover,
                                    std::string_view raw_price,
                                    std::string_view raw_volume) {
    if (mode == "zero") return "0";
    if (mode == "raw") {
        std::string raw = trim(raw_turnover);
        return raw.empty() ? "0" : format_source_number(raw);
    }
    if (mode == "pxqty") {
        double px = std::strtod(std::string(trim(raw_price)).c_str(), nullptr);
        double qty = std::strtod(std::string(trim(raw_volume)).c_str(), nullptr);
        return format_computed_number(px * qty);
    }
    throw std::runtime_error("invalid turnover mode: " + mode);
}

static std::string flow_id(std::string_view channel, std::string_view seq) {
    int64_t ch = parse_i64(channel);
    int64_t n = parse_i64(seq);
    if (n <= 0) return "0";
    return std::to_string(ch * kIdMultiplier + n);
}

static bool side_is_buy(std::string_view side) {
    std::string s = trim(side);
    return s == "B" || s == "49" || s == "1";
}

static bool side_is_sell(std::string_view side) {
    std::string s = trim(side);
    return s == "S" || s == "50" || s == "2";
}

static std::string side_dir(std::string_view side) {
    return side_is_sell(side) ? "1" : "0";
}

static std::string sh_trade_direction(std::string_view flag) {
    std::string s = trim(flag);
    if (s == "B") return "1";
    if (s == "S") return "2";
    return "0";
}

static std::string sz_price_type(std::string_view ord_type) {
    std::string s = trim(ord_type);
    auto it = g_options.sz_ordtype_map.find(s);
    if (it != g_options.sz_ordtype_map.end()) return it->second;
    auto def = g_options.sz_ordtype_map.find("default");
    return def == g_options.sz_ordtype_map.end() ? "2" : def->second;
}

static void append_row(std::string& line,
                       const std::string& instrument,
                       const std::string& exchange_time,
                       const std::string& local_time,
                       const std::string& action_type,
                       const std::string& dir,
                       const std::string& trade_direction,
                       const std::string& price_type,
                       const std::string& bid_id,
                       const std::string& ask_id,
                       const std::string& price,
                       const std::string& volume,
                       const std::string& turnover,
                       const std::string& app_seq) {
    line.clear();
    line.reserve(192);
    line += instrument;
    line += ',';
    line += exchange_time;
    line += ',';
    line += local_time;
    line += ",,,0,";
    line += action_type;
    line += ',';
    line += dir;
    line += ',';
    line += trade_direction;
    line += ',';
    line += price_type;
    line += ',';
    line += bid_id;
    line += ',';
    line += ask_id;
    line += ',';
    line += price;
    line += ',';
    line += volume;
    line += ',';
    line += turnover;
    line += ',';
    line += app_seq;
}

class Reader {
public:
    Reader(std::string path,
           std::string date_prefix,
           std::string symbol,
           int start_key,
           int end_key,
           int source_order)
        : path_(std::move(path)),
          date_prefix_(std::move(date_prefix)),
          symbol_(std::move(symbol)),
          start_key_(start_key),
          end_key_(end_key),
          source_order_(source_order) {}

    virtual ~Reader() = default;

    bool open() {
        in_.open(path_);
        if (!in_) {
            std::cerr << "skip missing file: " << path_ << "\n";
            return false;
        }
        in_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        std::getline(in_, line_);
        return true;
    }

    bool next(FlowRow& row) {
        while (std::getline(in_, line_)) {
            if (line_.empty()) continue;
            if (!line_.empty() && line_.back() == '\r') line_.pop_back();
            auto fields = split_csv_line(line_);
            if (parse(fields, row)) return true;
        }
        return false;
    }

protected:
    virtual bool parse(const std::vector<std::string_view>& fields, FlowRow& row) = 0;

    bool accept_time(std::string_view t) const {
        int k = time_key_ms(t);
        return (start_key_ < 0 || k >= start_key_) && (end_key_ < 0 || k <= end_key_);
    }

    bool accept_symbol(std::string_view raw, const std::string& market) const {
        std::string symbol = normalize_symbol(raw);
        if (!symbol_.empty() && symbol != symbol_) return false;
        return accept_symbol_prefix(market, symbol);
    }

    std::ifstream in_;
    std::string path_;
    std::string date_prefix_;
    std::string symbol_;
    int start_key_ = -1;
    int end_key_ = -1;
    int source_order_ = 0;
    std::string line_;
    std::string out_line_;
    std::vector<char> buffer_ = std::vector<char>(16 * 1024 * 1024);
};

class ShReader final : public Reader {
public:
    using Reader::Reader;

private:
    bool parse(const std::vector<std::string_view>& f, FlowRow& row) override {
        if (f.size() < 13) return false;
        std::string typ = trim(f[4]);
        if (typ != "A" && typ != "D" && typ != "T") return false;
        if (!accept_time(f[3])) return false;
        std::string security = normalize_symbol(f[2]);
        if (!accept_symbol(security, "SH")) return false;

        const auto& channel = f[1];
        const auto& flag = f[10];
        std::string buy_id = flow_id(channel, f[5]);
        std::string sell_id = flow_id(channel, f[6]);
        std::string action_type;
        std::string dir = side_dir(flag);
        std::string trade_direction = "0";
        std::string bid_id;
        std::string ask_id;

        if (typ == "A") {
            action_type = "0";
            bid_id = side_is_buy(flag) ? buy_id : "0";
            ask_id = side_is_sell(flag) ? sell_id : "0";
        } else if (typ == "D") {
            action_type = "1";
            bid_id = side_is_buy(flag) ? buy_id : "0";
            ask_id = side_is_sell(flag) ? sell_id : "0";
        } else {
            action_type = "2";
            if (g_options.sh_trade_dir_mode == "zero") dir = "0";
            bid_id = buy_id;
            ask_id = sell_id;
            trade_direction = sh_trade_direction(flag);
        }

        std::string app_seq = flow_id(channel, f[0]);
        std::string turnover = turnover_by_mode(g_options.sh_turnover, f[9], f[7], f[8]);
        append_row(out_line_,
                   security + ".SH",
                   flow_time(date_prefix_, f[3]),
                   flow_time(date_prefix_, f[11]),
                   action_type,
                   dir,
                   trade_direction,
                   g_options.sh_price_type,
                   bid_id,
                   ask_id,
                   format_source_number(f[7]),
                   format_volume(f[8]),
                   turnover,
                   app_seq);
        row.line = out_line_;
        row.instrument = security + ".SH";
        row.local_key = time_key_ms(f[11]);
        row.exchange_key = time_key_ms(f[3]);
        row.seq_no = parse_i64(f[12]);
        row.source_order = source_order_;
        return true;
    }
};

class SzOrderReader final : public Reader {
public:
    using Reader::Reader;

private:
    bool parse(const std::vector<std::string_view>& f, FlowRow& row) override {
        if (f.size() < 12) return false;
        if (!accept_time(f[8])) return false;
        std::string security = normalize_symbol(f[3]);
        if (!accept_symbol(security, "SZ")) return false;

        std::string app_seq = flow_id(f[0], f[1]);
        bool buy = side_is_buy(f[7]);
        std::string price_type = sz_price_type(f[9]);
        std::string price = format_source_number(f[5]);
        append_row(out_line_,
                   security + ".SZ",
                   flow_time(date_prefix_, f[8]),
                   flow_time(date_prefix_, f[10]),
                   "0",
                   buy ? "0" : "1",
                   "0",
                   price_type,
                   buy ? app_seq : "0",
                   buy ? "0" : app_seq,
                   price,
                   format_volume(f[6]),
                   "0",
                   app_seq);
        row.line = out_line_;
        row.instrument = security + ".SZ";
        row.local_key = time_key_ms(f[10]);
        row.exchange_key = time_key_ms(f[8]);
        row.seq_no = parse_i64(f[11]);
        row.source_order = source_order_;
        return true;
    }
};

class SzTradeReader final : public Reader {
public:
    using Reader::Reader;

private:
    bool parse(const std::vector<std::string_view>& f, FlowRow& row) override {
        if (f.size() < 13) return false;
        std::string exec_type = trim(f[9]);
        if (exec_type != "52" && exec_type != "70") return false;
        if (!accept_time(f[10])) return false;
        std::string security = normalize_symbol(f[5]);
        if (!accept_symbol(security, "SZ")) return false;

        bool cancel = exec_type == "52";
        std::string bid_id = flow_id(f[0], f[3]);
        std::string ask_id = flow_id(f[0], f[4]);
        bool buy_side = bid_id != "0";
        std::string price = format_source_number(f[7]);
        std::string volume = format_volume(f[8]);
        std::string turnover = turnover_by_mode(cancel ? g_options.sz_cancel_turnover : g_options.sz_trade_turnover,
                                                "", f[7], f[8]);
        std::string app_seq = flow_id(f[0], f[1]);
        append_row(out_line_,
                   security + ".SZ",
                   flow_time(date_prefix_, f[10]),
                   flow_time(date_prefix_, f[11]),
                   cancel ? "1" : "2",
                   buy_side ? "0" : "1",
                   "0",
                   "0",
                   bid_id,
                   ask_id,
                   price,
                   volume,
                   turnover,
                   app_seq);
        row.line = out_line_;
        row.instrument = security + ".SZ";
        row.local_key = time_key_ms(f[11]);
        row.exchange_key = time_key_ms(f[10]);
        row.seq_no = parse_i64(f[12]);
        row.source_order = source_order_;
        return true;
    }
};

struct QueueItem {
    FlowRow row;
    size_t reader_idx = 0;
};

struct QueueCompare {
    bool operator()(const QueueItem& a, const QueueItem& b) const {
        if (a.row.local_key != b.row.local_key) return a.row.local_key > b.row.local_key;
        if (a.row.exchange_key != b.row.exchange_key) return a.row.exchange_key > b.row.exchange_key;
        if (a.row.seq_no != b.row.seq_no) return a.row.seq_no > b.row.seq_no;
        return a.row.source_order > b.row.source_order;
    }
};


static std::string xml_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&apos;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out.push_back(c);
    }
    return out;
}

static bool replace_xml_attr(std::string& line, const std::string& name, const std::string& value) {
    std::string needle = name + "=\"";
    size_t b = line.find(needle);
    if (b == std::string::npos) return false;
    b += needle.size();
    size_t e = line.find('"', b);
    if (e == std::string::npos) return false;
    line.replace(b, e - b, xml_escape(value));
    return true;
}

static std::string instrument_template_line(const std::string& template_path) {
    std::ifstream in(template_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("<Instrument ") != std::string::npos) {
            if (!replace_xml_attr(line, "InstrumentID", "__INSTRUMENT_ID__")) break;
            if (!replace_xml_attr(line, "ExchangeID", "__EXCHANGE_ID__")) break;
            return line;
        }
    }
    return "  <Instrument InstrumentID=\"__INSTRUMENT_ID__\" ProductID=\"stock\" ExchangeID=\"__EXCHANGE_ID__\" FeeType=\"Turnover\" OpenFee=\"0.0001346\" CloseTodayFee=\"0.0001346\" CloseYesterdayFee=\"0.0001346\" Margin=\"1\" PriceTick=\"0.01\" VolumeMultiple=\"1\" VolumeLimit=\"1000000\"/>";
}

static std::string exchange_for_instrument(const std::string& instrument) {
    if (instrument.size() >= 3 && instrument.substr(instrument.size() - 3) == ".SH") return "SSE";
    if (instrument.size() >= 3 && instrument.substr(instrument.size() - 3) == ".SZ") return "SZSE";
    throw std::runtime_error("cannot infer ExchangeID for instrument: " + instrument);
}

static void write_instruments_xml(const std::string& output_path,
                                  const std::string& template_path,
                                  const std::set<std::string>& instruments) {
    if (output_path.empty()) return;
    std::filesystem::path path(output_path);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(output_path);
    if (!out) throw std::runtime_error("cannot open instruments xml output: " + output_path);
    std::string tmpl = instrument_template_line(template_path);
    out << "<Instruments>\n";
    for (const auto& inst : instruments) {
        std::string line = tmpl;
        if (!replace_xml_attr(line, "InstrumentID", inst)) {
            throw std::runtime_error("instrument template missing InstrumentID attribute");
        }
        if (!replace_xml_attr(line, "ExchangeID", exchange_for_instrument(inst))) {
            throw std::runtime_error("instrument template missing ExchangeID attribute");
        }
        out << line << '\n';
    }
    out << "</Instruments>\n";
    std::cerr << "wrote instruments xml: " << output_path << ", instruments: " << instruments.size() << "\n";
}

static std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

static void print_help(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " -d YYYYMMDD [options]\n"
        << "Options:\n"
        << "  -d, --date YYYYMMDD              trade date\n"
        << "  -i, --input-dir DIR              TL CSV directory (default: <tl-root>/<DATE>)\n"
        << "  -o, --output-file FILE           output CSV path (default: <output-root>/<DATE>.csv)\n"
        << "  --output-instruments-xml FILE    output Instruments.xml path (default: <output-root>/<DATE>.xml)\n"
        << "  --instruments-template FILE      template XML preserving non-id/exchange attrs (default: Instruments.xml)\n"
        << "  --tl-root DIR                    default TL root (default: " << defaultTlRoot() << ")\n"
        << "  --output-root DIR                default output root (default: " << defaultOutputRoot() << ")\n"
        << "  -m, --market SH|SZ               optional market filter\n"
        << "  -s, --symbol SYMBOL              optional symbol filter, e.g. 600396.SH or 002519.SZ\n"
        << "  --symbol-prefix-filter FILTER    optional prefix filter, e.g. SH:6,SZ:0|3\n"
        << "  --start-time HH:MM:SS.mmm        optional inclusive exchange-time start\n"
        << "  --end-time HH:MM:SS.mmm          optional inclusive exchange-time end\n"
        << "  --sz-ordtype-map MAP             SZ OrdType->priceType map (default: 49:1,85:U,default:2)\n"
        << "  --sh-price-type VALUE            SH output priceType (default: 0)\n"
        << "  --sort-key appseq|time           output sort key (default: appseq)\n"
        << "  --sort-tmp-dir DIR               temporary directory used by sort -T\n"
        << "  --keep-temp                      keep <output>.unsorted.tmp after appseq sort\n"
        << "  --missing-policy fail|skip       missing input handling (default: fail)\n"
        << "  --number-format compact|fixed6|raw  price/turnover text format (default: compact)\n"
        << "  --sh-turnover raw|pxqty|zero     SH turnover mode (default: raw)\n"
        << "  --sh-trade-dir-mode zero|side    SH trade dir mode (default: zero)\n"
        << "  --sz-trade-turnover raw|pxqty|zero   SZ trade turnover mode (default: raw)\n"
        << "  --sz-cancel-turnover raw|pxqty|zero  SZ cancel turnover mode (default: raw)\n";
}

static Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto require_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + name);
            return argv[++i];
        };
        if (a == "-d" || a == "--date") opt.date = require_value(a);
        else if (a == "-i" || a == "--input-dir") opt.input_dir = require_value(a);
        else if (a == "-o" || a == "--output-file") opt.output_file = require_value(a);
        else if (a == "--output-instruments-xml") opt.output_instruments_xml = require_value(a);
        else if (a == "--instruments-template") opt.instruments_template = require_value(a);
        else if (a == "--tl-root") opt.tl_root = require_value(a);
        else if (a == "--output-root") opt.output_root = require_value(a);
        else if (a == "-m" || a == "--market") opt.market = require_value(a);
        else if (a == "-s" || a == "--symbol") opt.symbol = require_value(a);
        else if (a == "--symbol-prefix-filter") opt.symbol_prefix_filter_text = require_value(a);
        else if (a == "--start-time") opt.start_time = require_value(a);
        else if (a == "--end-time") opt.end_time = require_value(a);
        else if (a == "--sz-ordtype-map") opt.sz_ordtype_map_text = require_value(a);
        else if (a == "--sh-price-type") opt.sh_price_type = require_value(a);
        else if (a == "--sort-key") opt.sort_key = require_value(a);
        else if (a == "--sort-tmp-dir") opt.sort_tmp_dir = require_value(a);
        else if (a == "--keep-temp") opt.keep_temp = true;
        else if (a == "--missing-policy") opt.missing_policy = require_value(a);
        else if (a == "--number-format") opt.number_format = require_value(a);
        else if (a == "--sh-turnover") opt.sh_turnover = require_value(a);
        else if (a == "--sh-trade-dir-mode") opt.sh_trade_dir_mode = require_value(a);
        else if (a == "--sz-trade-turnover") opt.sz_trade_turnover = require_value(a);
        else if (a == "--sz-cancel-turnover") opt.sz_cancel_turnover = require_value(a);
        else if (a == "-h" || a == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + a);
        }
    }
    if (opt.date.empty()) throw std::runtime_error("--date is required");
    std::string date = normalize_date(opt.date);
    if (opt.input_dir.empty()) opt.input_dir = opt.tl_root + "/" + date;
    if (opt.output_file.empty()) opt.output_file = opt.output_root + "/" + date + ".csv";
    if (opt.output_instruments_xml.empty()) opt.output_instruments_xml = opt.output_root + "/" + date + ".xml";
    if (opt.instruments_template.empty()) opt.instruments_template = "Instruments.xml";
    if (!opt.symbol.empty()) opt.symbol = strip_suffix_symbol(opt.symbol);
    std::transform(opt.market.begin(), opt.market.end(), opt.market.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    std::transform(opt.sort_key.begin(), opt.sort_key.end(), opt.sort_key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(opt.missing_policy.begin(), opt.missing_policy.end(), opt.missing_policy.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(opt.number_format.begin(), opt.number_format.end(), opt.number_format.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(opt.sh_turnover.begin(), opt.sh_turnover.end(), opt.sh_turnover.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(opt.sh_trade_dir_mode.begin(), opt.sh_trade_dir_mode.end(), opt.sh_trade_dir_mode.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(opt.sz_trade_turnover.begin(), opt.sz_trade_turnover.end(), opt.sz_trade_turnover.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(opt.sz_cancel_turnover.begin(), opt.sz_cancel_turnover.end(), opt.sz_cancel_turnover.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    require_one_of(opt.sort_key, "--sort-key", {"appseq", "time"});
    require_one_of(opt.missing_policy, "--missing-policy", {"fail", "skip"});
    require_one_of(opt.number_format, "--number-format", {"compact", "fixed6", "raw"});
    require_one_of(opt.sh_turnover, "--sh-turnover", {"raw", "pxqty", "zero"});
    require_one_of(opt.sh_trade_dir_mode, "--sh-trade-dir-mode", {"zero", "side"});
    require_one_of(opt.sz_trade_turnover, "--sz-trade-turnover", {"raw", "pxqty", "zero"});
    require_one_of(opt.sz_cancel_turnover, "--sz-cancel-turnover", {"raw", "pxqty", "zero"});
    opt.sz_ordtype_map = parse_ordtype_map(opt.sz_ordtype_map_text);
    opt.symbol_prefix_filter = parse_symbol_prefix_filter(opt.symbol_prefix_filter_text);
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);
        g_options = opt;
        std::string prefix = date_prefix(opt.date);
        int start_key = opt.start_time.empty() ? -1 : time_key_ms(opt.start_time);
        int end_key = opt.end_time.empty() ? -1 : time_key_ms(opt.end_time);

        std::vector<std::unique_ptr<Reader>> readers;
        if (opt.market.empty() || opt.market == "SH") {
            readers.emplace_back(std::make_unique<ShReader>(
                opt.input_dir + "/mdl_4_24_0.csv", prefix, opt.symbol, start_key, end_key, 0));
        }
        if (opt.market.empty() || opt.market == "SZ") {
            readers.emplace_back(std::make_unique<SzOrderReader>(
                opt.input_dir + "/mdl_6_33_0.csv", prefix, opt.symbol, start_key, end_key, 1));
            readers.emplace_back(std::make_unique<SzTradeReader>(
                opt.input_dir + "/mdl_6_36_0.csv", prefix, opt.symbol, start_key, end_key, 2));
        }

        std::set<std::string> output_instruments;

        std::filesystem::path output_path(opt.output_file);
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path());
        }

        auto open_reader = [&](size_t i) -> bool {
            if (readers[i]->open()) return true;
            if (opt.missing_policy == "skip") return false;
            throw std::runtime_error("required input file is missing: reader " + std::to_string(i));
        };

        if (opt.sort_key == "time") {
            std::ofstream out(opt.output_file);
            if (!out) throw std::runtime_error("cannot open output: " + opt.output_file);
            std::vector<char> out_buffer(32 * 1024 * 1024);
            out.rdbuf()->pubsetbuf(out_buffer.data(), static_cast<std::streamsize>(out_buffer.size()));
            out << "instrumentID,exchangeTime,localTime,tradeExchange,trSalCond,fillType,actionType,dir,"
                   "tradeDirection,priceType,bidId,askId,price,volume,turnover,appSeq\n";

            std::priority_queue<QueueItem, std::vector<QueueItem>, QueueCompare> pq;
            for (size_t i = 0; i < readers.size(); ++i) {
                if (!open_reader(i)) continue;
                FlowRow row;
                if (readers[i]->next(row)) pq.push(QueueItem{std::move(row), i});
            }

            uint64_t rows = 0;
            while (!pq.empty()) {
                QueueItem item = std::move(const_cast<QueueItem&>(pq.top()));
                pq.pop();
                out << item.row.line << '\n';
                output_instruments.insert(item.row.instrument);
                ++rows;
                FlowRow next_row;
                if (readers[item.reader_idx]->next(next_row)) {
                    pq.push(QueueItem{std::move(next_row), item.reader_idx});
                }
                if (rows % 10000000ULL == 0) {
                    std::cerr << "written rows: " << rows << "\n";
                }
            }
            std::cerr << "done, rows: " << rows << ", output sorted by time: " << opt.output_file << "\n";
        } else {
            std::filesystem::path temp_path = output_path;
            temp_path += ".unsorted.tmp";
            {
                std::ofstream tmp(temp_path);
                if (!tmp) throw std::runtime_error("cannot open temp output: " + temp_path.string());
                std::vector<char> tmp_buffer(32 * 1024 * 1024);
                tmp.rdbuf()->pubsetbuf(tmp_buffer.data(), static_cast<std::streamsize>(tmp_buffer.size()));

                uint64_t rows = 0;
                for (size_t i = 0; i < readers.size(); ++i) {
                    bool opened = false;
                    try {
                        opened = open_reader(i);
                    } catch (...) {
                        tmp.close();
                        std::filesystem::remove(temp_path);
                        throw;
                    }
                    if (!opened) continue;
                    FlowRow row;
                    while (readers[i]->next(row)) {
                        tmp << row.line << '\n';
                        output_instruments.insert(row.instrument);
                        ++rows;
                        if (rows % 10000000ULL == 0) {
                            std::cerr << "written temp rows: " << rows << "\n";
                        }
                    }
                }
                std::cerr << "temp rows: " << rows << ", temp: " << temp_path.string() << "\n";
            }

            {
                std::ofstream out(opt.output_file);
                if (!out) throw std::runtime_error("cannot open output: " + opt.output_file);
                out << "instrumentID,exchangeTime,localTime,tradeExchange,trSalCond,fillType,actionType,dir,"
                       "tradeDirection,priceType,bidId,askId,price,volume,turnover,appSeq\n";
            }

            std::string sort_cmd = "LC_ALL=C sort -t, -k16,16n ";
            if (!opt.sort_tmp_dir.empty()) sort_cmd += "-T " + shell_quote(opt.sort_tmp_dir) + " ";
            sort_cmd += shell_quote(temp_path.string()) + " >> " + shell_quote(opt.output_file);
            int sort_rc = std::system(sort_cmd.c_str());
            if (sort_rc != 0) {
                throw std::runtime_error("sort failed with code " + std::to_string(sort_rc) + ": " + sort_cmd);
            }
            if (!opt.keep_temp) std::filesystem::remove(temp_path);
            std::cerr << "done, output sorted by appSeq: " << opt.output_file << "\n";
        }
        write_instruments_xml(opt.output_instruments_xml, opt.instruments_template, output_instruments);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        print_help(argv[0]);
        return 1;
    }
}
