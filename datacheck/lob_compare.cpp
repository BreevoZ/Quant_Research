/**
 * lob_compare.cpp
 *
 * 对比两个 LOB 输出目录，支持 CSV（{sym}_order.csv）和 H5（{sym}.h5），自动检测。
 *
 * 用法:
 *   lob_compare --dir-a <pathA> --dir-b <pathB> --output <report.md>
 *               [--label-a LabelA] [--label-b LabelB]
 *               [--date YYYYMMDD] [--workers N] [--top N]
 */

#include <H5Cpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ── 线程池 ────────────────────────────────────────────────────────────────────

struct Pool {
    explicit Pool(int n) {
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> t;
                    { std::unique_lock<std::mutex> lk(mu_);
                      cv_.wait(lk, [this]{ return stop_ || !q_.empty(); });
                      if (stop_ && q_.empty()) return;
                      t = std::move(q_.front()); q_.pop(); }
                    t();
                }
            });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }
    template<class F>
    std::future<void> submit(F&& f) {
        auto p = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
        auto fut = p->get_future();
        { std::lock_guard<std::mutex> lk(mu_); q_.emplace([p]{ (*p)(); }); }
        cv_.notify_one();
        return fut;
    }
private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> q_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// ── CSV 工具 ──────────────────────────────────────────────────────────────────

static void splitCSV(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i)
        if (i == line.size() || line[i] == ',') {
            out.emplace_back(line, start, i - start);
            start = i + 1;
        }
}

static std::unordered_map<std::string, int>
parseHeader(const std::vector<std::string>& fields) {
    std::unordered_map<std::string, int> m;
    for (int i = 0; i < (int)fields.size(); ++i) m[fields[i]] = i;
    return m;
}

// ── 数据结构 ──────────────────────────────────────────────────────────────────

// 订单表：每行一个事件（同一 sysid 可能有多行：wt/zb/cl）
struct OrderRow {
    std::string ordertype;  // wt / cl / zb
    char direction = ' ';  // B / S
    double price = 0;
    int64_t volume = 0;
    int type = 0;           // 1-6 分类
    // LOB 快照（最佳档）
    double bdp1 = 0, akp1 = 0;
    int64_t bdv1 = 0, akv1 = 0;
    int bidLevels = 0, askLevels = 0;
};

// 成交表：每行一个订单的完整成交统计（sysid 唯一）
struct TradeRow {
    int64_t trans_vlm = 0;
    double vwap = 0;
    int64_t act_vlm = 0, pas_vlm = 0;
    int act_num = 0, pas_num = 0;
};

// 股票级别汇总
struct SymResult {
    std::string sym;

    // 委托事件数（按类型）
    int wt_a = 0, wt_b = 0;
    int cl_a = 0, cl_b = 0;
    int zb_a = 0, zb_b = 0;

    // 仅在 A/B 出现的 wt sysid 数
    int wt_a_only = 0, wt_b_only = 0;

    // 匹配 wt 中: 方向/价格/量/类型不同数
    int dir_mis = 0, price_mis = 0, vol_mis = 0, cls_mis = 0;

    // LOB 快照（末行）不同
    int lob_bdp_mis = 0, lob_akp_mis = 0;  // 0=相同 1=不同
    double bdp1_a = 0, bdp1_b = 0;
    double akp1_a = 0, akp1_b = 0;

    // 成交表
    int trd_a = 0, trd_b = 0;
    int trd_a_only = 0, trd_b_only = 0;
    int trd_vol_mis = 0, trd_vwap_mis = 0;
    int64_t tvol_a = 0, tvol_b = 0;

    bool perfect() const {
        return wt_a == wt_b && cl_a == cl_b && zb_a == zb_b &&
               trd_a == trd_b && wt_a_only == 0 && wt_b_only == 0 &&
               dir_mis == 0 && price_mis == 0 && vol_mis == 0 &&
               cls_mis == 0 && lob_bdp_mis == 0 && lob_akp_mis == 0 &&
               trd_a_only == 0 && trd_b_only == 0 &&
               trd_vol_mis == 0 && trd_vwap_mis == 0;
    }

    int totalDiff() const {
        return std::abs(wt_a - wt_b) + std::abs(cl_a - cl_b) +
               std::abs(zb_a - zb_b) + std::abs(trd_a - trd_b) +
               wt_a_only + wt_b_only +
               dir_mis + price_mis + vol_mis + cls_mis +
               lob_bdp_mis + lob_akp_mis +
               trd_a_only + trd_b_only + trd_vol_mis;
    }
};

// ── 读取 _order.csv ───────────────────────────────────────────────────────────

struct OrderData {
    std::unordered_map<int64_t, std::vector<OrderRow>> byId;  // sysid → events
    OrderRow lastRow;  // 末行（LOB 快照参考）
    bool hasLast = false;
    int wt = 0, cl = 0, zb = 0;
};

static OrderData readOrderCSV(const std::string& path) {
    OrderData out;
    std::ifstream ifs(path);
    if (!ifs) return out;

    std::string line;
    if (!std::getline(ifs, line)) return out;

    std::vector<std::string> flds;
    splitCSV(line, flds);
    auto col = parseHeader(flds);

    auto gi = [&](const std::string& k) -> int {
        auto it = col.find(k);
        return it != col.end() ? it->second : -1;
    };

    int c_sid = gi("sysid"), c_ot = gi("ordertype"), c_dir = gi("direction"),
        c_prc = gi("price"), c_vol = gi("volume"), c_tp = gi("type"),
        c_bdp = gi("bdp1"), c_akp = gi("akp1"),
        c_bdv = gi("bdv1"), c_akv = gi("akv1"),
        c_bl  = gi("bidLevels"), c_al  = gi("askLevels");

    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        splitCSV(line, flds);
        if (c_sid < 0 || (int)flds.size() <= c_sid) continue;

        int64_t sid = std::stoll(flds[c_sid]);
        OrderRow r;
        if (c_ot  >= 0 && (int)flds.size() > c_ot)  r.ordertype  = flds[c_ot];
        if (c_dir >= 0 && (int)flds.size() > c_dir)  r.direction  = flds[c_dir].empty() ? ' ' : flds[c_dir][0];
        if (c_prc >= 0 && (int)flds.size() > c_prc)  r.price      = std::stod(flds[c_prc]);
        if (c_vol >= 0 && (int)flds.size() > c_vol)  r.volume     = std::stoll(flds[c_vol]);
        if (c_tp  >= 0 && (int)flds.size() > c_tp && !flds[c_tp].empty())
            r.type = std::stoi(flds[c_tp]);
        if (c_bdp >= 0 && (int)flds.size() > c_bdp)  r.bdp1       = std::stod(flds[c_bdp]);
        if (c_akp >= 0 && (int)flds.size() > c_akp)  r.akp1       = std::stod(flds[c_akp]);
        if (c_bdv >= 0 && (int)flds.size() > c_bdv)  r.bdv1       = std::stoll(flds[c_bdv]);
        if (c_akv >= 0 && (int)flds.size() > c_akv)  r.akv1       = std::stoll(flds[c_akv]);
        if (c_bl  >= 0 && (int)flds.size() > c_bl)   r.bidLevels  = std::stoi(flds[c_bl]);
        if (c_al  >= 0 && (int)flds.size() > c_al)   r.askLevels  = std::stoi(flds[c_al]);

        if (r.ordertype == "wt") out.wt++;
        else if (r.ordertype == "cl") out.cl++;
        else if (r.ordertype == "zb") out.zb++;

        out.byId[sid].push_back(r);
        out.lastRow = r;
        out.hasLast = true;
    }
    return out;
}

// ── 读取 _trade.csv ───────────────────────────────────────────────────────────

static std::unordered_map<int64_t, TradeRow> readTradeCSV(const std::string& path) {
    std::unordered_map<int64_t, TradeRow> out;
    std::ifstream ifs(path);
    if (!ifs) return out;

    std::string line;
    if (!std::getline(ifs, line)) return out;

    std::vector<std::string> flds;
    splitCSV(line, flds);
    auto col = parseHeader(flds);

    auto gi = [&](const std::string& k) -> int {
        auto it = col.find(k);
        return it != col.end() ? it->second : -1;
    };

    int c_sid  = gi("sysid"), c_tv = gi("trans_vlm"), c_vw = gi("vwap"),
        c_av   = gi("act_vlm"), c_pv = gi("pas_vlm"),
        c_an   = gi("act_num"), c_pn = gi("pas_num");

    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        splitCSV(line, flds);
        if (c_sid < 0 || (int)flds.size() <= c_sid) continue;

        int64_t sid = std::stoll(flds[c_sid]);
        TradeRow r;
        if (c_tv >= 0 && (int)flds.size() > c_tv) r.trans_vlm = std::stoll(flds[c_tv]);
        if (c_vw >= 0 && (int)flds.size() > c_vw) r.vwap      = std::stod(flds[c_vw]);
        if (c_av >= 0 && (int)flds.size() > c_av) r.act_vlm   = std::stoll(flds[c_av]);
        if (c_pv >= 0 && (int)flds.size() > c_pv) r.pas_vlm   = std::stoll(flds[c_pv]);
        if (c_an >= 0 && (int)flds.size() > c_an) r.act_num   = std::stoi(flds[c_an]);
        if (c_pn >= 0 && (int)flds.size() > c_pn) r.pas_num   = std::stoi(flds[c_pn]);
        out[sid] = std::move(r);
    }
    return out;
}

// ── HDF5 读取 (compound type, 与 TLLobBuilder / dc::LobBuilder 格式兼容) ───────
// "order" 和 "trade_1500" 是根级 compound-type 1D dataset。

// sysid_mod: if >0, apply sysid = sysid % sysid_mod (strips TL's channel prefix)
// sysid_offset: if !=0, apply sysid -= sysid_offset (legacy)
static OrderData readOrderH5(const std::string& h5path, int64_t sysid_offset = 0,
                              int64_t sysid_mod = 0) {
    OrderData out;
    try {
        H5::Exception::dontPrint();
        H5::H5File file(h5path, H5F_ACC_RDONLY);
        if (!file.exists("order")) return out;

        // 只读需要的字段（partial compound read）
        struct Row {
            int64_t sysid;
            char    ordertype[4];
            char    direction[2];
            double  price;
            int64_t volume;
            int64_t type;
            double  bdp1;
            double  akp1;
        };
        H5::StrType st4(H5::PredType::C_S1, 4);
        H5::StrType st2(H5::PredType::C_S1, 2);
        H5::CompType mt(sizeof(Row));
        mt.insertMember("sysid",     HOFFSET(Row, sysid),     H5::PredType::NATIVE_INT64);
        mt.insertMember("ordertype", HOFFSET(Row, ordertype), st4);
        mt.insertMember("direction", HOFFSET(Row, direction), st2);
        mt.insertMember("price",     HOFFSET(Row, price),     H5::PredType::NATIVE_DOUBLE);
        mt.insertMember("volume",    HOFFSET(Row, volume),    H5::PredType::NATIVE_INT64);
        mt.insertMember("type",      HOFFSET(Row, type),      H5::PredType::NATIVE_INT64);
        mt.insertMember("bdp1",      HOFFSET(Row, bdp1),      H5::PredType::NATIVE_DOUBLE);
        mt.insertMember("akp1",      HOFFSET(Row, akp1),      H5::PredType::NATIVE_DOUBLE);

        H5::DataSet ds = file.openDataSet("order");
        hsize_t dims[1]; ds.getSpace().getSimpleExtentDims(dims);
        std::vector<Row> rows(dims[0]);
        if (dims[0] > 0) ds.read(rows.data(), mt);

        for (const auto& row : rows) {
            OrderRow r;
            r.ordertype = row.ordertype;
            r.direction = row.direction[0];
            r.price     = row.price;
            r.volume    = row.volume;
            r.type      = (int)row.type;
            r.bdp1      = row.bdp1;
            r.akp1      = row.akp1;
            if (r.ordertype == "wt")      out.wt++;
            else if (r.ordertype == "cl") out.cl++;
            else if (r.ordertype == "zb") out.zb++;
            int64_t sid = row.sysid - sysid_offset;
            if (sysid_mod > 0) sid = sid % sysid_mod;
            out.byId[sid].push_back(r);
            out.lastRow = r;
            out.hasLast = true;
        }
    } catch (const H5::Exception&) {}
    return out;
}

static std::unordered_map<int64_t, TradeRow> readTradeH5(const std::string& h5path,
                                                           int64_t sysid_offset = 0,
                                                           int64_t sysid_mod = 0) {
    std::unordered_map<int64_t, TradeRow> out;
    try {
        H5::Exception::dontPrint();
        H5::H5File file(h5path, H5F_ACC_RDONLY);
        if (!file.exists("trade_1500")) return out;

        struct Row { int64_t sysid, trans_vlm; double vwap; };
        H5::CompType mt(sizeof(Row));
        mt.insertMember("sysid",     HOFFSET(Row, sysid),     H5::PredType::NATIVE_INT64);
        mt.insertMember("trans_vlm", HOFFSET(Row, trans_vlm), H5::PredType::NATIVE_INT64);
        mt.insertMember("vwap",      HOFFSET(Row, vwap),      H5::PredType::NATIVE_DOUBLE);

        H5::DataSet ds = file.openDataSet("trade_1500");
        hsize_t dims[1]; ds.getSpace().getSimpleExtentDims(dims);
        std::vector<Row> rows(dims[0]);
        if (dims[0] > 0) ds.read(rows.data(), mt);

        for (const auto& row : rows) {
            TradeRow r;
            r.trans_vlm = row.trans_vlm;
            r.vwap      = row.vwap;
            int64_t sid = row.sysid - sysid_offset;
            if (sysid_mod > 0) sid = sid % sysid_mod;
            out[sid] = r;
        }
    } catch (const H5::Exception&) {}
    return out;
}

// ── 统一读取入口（自动检测 .h5 或 _order.csv）────────────────────────────────

static bool hasH5(const std::string& dir, const std::string& sym) {
    std::ifstream f(dir + "/" + sym + ".h5");
    return f.good();
}

static OrderData readOrderAuto(const std::string& dir, const std::string& sym,
                                int64_t sysid_offset = 0, int64_t sysid_mod = 0) {
    if (hasH5(dir, sym)) return readOrderH5(dir + "/" + sym + ".h5", sysid_offset, sysid_mod);
    return readOrderCSV(dir + "/" + sym + "_order.csv");
}

static std::unordered_map<int64_t, TradeRow> readTradeAuto(
        const std::string& dir, const std::string& sym,
        int64_t sysid_offset = 0, int64_t sysid_mod = 0) {
    if (hasH5(dir, sym)) return readTradeH5(dir + "/" + sym + ".h5", sysid_offset, sysid_mod);
    return readTradeCSV(dir + "/" + sym + "_trade.csv");
}

// ── 对比单只股票 ───────────────────────────────────────────────────────────────

static SymResult compareSymbol(const std::string& sym,
                                const std::string& dir_a,
                                const std::string& dir_b,
                                const std::string& file_prefix_a = "",
                                const std::string& file_prefix_b = "",
                                int64_t sysid_offset_b = 0,
                                int64_t sysid_mod_b = 0,
                                int64_t sysid_mod_a = 0) {
    SymResult res;
    res.sym = sym;
    const std::string sym_a = file_prefix_a + sym;
    const std::string sym_b = file_prefix_b + sym;  // e.g. "sh600000" for TL

    auto oa = readOrderAuto(dir_a, sym_a, 0, sysid_mod_a);
    auto ob = readOrderAuto(dir_b, sym_b, sysid_offset_b, sysid_mod_b);
    auto ta = readTradeAuto(dir_a, sym_a, 0, sysid_mod_a);
    auto tb = readTradeAuto(dir_b, sym_b, sysid_offset_b, sysid_mod_b);

    // ── Order 统计
    res.wt_a = oa.wt; res.wt_b = ob.wt;
    res.cl_a = oa.cl; res.cl_b = ob.cl;
    res.zb_a = oa.zb; res.zb_b = ob.zb;

    // 仅比对 wt 事件的 sysid（新委托集合）
    // 对于同一 sysid 取第一条 wt 记录
    std::unordered_map<int64_t, OrderRow> wt_a_map, wt_b_map;
    for (auto& [sid, rows] : oa.byId)
        for (auto& r : rows)
            if (r.ordertype == "wt") { wt_a_map[sid] = r; break; }
    for (auto& [sid, rows] : ob.byId)
        for (auto& r : rows)
            if (r.ordertype == "wt") { wt_b_map[sid] = r; break; }

    for (auto& [sid, ra] : wt_a_map) {
        auto it = wt_b_map.find(sid);
        if (it == wt_b_map.end()) { res.wt_a_only++; continue; }
        const auto& rb = it->second;
        if (ra.direction != rb.direction) res.dir_mis++;
        if (std::abs(ra.price - rb.price) > 0.005) res.price_mis++;
        if (ra.volume != rb.volume) res.vol_mis++;
        if (ra.type != rb.type && ra.type != 0 && rb.type != 0) res.cls_mis++;
    }
    for (auto& [sid, rb] : wt_b_map)
        if (!wt_a_map.count(sid)) res.wt_b_only++;

    // LOB 快照：对比末行 bdp1 / akp1
    if (oa.hasLast && ob.hasLast) {
        res.bdp1_a = oa.lastRow.bdp1; res.bdp1_b = ob.lastRow.bdp1;
        res.akp1_a = oa.lastRow.akp1; res.akp1_b = ob.lastRow.akp1;
        if (std::abs(oa.lastRow.bdp1 - ob.lastRow.bdp1) > 0.005) res.lob_bdp_mis = 1;
        if (std::abs(oa.lastRow.akp1 - ob.lastRow.akp1) > 0.005) res.lob_akp_mis = 1;
    }

    // ── Trade 统计
    res.trd_a = (int)ta.size();
    res.trd_b = (int)tb.size();

    for (auto& [sid, ra] : ta) {
        res.tvol_a += ra.trans_vlm;
        auto it = tb.find(sid);
        if (it == tb.end()) { res.trd_a_only++; continue; }
        const auto& rb = it->second;
        if (ra.trans_vlm != rb.trans_vlm) res.trd_vol_mis++;
        if (std::abs(ra.vwap - rb.vwap) > 0.005) res.trd_vwap_mis++;
    }
    for (auto& [sid, rb] : tb) {
        res.tvol_b += rb.trans_vlm;
        if (!ta.count(sid)) res.trd_b_only++;
    }

    return res;
}

// ── Markdown 报告 ─────────────────────────────────────────────────────────────

static std::string pct(int num, int den) {
    if (den == 0) return "—";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * num / den);
    return buf;
}

static void writeReport(const std::string& path,
                         const std::string& date,
                         const std::string& label_a,
                         const std::string& label_b,
                         const std::vector<SymResult>& results,
                         int top_n) {
    std::ofstream ofs(path);
    if (!ofs) { std::cerr << "无法写入: " << path << "\n"; return; }

    // ── 全市场汇总
    int total = (int)results.size();
    int perfect = 0;
    int64_t sum_wt_a = 0, sum_wt_b = 0;
    int64_t sum_cl_a = 0, sum_cl_b = 0;
    int64_t sum_zb_a = 0, sum_zb_b = 0;
    int64_t sum_trd_a = 0, sum_trd_b = 0;
    int64_t sum_tvol_a = 0, sum_tvol_b = 0;
    int sym_cnt_mis = 0, lob_mis = 0, wt_id_mis = 0, trd_mis = 0;

    for (auto& r : results) {
        if (r.perfect()) perfect++;
        sum_wt_a  += r.wt_a;  sum_wt_b  += r.wt_b;
        sum_cl_a  += r.cl_a;  sum_cl_b  += r.cl_b;
        sum_zb_a  += r.zb_a;  sum_zb_b  += r.zb_b;
        sum_trd_a += r.trd_a; sum_trd_b += r.trd_b;
        sum_tvol_a += r.tvol_a; sum_tvol_b += r.tvol_b;
        if (r.wt_a != r.wt_b || r.cl_a != r.cl_b || r.zb_a != r.zb_b) sym_cnt_mis++;
        if (r.lob_bdp_mis || r.lob_akp_mis) lob_mis++;
        if (r.wt_a_only || r.wt_b_only) wt_id_mis++;
        if (r.trd_a_only || r.trd_b_only || r.trd_vol_mis) trd_mis++;
    }

    ofs << "# LOB 对比报告\n\n";
    ofs << "日期: **" << (date.empty() ? "—" : date)
        << "** | A: **" << label_a
        << "** | B: **" << label_b << "**\n\n";

    ofs << "## 全市场汇总\n\n";
    ofs << "| 指标 | A (" << label_a << ") | B (" << label_b << ") |\n";
    ofs << "|---|---:|---:|\n";
    ofs << "| 股票数 | " << total << " | " << total << " |\n";
    ofs << "| 委托 (wt) | " << sum_wt_a << " | " << sum_wt_b << " |\n";
    ofs << "| 撤单 (cl) | " << sum_cl_a << " | " << sum_cl_b << " |\n";
    ofs << "| 成交事件 (zb) | " << sum_zb_a << " | " << sum_zb_b << " |\n";
    ofs << "| 成交订单数 | " << sum_trd_a << " | " << sum_trd_b << " |\n";
    ofs << "| 成交总量 | " << sum_tvol_a << " | " << sum_tvol_b << " |\n\n";

    ofs << "## 差异统计\n\n";
    ofs << "| 检查项 | 差异股票数 | 占比 |\n";
    ofs << "|---|---:|---:|\n";
    ofs << "| **完全匹配** | " << perfect << " | " << pct(perfect, total) << " |\n";
    ofs << "| 事件数量有差异 (wt/cl/zb) | " << sym_cnt_mis << " | " << pct(sym_cnt_mis, total) << " |\n";
    ofs << "| 委托 sysid 集合有差异 | " << wt_id_mis << " | " << pct(wt_id_mis, total) << " |\n";
    ofs << "| 末行 LOB 快照有差异 | " << lob_mis << " | " << pct(lob_mis, total) << " |\n";
    ofs << "| 成交记录有差异 | " << trd_mis << " | " << pct(trd_mis, total) << " |\n\n";

    // ── 差异最大的 Top-N 股票
    std::vector<const SymResult*> sorted;
    sorted.reserve(results.size());
    for (auto& r : results) if (!r.perfect()) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(),
              [](const SymResult* a, const SymResult* b){
                  return a->totalDiff() > b->totalDiff();
              });

    int show = std::min((int)sorted.size(), top_n);
    if (show > 0) {
        ofs << "## 差异明细（Top " << show << "，按差异量降序）\n\n";
        ofs << "| 股票 | wt差 | cl差 | zb差 | sysid仅A | sysid仅B"
               " | 方向错 | 价格错 | 量错 | 分类错"
               " | LOB买1差 | LOB卖1差"
               " | 成交差 | 成交仅A | 成交仅B | 量错 |\n";
        ofs << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";

        for (int i = 0; i < show; ++i) {
            auto& r = *sorted[i];
            auto d = [](int a, int b) -> std::string {
                int diff = a - b;
                if (diff == 0) return "0";
                char buf[32];
                snprintf(buf, sizeof(buf), "%+d", diff);
                return buf;
            };
            ofs << "| " << r.sym
                << " | " << d(r.wt_a, r.wt_b)
                << " | " << d(r.cl_a, r.cl_b)
                << " | " << d(r.zb_a, r.zb_b)
                << " | " << r.wt_a_only
                << " | " << r.wt_b_only
                << " | " << r.dir_mis
                << " | " << r.price_mis
                << " | " << r.vol_mis
                << " | " << r.cls_mis
                << " | " << (r.lob_bdp_mis ? "✗" : "✓")
                << " | " << (r.lob_akp_mis ? "✗" : "✓")
                << " | " << d(r.trd_a, r.trd_b)
                << " | " << r.trd_a_only
                << " | " << r.trd_b_only
                << " | " << r.trd_vol_mis
                << " |\n";
        }
        ofs << "\n";
    } else {
        ofs << "> 全部股票完全匹配 ✓\n\n";
    }

    // ── LOB 末行快照差异详情
    std::vector<const SymResult*> lob_diff;
    for (auto& r : results)
        if (r.lob_bdp_mis || r.lob_akp_mis) lob_diff.push_back(&r);
    std::sort(lob_diff.begin(), lob_diff.end(),
              [](const SymResult* a, const SymResult* b){
                  return std::abs(a->bdp1_a - a->bdp1_b) + std::abs(a->akp1_a - a->akp1_b)
                       > std::abs(b->bdp1_a - b->bdp1_b) + std::abs(b->akp1_a - b->akp1_b);
              });

    if (!lob_diff.empty()) {
        int show_lob = std::min((int)lob_diff.size(), top_n);
        ofs << "## LOB 末行快照差异（Top " << show_lob << "）\n\n";
        ofs << "| 股票 | 买1(A) | 买1(B) | 差 | 卖1(A) | 卖1(B) | 差 |\n";
        ofs << "|---|---:|---:|---:|---:|---:|---:|\n";
        for (int i = 0; i < show_lob; ++i) {
            auto& r = *lob_diff[i];
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "| %s | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f |\n",
                     r.sym.c_str(),
                     r.bdp1_a, r.bdp1_b, r.bdp1_a - r.bdp1_b,
                     r.akp1_a, r.akp1_b, r.akp1_a - r.akp1_b);
            ofs << buf;
        }
        ofs << "\n";
    }

    ofs << "---\n\n";
    ofs << "*Generated by lob_compare*\n";
    std::cerr << "[报告] 已写入: " << path << "\n";
}

// ── 扫描目录，找共同股票 ──────────────────────────────────────────────────────

static std::vector<std::string> discoverSymbols(const std::string& dir_a,
                                                  const std::string& dir_b,
                                                  const std::string& file_prefix_a = "",
                                                  const std::string& file_prefix_b = "") {
    auto listSyms = [](const std::string& dir, const std::string& strip_prefix) {
        std::set<std::string> s;
        for (const char* pat : {"/*_order.csv", "/*.h5"}) {
            std::string cmd = "ls \"" + dir + "\"" + pat + " 2>/dev/null";
            FILE* fp = popen(cmd.c_str(), "r");
            if (!fp) continue;
            char buf[512];
            while (fgets(buf, sizeof(buf), fp)) {
                std::string p(buf);
                while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
                size_t slash = p.rfind('/');
                std::string fname = (slash != std::string::npos) ? p.substr(slash+1) : p;
                std::string sym;
                size_t us = fname.rfind("_order.csv");
                if (us != std::string::npos) sym = fname.substr(0, us);
                else {
                    us = fname.rfind(".h5");
                    if (us != std::string::npos) sym = fname.substr(0, us);
                }
                if (!sym.empty()) {
                    // strip leading prefix (e.g. "sh"/"sz") if specified
                    if (!strip_prefix.empty() && sym.substr(0, strip_prefix.size()) == strip_prefix)
                        sym = sym.substr(strip_prefix.size());
                    s.insert(sym);
                }
            }
            pclose(fp);
        }
        return s;
    };

    auto sa = listSyms(dir_a, file_prefix_a);
    auto sb = listSyms(dir_b, file_prefix_b);

    std::vector<std::string> common;
    for (auto& sym : sa) if (sb.count(sym)) common.push_back(sym);
    std::sort(common.begin(), common.end());

    int only_a = (int)(sa.size() - common.size());
    int only_b = (int)(sb.size() - common.size());
    std::cerr << "[扫描] A=" << sa.size() << " 只  B=" << sb.size() << " 只  "
              << "共同=" << common.size() << " 只  "
              << "仅A=" << only_a << "  仅B=" << only_b << "\n";
    return common;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string dir_a, dir_b, output, date, label_a = "A", label_b = "B";
    std::string file_prefix_a, file_prefix_b;
    int64_t sysid_offset_b = 0;
    int64_t sysid_mod_a = 0, sysid_mod_b = 0;
    int n_workers = 40, top_n = 50;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dir-a"         && i+1 < argc) dir_a         = argv[++i];
        else if (a == "--dir-b"         && i+1 < argc) dir_b         = argv[++i];
        else if (a == "--output"        && i+1 < argc) output        = argv[++i];
        else if (a == "--date"          && i+1 < argc) date          = argv[++i];
        else if (a == "--label-a"       && i+1 < argc) label_a       = argv[++i];
        else if (a == "--label-b"       && i+1 < argc) label_b       = argv[++i];
        else if (a == "--file-prefix-a"  && i+1 < argc) file_prefix_a  = argv[++i];
        else if (a == "--file-prefix-b"  && i+1 < argc) file_prefix_b  = argv[++i];
        else if (a == "--sysid-offset-b" && i+1 < argc) sysid_offset_b = std::stoll(argv[++i]);
        else if (a == "--sysid-mod-a"    && i+1 < argc) sysid_mod_a    = std::stoll(argv[++i]);
        else if (a == "--sysid-mod-b"    && i+1 < argc) sysid_mod_b    = std::stoll(argv[++i]);
        else if (a == "--workers"        && i+1 < argc) n_workers      = std::stoi(argv[++i]);
        else if (a == "--top"            && i+1 < argc) top_n          = std::stoi(argv[++i]);
    }

    if (dir_a.empty() || dir_b.empty()) {
        std::cerr << "用法: lob_compare --dir-a <pathA> --dir-b <pathB> --output <report.md>\n"
                  << "                 [--label-a A] [--label-b B]\n"
                  << "                 [--file-prefix-a sh]         (dir-a 文件名前缀，如 sh/sz)\n"
                  << "                 [--file-prefix-b sh]         (dir-b 文件名前缀，如 sh/sz)\n"
                  << "                 [--sysid-mod-a 1000000000000]    (dir-a sysid % mod)\n"
                  << "                 [--sysid-offset-b 5000000000000] (dir-b sysid 减去偏移)\n"
                  << "                 [--sysid-mod-b 1000000000000]    (dir-b sysid % mod)\n"
                  << "                 [--date YYYYMMDD] [--workers N] [--top N]\n";
        return 1;
    }
    if (output.empty()) {
        output = "lob_compare_" + (date.empty() ? "report" : date) + ".md";
    }

    auto syms = discoverSymbols(dir_a, dir_b, file_prefix_a, file_prefix_b);
    if (syms.empty()) {
        std::cerr << "未找到共同股票，请检查路径\n";
        return 1;
    }

    int total = (int)syms.size();
    std::vector<SymResult> results(total);
    std::atomic<int> cnt{0};
    std::mutex mu;

    auto t0 = std::chrono::steady_clock::now();

    {
        Pool pool(std::min(n_workers, total));
        std::vector<std::future<void>> futs;
        futs.reserve(total);

        for (int i = 0; i < total; ++i) {
            futs.push_back(pool.submit([&, i]() {
                results[i] = compareSymbol(syms[i], dir_a, dir_b, file_prefix_a, file_prefix_b, sysid_offset_b, sysid_mod_b, sysid_mod_a);
                int n = ++cnt;
                if (n % 500 == 0 || n == total) {
                    double sec = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0).count();
                    std::lock_guard<std::mutex> lk(mu);
                    std::cerr << "  " << n << "/" << total << " (" << sec << "s)\n";
                }
            }));
        }
        for (auto& f : futs) f.get();
    }

    double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::cerr << "[完成] " << total << " 只股票，耗时 " << wall << "s\n";

    writeReport(output, date, label_a, label_b, results, top_n);
    return 0;
}
