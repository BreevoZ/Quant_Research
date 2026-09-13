/**
 * datacheck_main.cpp
 *
 * 统一数据检查工具：事件层对比 + 可选 LOB 重建
 *
 * 主源和比对源各做一次 IO 扫描：
 *   主源 → Events（用于事件对比）+ 原始 Records（如开启 --gen-lob，用于重建 LOB）
 *   比对源 → Events（仅用于对比）
 *
 * 用法:
 *   datacheck
 *     --symbol 600638 | --symbols a,b,c | --all
 *     --date 20260608 --market sh
 *     --primary-type flow|tl   --primary-path <dir|csv>
 *     --compare-type flow|tl   --compare-path <dir|csv>
 *     [--output-dir /reports/20260608]   # 汇总报告 _summary_{date}.md
 *     [--detail]                         # 同时写每只股票详情 MD
 *     [--gen-lob]                        # 根据主源重建 LOB
 *     [--lob-dir /lob/20260608]          # LOB 输出目录（默认 output-dir/lob）
 *     [--workers 32]
 *     [--min-src-events 100]
 *
 * 兼容别名:
 *   --flow-dir  ≡  --primary-type flow --primary-path
 *   --tl-file   ≡  --compare-type tl   --compare-path
 */

#include "lob_builder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>

// ── Thread pool ───────────────────────────────────────────────────────────────

struct ThreadPool {
    explicit ThreadPool(int n) {
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    { std::unique_lock<std::mutex> lk(mu_);
                      cv_.wait(lk, [this]{ return stop_ || !q_.empty(); });
                      if (stop_ && q_.empty()) return;
                      task = std::move(q_.front()); q_.pop(); }
                    task();
                }
            });
    }
    ~ThreadPool() {
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }
    template<class F>
    std::future<void> submit(F&& f) {
        auto pkg = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
        auto fut = pkg->get_future();
        { std::lock_guard<std::mutex> lk(mu_); q_.emplace([pkg]{ (*pkg)(); }); }
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

// ── Event types ───────────────────────────────────────────────────────────────

static const int64_t FLOW_CHAN_MOD = 1000000000000LL;

struct Event {
    double  timeSec;
    int64_t sysid;
    char    action;  // 'w'=委托 'c'=撤单 't'=成交
    char    dir;     // 'B' or 'S'
    double  price;
    int64_t volume;
};

using EventMap    = std::unordered_map<std::string, std::vector<Event>>;
using UnifiedMap  = std::unordered_map<std::string, std::vector<dc::UnifiedRecord>>;

// ── Primary source data (combined single-pass) ────────────────────────────────

struct PrimaryData {
    EventMap   events;
    UnifiedMap lob_recs;  // populated only when gen_lob=true
};

// ── Utilities ─────────────────────────────────────────────────────────────────

static double timeStrToSec(const std::string& t) {
    if (t.size() < 5) return -1.0;
    try {
        int h = std::stoi(t.substr(0, 2));
        int m = std::stoi(t.substr(3, 2));
        double s = (t.size() >= 8) ? std::stod(t.substr(6)) : 0.0;
        return h * 3600.0 + m * 60.0 + s;
    } catch (...) { return -1.0; }
}

static std::string secToHHMM(double sec) {
    int h = (int)sec / 3600, m = ((int)sec % 3600) / 60;
    char buf[8]; snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    return buf;
}

static void splitCSV(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.emplace_back(line, start, i - start);
            start = i + 1;
        }
    }
}

// ── Combined Flow parser ──────────────────────────────────────────────────────
// Single scan → Events + (optionally) FlowRecord per symbol

static PrimaryData parseFlowDirCombined(const std::string& dirPath,
                                         const std::vector<std::string>& symbols,
                                         const std::string& market,
                                         bool gen_lob,
                                         double lob_start_sec = 0.0,
                                         double lob_end_sec   = 1e9)
{
    std::string sfx = (market == "sz") ? ".SZ" : ".SH";
    std::unordered_map<std::string, std::string> flow_to_sym;
    for (const auto& s : symbols) flow_to_sym[s + sfx] = s;

    PrimaryData out;
    for (const auto& s : symbols) {
        out.events[s].reserve(30000);
        if (gen_lob) out.lob_recs[s].reserve(30000);
    }

    std::vector<std::string> files;
    DIR* d = opendir(dirPath.c_str());
    if (!d) { std::cerr << "Cannot open: " << dirPath << "\n"; return out; }
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.substr(n.size()-4) == ".csv")
            files.push_back(dirPath + "/" + n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());

    std::vector<std::string> fields;

    for (const auto& fp : files) {
        std::ifstream ifs(fp);
        if (!ifs) continue;
        std::string line;
        if (!std::getline(ifs, line)) continue;

        splitCSV(line, fields);
        std::unordered_map<std::string, int> col;
        for (int i = 0; i < (int)fields.size(); ++i) col[fields[i]] = i;
        if (!col.count("actionType")) continue;

        // column indices for Event fields
        int c_time = col["exchangeTime"], c_at = col["actionType"],
            c_dir  = col["dir"],          c_bid = col["bidId"],
            c_ask  = col["askId"],        c_prc = col["price"],
            c_vol  = col["volume"];

        // column indices for FlowRecord extra fields
        int c_loc  = col.count("localTime")      ? col["localTime"]      : -1;
        int c_td   = col.count("tradeDirection") ? col["tradeDirection"] : -1;
        int c_pt   = col.count("priceType")      ? col["priceType"]      : -1;
        int c_to   = col.count("turnover")        ? col["turnover"]       : -1;
        int c_seq  = col.count("appSeq")          ? col["appSeq"]         : -1;

        while (std::getline(ifs, line)) {
            if (line.empty()) continue;

            // fast reject: instrumentID is first field
            size_t comma = line.find(',');
            if (comma == std::string::npos) continue;
            auto sym_it = flow_to_sym.find(std::string(line.data(), comma));
            if (sym_it == flow_to_sym.end()) continue;

            splitCSV(line, fields);
            if ((int)fields.size() <= c_vol) continue;

            const std::string& sym = sym_it->second;

            // ── parse time
            const std::string& ts_raw = fields[c_time];
            auto sp = ts_raw.find(' ');
            double tsec = timeStrToSec(sp != std::string::npos
                                       ? ts_raw.substr(sp + 1) : ts_raw);
            if (tsec < 0) continue;

            // ── parse common fields
            int    at   = std::stoi(fields[c_at]);
            int    diri = std::stoi(fields[c_dir]);
            char   dir  = (diri == 0) ? 'B' : 'S';  // Flow: 0=BUY 1=SELL
            double prc  = std::stod(fields[c_prc]);
            int64_t vol = std::stoll(fields[c_vol]);
            int64_t bid = fields[c_bid].empty() ? 0 : std::stoll(fields[c_bid]);
            int64_t ask = fields[c_ask].empty() ? 0 : std::stoll(fields[c_ask]);

            auto norm = [](int64_t id) {
                return (id > FLOW_CHAN_MOD) ? id % FLOW_CHAN_MOD : id;
            };

            // ── add Event
            auto& evs = out.events[sym];
            if (at == 0) {
                int64_t raw = (bid > 0) ? bid : ask;
                evs.push_back({tsec, norm(raw), 'w', (bid > 0 ? 'B' : 'S'), prc, vol});
            } else if (at == 1) {
                int64_t raw = (bid > 0) ? bid : ask;
                evs.push_back({tsec, norm(raw), 'c', dir, prc, vol});
            } else if (at == 2) {
                evs.push_back({tsec, norm(bid), 't', 'B', prc, vol});
                evs.push_back({tsec, norm(ask), 't', 'S', prc, vol});
            }

            // ── add UnifiedRecord (if gen_lob, within time range)
            if (gen_lob && tsec >= lob_start_sec && tsec < lob_end_sec) {
                dc::UnifiedRecord ur;
                ur.seqNo = (c_seq >= 0 && (int)fields.size() > c_seq && !fields[c_seq].empty())
                           ? std::stoll(fields[c_seq]) : 0;
                {
                    auto sp2 = ts_raw.find(' ');
                    ur.time = (sp2 != std::string::npos) ? ts_raw.substr(sp2 + 1) : ts_raw;
                }
                ur.timeSeconds    = tsec;
                ur.actionType     = at;
                ur.direction      = dir;
                ur.price          = prc;
                ur.intPrice       = static_cast<int64_t>((prc + 1e-6) * 100);
                ur.volume         = vol;
                ur.turnover       = (c_to >= 0 && (int)fields.size() > c_to && !fields[c_to].empty())
                                    ? std::stod(fields[c_to]) : 0.0;
                ur.priceType      = (c_pt >= 0 && (int)fields.size() > c_pt && !fields[c_pt].empty())
                                    ? fields[c_pt][0] : '2';
                ur.tradeDirection = (c_td >= 0 && (int)fields.size() > c_td && !fields[c_td].empty())
                                    ? std::stoi(fields[c_td]) : 0;
                if (at == 0 || at == 1) {
                    ur.sysid = norm((bid > 0) ? bid : ask);
                } else {
                    ur.buyId  = norm(bid);
                    ur.sellId = norm(ask);
                }
                out.lob_recs[sym].push_back(std::move(ur));
            }
        }
    }

    for (auto& [sym, evs] : out.events)
        std::sort(evs.begin(), evs.end(),
                  [](const Event& a, const Event& b){ return a.timeSec < b.timeSec; });
    return out;
}

// ── Combined TL SH parser ─────────────────────────────────────────────────────
// Parallel byte-range scan → Events + (optionally) SHTickRecord per symbol
//
// LOB 重建顺序无关：LobBuilder::build() 会按 seqNo(BizIndex) 重新排序，
// 所以这里分块并行读取、最后简单 concat 各线程结果即可，不需要保证块内/块间顺序。

struct FileChunk { std::streampos start, end; };

// 把 [headerEnd, fileSize) 切成 n 段，每段边界都对齐到完整行（向后找到下一个换行符）
static std::vector<FileChunk> computeFileChunks(const std::string& filePath,
                                                  std::streampos headerEnd, int n) {
    std::ifstream ifs(filePath, std::ios::binary);
    ifs.seekg(0, std::ios::end);
    std::streampos fileSize = ifs.tellg();

    std::vector<FileChunk> chunks;
    if (n <= 1 || fileSize <= headerEnd) {
        chunks.push_back({headerEnd, fileSize});
        return chunks;
    }
    int64_t total = (int64_t)fileSize - (int64_t)headerEnd;
    int64_t chunkSize = total / n;
    std::streampos prevEnd = headerEnd;
    std::string tmp;
    for (int i = 0; i < n; ++i) {
        std::streampos realEnd;
        if (i == n - 1) {
            realEnd = fileSize;
        } else {
            std::streampos nominalEnd = headerEnd + (std::streamoff)(chunkSize * (i + 1));
            ifs.seekg(nominalEnd);
            std::getline(ifs, tmp);  // 消费掉这一行的剩余部分，对齐到行边界
            realEnd = ifs.eof() ? fileSize : ifs.tellg();
        }
        chunks.push_back({prevEnd, realEnd});
        prevEnd = realEnd;
        if (realEnd >= fileSize) break;
    }
    return chunks;
}

static PrimaryData parseTLChunk(const std::string& filePath, FileChunk chunk,
                                 const std::unordered_set<std::string>& sym_set,
                                 int c_sec, int c_tick, int c_type, int c_buy, int c_sell,
                                 int c_prc, int c_qty, int c_flag, int c_biz, int c_tm,
                                 bool gen_lob, double lob_start_sec, double lob_end_sec)
{
    PrimaryData out;
    std::ifstream ifs(filePath, std::ios::binary);
    ifs.seekg(chunk.start);

    std::string line;
    std::vector<std::string> fields;
    while (true) {
        std::streampos pos = ifs.tellg();
        if (pos == std::streampos(-1) || pos >= chunk.end) break;
        if (!std::getline(ifs, line)) break;
        if (line.empty()) continue;
        splitCSV(line, fields);
        if ((int)fields.size() <= c_flag) continue;

        const std::string& sec = fields[c_sec];
        if (!sym_set.count(sec)) continue;
        const std::string& type = fields[c_type];
        if (type == "S") continue;

        double  tsec = timeStrToSec(fields[c_tick]);
        if (tsec < 0) continue;
        double  prc  = std::stod(fields[c_prc]);
        int64_t vol  = std::stoll(fields[c_qty]);
        int64_t buy  = std::stoll(fields[c_buy]);
        int64_t sell = std::stoll(fields[c_sell]);
        char    flag = fields[c_flag].empty() ? ' ' : fields[c_flag][0];

        // ── add Event
        auto& evs = out.events[sec];
        if (type == "A") {
            char dir = (flag == 'B') ? 'B' : 'S';
            evs.push_back({tsec, (dir=='B' ? buy : sell), 'w', dir, prc, vol});
        } else if (type == "D") {
            char dir = (buy > 0 && sell == 0) ? 'B' : 'S';
            evs.push_back({tsec, (buy > 0 ? buy : sell), 'c', dir, prc, vol});
        } else if (type == "T") {
            evs.push_back({tsec, buy,  't', 'B', prc, vol});
            evs.push_back({tsec, sell, 't', 'S', prc, vol});
        }

        // ── add UnifiedRecord (if gen_lob, within time range)
        if (gen_lob && tsec >= lob_start_sec && tsec < lob_end_sec) {
            dc::UnifiedRecord ur;
            ur.seqNo       = (c_biz >= 0 && (int)fields.size() > c_biz && !fields[c_biz].empty())
                             ? std::stoll(fields[c_biz]) : 0;
            ur.time        = fields[c_tick];
            ur.timeSeconds = tsec;
            ur.price       = prc;
            ur.intPrice    = static_cast<int64_t>((prc + 1e-6) * 100);
            ur.volume      = vol;
            ur.turnover    = (c_tm >= 0 && (int)fields.size() > c_tm && !fields[c_tm].empty())
                             ? std::stod(fields[c_tm]) : 0.0;
            ur.priceType   = '2';
            if (type == "A") {
                ur.actionType = 0;
                ur.direction  = flag;
                ur.sysid      = (flag == 'B') ? buy : sell;
            } else if (type == "D") {
                ur.actionType = 1;
                ur.direction  = (buy > 0 && sell == 0) ? 'B' : 'S';
                ur.sysid      = (buy > 0) ? buy : sell;
            } else {  // T
                ur.actionType    = 2;
                ur.buyId         = buy;
                ur.sellId        = sell;
                ur.direction     = flag;
                ur.tradeDirection = (flag == 'B') ? 1 : 2;
            }
            out.lob_recs[sec].push_back(std::move(ur));
        }
    }
    return out;
}

static PrimaryData parseTLFileCombined(const std::string& filePath,
                                        const std::vector<std::string>& symbols,
                                        bool gen_lob,
                                        double lob_start_sec = 0.0,
                                        double lob_end_sec   = 1e9,
                                        int n_workers = 1)
{
    std::unordered_set<std::string> sym_set(symbols.begin(), symbols.end());
    PrimaryData out;
    for (const auto& s : symbols) {
        out.events[s].reserve(30000);
        if (gen_lob) out.lob_recs[s].reserve(30000);
    }

    std::ifstream ifs(filePath);
    if (!ifs) { std::cerr << "Cannot open: " << filePath << "\n"; return out; }

    std::string line;
    if (!std::getline(ifs, line)) return out;
    std::vector<std::string> fields;
    splitCSV(line, fields);
    std::unordered_map<std::string, int> col;
    for (int i = 0; i < (int)fields.size(); ++i) col[fields[i]] = i;
    if (!col.count("SecurityID")) { std::cerr << "TL header missing SecurityID\n"; return out; }

    int c_sec  = col["SecurityID"],  c_tick = col["TickTime"],
        c_type = col["Type"],        c_buy  = col["BuyOrderNO"],
        c_sell = col["SellOrderNO"], c_prc  = col["Price"],
        c_qty  = col["Qty"],         c_flag = col["TickBSFlag"];
    int c_biz  = col.count("BizIndex")    ? col["BizIndex"]    : -1;
    int c_tm   = col.count("TradeMoney")  ? col["TradeMoney"]  : -1;

    std::streampos headerEnd = ifs.tellg();
    ifs.close();

    int n = std::max(1, n_workers);
    auto chunks = computeFileChunks(filePath, headerEnd, n);

    std::vector<std::future<PrimaryData>> futs;
    futs.reserve(chunks.size());
    for (auto& ch : chunks) {
        futs.push_back(std::async(std::launch::async, parseTLChunk, filePath, ch,
                                   std::cref(sym_set), c_sec, c_tick, c_type, c_buy, c_sell,
                                   c_prc, c_qty, c_flag, c_biz, c_tm,
                                   gen_lob, lob_start_sec, lob_end_sec));
    }
    for (auto& f : futs) {
        PrimaryData part = f.get();
        for (auto& [sym, evs] : part.events) {
            auto& dst = out.events[sym];
            dst.insert(dst.end(), evs.begin(), evs.end());
        }
        if (gen_lob) {
            for (auto& [sym, recs] : part.lob_recs) {
                auto& dst = out.lob_recs[sym];
                dst.insert(dst.end(),
                           std::make_move_iterator(recs.begin()),
                           std::make_move_iterator(recs.end()));
            }
        }
    }

    for (auto& [sym, evs] : out.events)
        std::sort(evs.begin(), evs.end(),
                  [](const Event& a, const Event& b){ return a.timeSec < b.timeSec; });
    return out;
}

// ── Combined TL SZ parser ─────────────────────────────────────────────────────
// 深圳行情拆成两个文件：mdl_6_33(委托) + mdl_6_36(成交/撤单)，各自的 SeqNo 是独立序列，
// 不能像上海一样直接用 BizIndex 全局排序。两个文件各自并行分块读取后，
// 在合并阶段按 (timeSeconds, actionType, sysid) 重新排序并重新分配 seqNo，
// LobBuilder::build() 内部再按 seqNo 排一次就是无操作（已经有序），结果等价于全局正确顺序。
//
// Side: 49='1'=买 50='2'=卖；OrdType: 49=市价 85=本方最优 其余=限价
// ExecType（成交文件）: 52='4'=撤单 70='F'=成交

static const int SZ_SIDE_BUY    = 49;
static const int SZ_EXEC_CANCEL = 52;
static const int SZ_EXEC_TRADE  = 70;

static std::string padSZSecurityID(const std::string& s) {
    if (s.size() >= 6) return s;
    return std::string(6 - s.size(), '0') + s;
}

static PrimaryData parseSZOrderChunk(const std::string& filePath, FileChunk chunk,
                                      const std::unordered_set<std::string>& sym_set,
                                      int c_sec, int c_prc, int c_qty, int c_side,
                                      int c_time, int c_ordtype, int c_appl,
                                      bool gen_lob, double lob_start_sec, double lob_end_sec)
{
    PrimaryData out;
    std::ifstream ifs(filePath, std::ios::binary);
    ifs.seekg(chunk.start);

    std::string line;
    std::vector<std::string> fields;
    while (true) {
        std::streampos pos = ifs.tellg();
        if (pos == std::streampos(-1) || pos >= chunk.end) break;
        if (!std::getline(ifs, line)) break;
        if (line.empty()) continue;
        splitCSV(line, fields);
        if ((int)fields.size() <= c_appl) continue;

        std::string sec = padSZSecurityID(fields[c_sec]);
        if (!sym_set.count(sec)) continue;

        double  tsec = timeStrToSec(fields[c_time]);
        if (tsec < 0) continue;
        double  prc   = std::stod(fields[c_prc]);
        int64_t vol   = std::stoll(fields[c_qty]);
        int     side  = std::stoi(fields[c_side]);
        int64_t appl  = std::stoll(fields[c_appl]);
        char    dir   = (side == SZ_SIDE_BUY) ? 'B' : 'S';

        auto& evs = out.events[sec];
        evs.push_back({tsec, appl, 'w', dir, prc, vol});

        if (gen_lob && tsec >= lob_start_sec && tsec < lob_end_sec) {
            dc::UnifiedRecord ur;
            ur.seqNo       = appl;  // 仅供调试，最终顺序由合并后的重排序决定
            ur.time        = fields[c_time];
            ur.timeSeconds = tsec;
            ur.actionType  = 0;
            ur.direction   = dir;
            ur.sysid       = appl;
            ur.price       = prc;
            ur.intPrice    = static_cast<int64_t>((prc + 1e-6) * 100);
            ur.volume      = vol;
            int ordtype = (c_ordtype >= 0 && (int)fields.size() > c_ordtype && !fields[c_ordtype].empty())
                          ? std::stoi(fields[c_ordtype]) : 0;
            ur.priceType = (ordtype == 49) ? '1' : (ordtype == 85) ? 'U' : '2';
            out.lob_recs[sec].push_back(std::move(ur));
        }
    }
    return out;
}

static PrimaryData parseSZTradeChunk(const std::string& filePath, FileChunk chunk,
                                      const std::unordered_set<std::string>& sym_set,
                                      int c_bid, int c_offer, int c_sec, int c_prc,
                                      int c_qty, int c_exec, int c_time, int c_appl,
                                      bool gen_lob, double lob_start_sec, double lob_end_sec)
{
    PrimaryData out;
    std::ifstream ifs(filePath, std::ios::binary);
    ifs.seekg(chunk.start);

    std::string line;
    std::vector<std::string> fields;
    while (true) {
        std::streampos pos = ifs.tellg();
        if (pos == std::streampos(-1) || pos >= chunk.end) break;
        if (!std::getline(ifs, line)) break;
        if (line.empty()) continue;
        splitCSV(line, fields);
        if ((int)fields.size() <= c_exec) continue;

        std::string sec = padSZSecurityID(fields[c_sec]);
        if (!sym_set.count(sec)) continue;

        double  tsec = timeStrToSec(fields[c_time]);
        if (tsec < 0) continue;
        double  prc    = std::stod(fields[c_prc]);
        int64_t vol    = std::stoll(fields[c_qty]);
        int64_t bidId  = std::stoll(fields[c_bid]);
        int64_t askId  = std::stoll(fields[c_offer]);
        int     exec   = std::stoi(fields[c_exec]);
        int64_t appl   = std::stoll(fields[c_appl]);

        auto& evs = out.events[sec];
        if (exec == SZ_EXEC_CANCEL) {
            int64_t id = (bidId > 0) ? bidId : askId;
            char dir = (bidId > 0) ? 'B' : 'S';
            evs.push_back({tsec, id, 'c', dir, prc, vol});
        } else if (exec == SZ_EXEC_TRADE) {
            evs.push_back({tsec, bidId, 't', 'B', prc, vol});
            evs.push_back({tsec, askId, 't', 'S', prc, vol});
        } else {
            continue;
        }

        if (gen_lob && tsec >= lob_start_sec && tsec < lob_end_sec) {
            dc::UnifiedRecord ur;
            ur.time        = fields[c_time];
            ur.timeSeconds = tsec;
            ur.price       = prc;
            ur.intPrice    = static_cast<int64_t>((prc + 1e-6) * 100);
            ur.volume      = vol;
            ur.priceType   = '2';
            // 消息自身的 ApplSeqNum：同一 Channel 内委托/成交/撤单共享一个全局递增序号
            // （已用全市场数据验证：单 Channel 内 order+trade 的 ApplSeqNum 合并后是
            //  1..N 无重复无空洞的连续序列），直接拿来做排序键即可还原真实时间顺序，
            // 不需要再靠 (时间, 类型, sysid) 去猜。
            ur.seqNo = appl;
            if (exec == SZ_EXEC_CANCEL) {
                ur.actionType = 1;
                if (bidId > 0) { ur.direction = 'B'; ur.sysid = bidId; }
                else            { ur.direction = 'S'; ur.sysid = askId; }
            } else {  // SZ_EXEC_TRADE
                ur.actionType    = 2;
                ur.buyId         = bidId;
                ur.sellId        = askId;
                ur.turnover      = prc * vol;
            }
            out.lob_recs[sec].push_back(std::move(ur));
        }
    }
    return out;
}

static PrimaryData parseTLSZFilesCombined(const std::string& orderPath, const std::string& tradePath,
                                           const std::vector<std::string>& symbols,
                                           bool gen_lob,
                                           double lob_start_sec = 0.0,
                                           double lob_end_sec   = 1e9,
                                           int n_workers = 1)
{
    std::unordered_set<std::string> sym_set;
    for (const auto& s : symbols) sym_set.insert(padSZSecurityID(s));
    PrimaryData out;
    for (const auto& s : symbols) {
        out.events[s].reserve(30000);
        if (gen_lob) out.lob_recs[s].reserve(30000);
    }

    // ── order file header ──
    std::ifstream ifsO(orderPath);
    if (!ifsO) { std::cerr << "Cannot open: " << orderPath << "\n"; return out; }
    std::string line;
    if (!std::getline(ifsO, line)) return out;
    std::vector<std::string> fields;
    splitCSV(line, fields);
    std::unordered_map<std::string, int> colO;
    for (int i = 0; i < (int)fields.size(); ++i) colO[fields[i]] = i;
    if (!colO.count("SecurityID") || !colO.count("ApplSeqNum")) {
        std::cerr << "SZ order header missing SecurityID/ApplSeqNum\n"; return out;
    }
    int oc_sec = colO["SecurityID"], oc_prc = colO["Price"], oc_qty = colO["OrderQty"],
        oc_side = colO["Side"], oc_time = colO["TransactTime"], oc_appl = colO["ApplSeqNum"];
    int oc_ordtype = colO.count("OrdType") ? colO["OrdType"] : -1;
    std::streampos orderHeaderEnd = ifsO.tellg();
    ifsO.close();

    // ── trade file header ──
    std::ifstream ifsT(tradePath);
    if (!ifsT) { std::cerr << "Cannot open: " << tradePath << "\n"; return out; }
    if (!std::getline(ifsT, line)) return out;
    splitCSV(line, fields);
    std::unordered_map<std::string, int> colT;
    for (int i = 0; i < (int)fields.size(); ++i) colT[fields[i]] = i;
    if (!colT.count("SecurityID") || !colT.count("ExecType") || !colT.count("ApplSeqNum")) {
        std::cerr << "SZ trade header missing SecurityID/ExecType/ApplSeqNum\n"; return out;
    }
    int tc_bid = colT["BidApplSeqNum"], tc_offer = colT["OfferApplSeqNum"],
        tc_sec = colT["SecurityID"], tc_prc = colT["LastPx"], tc_qty = colT["LastQty"],
        tc_exec = colT["ExecType"], tc_time = colT["TransactTime"], tc_appl = colT["ApplSeqNum"];
    std::streampos tradeHeaderEnd = ifsT.tellg();
    ifsT.close();

    int nEach = std::max(1, n_workers / 2);
    auto orderChunks = computeFileChunks(orderPath, orderHeaderEnd, nEach);
    auto tradeChunks = computeFileChunks(tradePath, tradeHeaderEnd, nEach);

    std::vector<std::future<PrimaryData>> futs;
    futs.reserve(orderChunks.size() + tradeChunks.size());
    for (auto& ch : orderChunks) {
        futs.push_back(std::async(std::launch::async, parseSZOrderChunk, orderPath, ch,
                                   std::cref(sym_set), oc_sec, oc_prc, oc_qty, oc_side,
                                   oc_time, oc_ordtype, oc_appl,
                                   gen_lob, lob_start_sec, lob_end_sec));
    }
    for (auto& ch : tradeChunks) {
        futs.push_back(std::async(std::launch::async, parseSZTradeChunk, tradePath, ch,
                                   std::cref(sym_set), tc_bid, tc_offer, tc_sec, tc_prc,
                                   tc_qty, tc_exec, tc_time, tc_appl,
                                   gen_lob, lob_start_sec, lob_end_sec));
    }
    for (auto& f : futs) {
        PrimaryData part = f.get();
        for (auto& [sym, evs] : part.events) {
            auto& dst = out.events[sym];
            dst.insert(dst.end(), evs.begin(), evs.end());
        }
        if (gen_lob) {
            for (auto& [sym, recs] : part.lob_recs) {
                auto& dst = out.lob_recs[sym];
                dst.insert(dst.end(),
                           std::make_move_iterator(recs.begin()),
                           std::make_move_iterator(recs.end()));
            }
        }
    }

    for (auto& [sym, evs] : out.events)
        std::sort(evs.begin(), evs.end(),
                  [](const Event& a, const Event& b){ return a.timeSec < b.timeSec; });

    // 注意：lob_recs 不需要在这里排序——每条记录的 seqNo 已经是该消息在所属 Channel 内
    // 的原始 ApplSeqNum（委托/成交/撤单共享同一个全局递增序号，已用全市场数据验证过
    // 同一 Channel 内 order+trade 的 ApplSeqNum 合并后是 1..N 无重复无空洞的连续序列），
    // LobBuilder::build() 内部会按 seqNo 排序，直接复用即可还原真实时间顺序。
    return out;
}

// ── Symbol discovery from TL file ────────────────────────────────────────────

static std::vector<std::string> discoverTLSymbols(const std::string& filePath) {
    std::ifstream ifs(filePath);
    if (!ifs) return {};
    std::string line;
    if (!std::getline(ifs, line)) return {};
    std::vector<std::string> fields;
    splitCSV(line, fields);
    std::unordered_map<std::string, int> col;
    for (int i = 0; i < (int)fields.size(); ++i) col[fields[i]] = i;
    if (!col.count("SecurityID") || !col.count("Type")) return {};
    int c_sec = col["SecurityID"], c_type = col["Type"];

    std::unordered_set<std::string> seen;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        splitCSV(line, fields);
        if ((int)fields.size() <= c_type) continue;
        if (fields[c_type] == "S") continue;
        seen.insert(fields[c_sec]);
    }
    std::vector<std::string> result(seen.begin(), seen.end());
    std::sort(result.begin(), result.end());
    return result;
}

static std::vector<std::string> discoverSZSymbols(const std::string& orderFilePath) {
    std::ifstream ifs(orderFilePath);
    if (!ifs) return {};
    std::string line;
    if (!std::getline(ifs, line)) return {};
    std::vector<std::string> fields;
    splitCSV(line, fields);
    std::unordered_map<std::string, int> col;
    for (int i = 0; i < (int)fields.size(); ++i) col[fields[i]] = i;
    if (!col.count("SecurityID")) return {};
    int c_sec = col["SecurityID"];

    std::unordered_set<std::string> seen;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        splitCSV(line, fields);
        if ((int)fields.size() <= c_sec) continue;
        seen.insert(padSZSecurityID(fields[c_sec]));
    }
    std::vector<std::string> result(seen.begin(), seen.end());
    std::sort(result.begin(), result.end());
    return result;
}

// ── Layer 1: time-window event counts ────────────────────────────────────────

struct WinCounts {
    std::string label;
    int64_t f_wt=0, f_cl=0, f_tr=0;
    int64_t t_wt=0, t_cl=0, t_tr=0;
};

static std::vector<WinCounts> layer1(
    const std::vector<Event>& fe, const std::vector<Event>& te,
    double start, double end, double interval, bool skip_lunch)
{
    static const double LUNCH_S = 11*3600.0 + 30*60.0;
    static const double LUNCH_E = 13*3600.0;
    struct Win { double ws, we; };
    std::vector<Win> wins;
    for (double ws = start; ws < end; ws += interval) {
        double we = std::min(ws + interval, end);
        if (skip_lunch && ws >= LUNCH_S && we <= LUNCH_E) continue;
        wins.push_back({ws, we});
    }
    std::vector<WinCounts> result(wins.size());
    for (size_t i = 0; i < wins.size(); ++i)
        result[i].label = secToHHMM(wins[i].ws) + "~" + secToHHMM(wins[i].we);

    auto sweep = [&](const std::vector<Event>& evs, bool is_src) {
        size_t wi = 0, ei = 0;
        while (wi < wins.size() && ei < evs.size()) {
            double t = evs[ei].timeSec;
            if (t < wins[wi].ws) { ++ei; continue; }
            if (t >= wins[wi].we) { ++wi; continue; }
            auto& wc = result[wi];
            if (is_src) {
                if      (evs[ei].action == 'w') ++wc.f_wt;
                else if (evs[ei].action == 'c') ++wc.f_cl;
                else                            ++wc.f_tr;
            } else {
                if      (evs[ei].action == 'w') ++wc.t_wt;
                else if (evs[ei].action == 'c') ++wc.t_cl;
                else                            ++wc.t_tr;
            }
            ++ei;
        }
    };
    sweep(fe, true);
    sweep(te, false);
    return result;
}

// ── EventIndex + Layer 2 + Layer 3 ───────────────────────────────────────────

struct SampleEvent {
    double timeSec; int64_t sysid; char dir; double price; int64_t volume;
};

struct EventIndex {
    int64_t n_wt=0, n_cl=0, n_tr=0;
    std::unordered_map<int64_t, SampleEvent> wt_map;
    std::unordered_map<int64_t, int64_t>     cl_vol;
    std::unordered_map<int64_t, SampleEvent> cl_sample;
    std::unordered_set<int64_t>              tr_set;
};

static EventIndex buildIndex(const std::vector<Event>& evs) {
    EventIndex idx;
    idx.wt_map.reserve(evs.size()/2+1);
    idx.cl_vol.reserve(evs.size()/8+1);
    idx.cl_sample.reserve(evs.size()/8+1);
    idx.tr_set.reserve(evs.size()/4+1);
    for (const auto& e : evs) {
        if (e.action == 'w') {
            ++idx.n_wt;
            idx.wt_map.emplace(e.sysid, SampleEvent{e.timeSec, e.sysid, e.dir, e.price, e.volume});
        } else if (e.action == 'c') {
            ++idx.n_cl;
            idx.cl_vol[e.sysid] += e.volume;
            idx.cl_sample.emplace(e.sysid, SampleEvent{e.timeSec, e.sysid, e.dir, e.price, e.volume});
        } else {
            ++idx.n_tr;
            idx.tr_set.insert(e.sysid);
        }
    }
    return idx;
}

struct Coverage {
    int64_t both=0, f_only=0, t_only=0;
    std::vector<SampleEvent> f_only_samples, t_only_samples;
};

struct AttrStats {
    int64_t common=0, price_match=0, vol_match=0, dir_match=0, all_match=0;
};

struct AllStats { Coverage wt_cov, cl_cov, tr_cov; AttrStats wt_attr, cl_attr; };

static AllStats computeStats(const EventIndex& si, const EventIndex& ci, int max_samples=10) {
    AllStats r{};
    for (const auto& [id, sev] : si.wt_map) {
        auto it = ci.wt_map.find(id);
        if (it == ci.wt_map.end()) {
            ++r.wt_cov.f_only;
            if ((int)r.wt_cov.f_only_samples.size() < max_samples) r.wt_cov.f_only_samples.push_back(sev);
        } else {
            ++r.wt_cov.both; ++r.wt_attr.common;
            bool pm = std::abs(sev.price - it->second.price) < 0.001;
            bool vm = sev.volume == it->second.volume;
            bool dm = sev.dir    == it->second.dir;
            if (pm) ++r.wt_attr.price_match;
            if (vm) ++r.wt_attr.vol_match;
            if (dm) ++r.wt_attr.dir_match;
            if (pm && vm && dm) ++r.wt_attr.all_match;
        }
    }
    for (const auto& [id, cev] : ci.wt_map)
        if (!si.wt_map.count(id)) {
            ++r.wt_cov.t_only;
            if ((int)r.wt_cov.t_only_samples.size() < max_samples) r.wt_cov.t_only_samples.push_back(cev);
        }
    for (const auto& [id, sv] : si.cl_vol) {
        auto it = ci.cl_vol.find(id);
        if (it == ci.cl_vol.end()) {
            ++r.cl_cov.f_only;
            if ((int)r.cl_cov.f_only_samples.size() < max_samples) {
                auto sit = si.cl_sample.find(id);
                if (sit != si.cl_sample.end()) r.cl_cov.f_only_samples.push_back(sit->second);
            }
        } else {
            ++r.cl_cov.both; ++r.cl_attr.common;
            if (sv == it->second) ++r.cl_attr.vol_match;
        }
    }
    for (const auto& [id, cv] : ci.cl_vol)
        if (!si.cl_vol.count(id)) {
            ++r.cl_cov.t_only;
            if ((int)r.cl_cov.t_only_samples.size() < max_samples) {
                auto cit = ci.cl_sample.find(id);
                if (cit != ci.cl_sample.end()) r.cl_cov.t_only_samples.push_back(cit->second);
            }
        }
    for (int64_t id : si.tr_set) {
        if (ci.tr_set.count(id)) ++r.tr_cov.both; else ++r.tr_cov.f_only;
    }
    for (int64_t id : ci.tr_set)
        if (!si.tr_set.count(id)) ++r.tr_cov.t_only;
    return r;
}

// ── Report types ──────────────────────────────────────────────────────────────

struct SummaryRow {
    std::string sym;
    int64_t s_wt=0, s_cl=0, s_tr=0, c_wt=0, c_cl=0, c_tr=0;
    int64_t wt_both=0, wt_s_only=0, wt_c_only=0, wt_common=0, wt_all_match=0;
    int64_t cl_both=0, cl_s_only=0, cl_c_only=0, cl_common=0, cl_vol_match=0;
    int64_t tr_both=0, tr_s_only=0, tr_c_only=0;
};

static bool rowIsClean(const SummaryRow& r) {
    return r.wt_s_only==0 && r.wt_c_only==0 && r.wt_all_match==r.wt_common &&
           r.cl_s_only==0 && r.cl_c_only==0 && r.cl_vol_match==r.cl_common &&
           r.tr_s_only==0 && r.tr_c_only==0;
}

// ── Markdown output ───────────────────────────────────────────────────────────

static void writeSampleTable(std::ofstream& f,
                              const std::vector<SampleEvent>& samples,
                              const std::string& title) {
    if (samples.empty()) return;
    f << "**" << title << "** (" << samples.size() << " 条样例)\n\n"
      << "| sysid | 时间 | 方向 | 价格 | 量 |\n|---:|---|:---:|---:|---:|\n";
    for (const auto& s : samples) {
        int h = (int)s.timeSec/3600, m = ((int)s.timeSec%3600)/60;
        double sec = s.timeSec - h*3600 - m*60;
        char tbuf[16]; snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%05.2f", h, m, sec);
        f << "| " << s.sysid << " | " << tbuf << " | " << s.dir
          << " | " << std::fixed << std::setprecision(2) << s.price
          << " | " << s.volume << " |\n";
    }
    f << "\n";
}

static void writeSummary(const std::string& path,
                          const std::string& date,
                          const std::string& market,
                          const std::string& src_label,
                          const std::string& cmp_label,
                          std::vector<SummaryRow>& rows,
                          const std::string& lob_info = "")
{
    std::sort(rows.begin(), rows.end(),
              [](const SummaryRow& a, const SummaryRow& b){ return a.sym < b.sym; });
    std::ofstream f(path);
    if (!f) { std::cerr << "Cannot write summary: " << path << "\n"; return; }

    auto pct = [](int64_t n, int64_t d) -> std::string {
        if (d == 0) return "—";
        char buf[16]; snprintf(buf, sizeof(buf), "%.1f%%", n*100.0/d); return buf;
    };

    int64_t tot = (int64_t)rows.size(), perfect = 0;
    int64_t s_wt=0, s_cl=0, s_tr=0, c_wt=0, c_cl=0, c_tr=0;
    for (const auto& r : rows) {
        if (rowIsClean(r)) ++perfect;
        s_wt+=r.s_wt; s_cl+=r.s_cl; s_tr+=r.s_tr;
        c_wt+=r.c_wt; c_cl+=r.c_cl; c_tr+=r.c_tr;
    }

    std::string mkt_upper = market; for (auto& c : mkt_upper) c = toupper(c);
    f << "# 全市场事件层对比汇总\n\n"
      << "**日期**: " << date << "　**市场**: " << mkt_upper
      << "　**主源**: " << src_label
      << "　**比对源**: " << cmp_label << "　**股票数**: " << tot;
    if (!lob_info.empty()) f << "　**LOB时段**: " << lob_info;
    f << "\n\n"
      << "## 一、总览\n\n"
      << "| 指标 | " << src_label << " | " << cmp_label << " |\n|---|---:|---:|\n"
      << "| 委托(wt) | " << s_wt << " | " << c_wt << " |\n"
      << "| 撤单(cl) | " << s_cl << " | " << c_cl << " |\n"
      << "| 成交(tr) | " << s_tr << " | " << c_tr << " |\n\n"
      << "| 全匹配股票 | " << perfect << " / " << tot
      << " (" << pct(perfect, tot) << ") |\n|---|---|\n\n";

    std::vector<const SummaryRow*> anomalies;
    for (const auto& r : rows) if (!rowIsClean(r)) anomalies.push_back(&r);

    f << "## 二、有差异股票（" << anomalies.size() << " 只）\n\n";
    if (anomalies.empty()) {
        f << "无差异。\n\n";
    } else {
        f << "| 股票 | wt仅" << src_label << " | wt仅" << cmp_label
          << " | wt全匹配率 | cl仅" << src_label << " | cl仅" << cmp_label
          << " | cl撤量匹配率 | tr仅" << src_label << " | tr仅" << cmp_label << " |\n"
          << "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for (const auto* r : anomalies) {
            f << "| " << r->sym
              << " | " << r->wt_s_only << " | " << r->wt_c_only
              << " | " << pct(r->wt_all_match, r->wt_common)
              << " | " << r->cl_s_only << " | " << r->cl_c_only
              << " | " << pct(r->cl_vol_match, r->cl_common)
              << " | " << r->tr_s_only << " | " << r->tr_c_only << " |\n";
        }
        f << "\n";
    }

    f << "## 三、全市场明细\n\n"
      << "| 股票 | " << src_label << "_wt | " << cmp_label << "_wt"
      << " | wt覆盖率 | wt全匹配率"
      << " | " << src_label << "_cl | " << cmp_label << "_cl | cl覆盖率 | tr覆盖率 |\n"
      << "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& r : rows) {
        f << "| " << r.sym
          << " | " << r.s_wt << " | " << r.c_wt
          << " | " << pct(r.wt_both, r.wt_both + std::max(r.wt_s_only, r.wt_c_only))
          << " | " << pct(r.wt_all_match, r.wt_common)
          << " | " << r.s_cl << " | " << r.c_cl
          << " | " << pct(r.cl_both, r.cl_both + std::max(r.cl_s_only, r.cl_c_only))
          << " | " << pct(r.tr_both, r.tr_both + std::max(r.tr_s_only, r.tr_c_only))
          << (rowIsClean(r) ? "" : " ⚠️") << " |\n";
    }
    f << "\n";
}

static void writeMd(const std::string& path,
                    const std::string& symbol, const std::string& date,
                    const std::string& src_label, const std::string& cmp_label,
                    const EventIndex& si, const EventIndex& ci,
                    const std::vector<WinCounts>& wcs, const AllStats& stats)
{
    std::ofstream f(path);
    if (!f) { std::cerr << "Cannot write: " << path << "\n"; return; }
    f << "# 事件层对比报告\n\n"
      << "**股票**: " << symbol << "　**日期**: " << date
      << "　**主源**: " << src_label << "　**比对源**: " << cmp_label << "\n\n";

    int64_t s_wt=si.n_wt, s_cl=si.n_cl, s_tr=si.n_tr, s_tot=s_wt+s_cl+s_tr;
    int64_t c_wt=ci.n_wt, c_cl=ci.n_cl, c_tr=ci.n_tr, c_tot=c_wt+c_cl+c_tr;
    f << "## 一、总量统计\n\n| | " << src_label << " | " << cmp_label << " | 差值 |\n"
      << "|---|---:|---:|---:|\n"
      << "| 委托(wt) | " << s_wt  << " | " << c_wt  << " | " << s_wt-c_wt   << " |\n"
      << "| 撤单(cl) | " << s_cl  << " | " << c_cl  << " | " << s_cl-c_cl   << " |\n"
      << "| 成交(tr) | " << s_tr  << " | " << c_tr  << " | " << s_tr-c_tr   << " |\n"
      << "| 合计     | " << s_tot << " | " << c_tot << " | " << s_tot-c_tot << " |\n\n";

    size_t pfx = 1;
    while (pfx < src_label.size() && pfx < cmp_label.size() && src_label[pfx-1] == cmp_label[pfx-1]) pfx++;
    pfx = std::max(pfx, (size_t)2);
    std::string sA = src_label.substr(0, std::min(pfx, src_label.size()));
    std::string cA = cmp_label.substr(0, std::min(pfx, cmp_label.size()));
    f << "## 二、时间窗口分布（Layer 1）\n\n"
      << "| 时间段 | " << sA << "_wt | " << cA << "_wt | Δwt"
      << " | " << sA << "_cl | " << cA << "_cl | Δcl"
      << " | " << sA << "_tr | " << cA << "_tr | Δtr |\n"
      << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& wc : wcs) {
        auto d = [](int64_t a, int64_t b) {
            int64_t diff = a-b;
            return diff==0 ? std::string("0") : (diff>0?"+":"")+std::to_string(diff);
        };
        f << "| " << wc.label
          << " | " << wc.f_wt << " | " << wc.t_wt << " | " << d(wc.f_wt, wc.t_wt)
          << " | " << wc.f_cl << " | " << wc.t_cl << " | " << d(wc.f_cl, wc.t_cl)
          << " | " << wc.f_tr << " | " << wc.t_tr << " | " << d(wc.f_tr, wc.t_tr) << " |\n";
    }
    f << "\n";

    const auto& wc = stats.wt_cov; const auto& cc = stats.cl_cov; const auto& tc = stats.tr_cov;
    const auto& wa = stats.wt_attr; const auto& ca = stats.cl_attr;
    auto pct = [](int64_t b, int64_t o) {
        int64_t tot = b+o; if (!tot) return std::string("—");
        char buf[16]; snprintf(buf, sizeof(buf), "%.1f%%", b*100.0/tot); return std::string(buf);
    };
    f << "## 三、Sysid 覆盖率（Layer 2）\n\n"
      << "| 事件 | 共有 | 仅" << src_label << " | 仅" << cmp_label
      << " | " << src_label << "覆盖率 | " << cmp_label << "覆盖率 |\n"
      << "|---|---:|---:|---:|---:|---:|\n"
      << "| 委托(wt) | " << wc.both << " | " << wc.f_only << " | " << wc.t_only
      << " | " << pct(wc.both, wc.f_only) << " | " << pct(wc.both, wc.t_only) << " |\n"
      << "| 撤单(cl) | " << cc.both << " | " << cc.f_only << " | " << cc.t_only
      << " | " << pct(cc.both, cc.f_only) << " | " << pct(cc.both, cc.t_only) << " |\n"
      << "| 成交(tr) | " << tc.both << " | " << tc.f_only << " | " << tc.t_only
      << " | " << pct(tc.both, tc.f_only) << " | " << pct(tc.both, tc.t_only) << " |\n\n";
    writeSampleTable(f, wc.f_only_samples, "仅" + src_label + " 委托样例");
    writeSampleTable(f, wc.t_only_samples, "仅" + cmp_label + " 委托样例");
    writeSampleTable(f, cc.f_only_samples, "仅" + src_label + " 撤单样例");
    writeSampleTable(f, cc.t_only_samples, "仅" + cmp_label + " 撤单样例");

    f << "## 四、属性一致性（Layer 3）\n\n"
      << "### 委托(wt) — 共有 " << wa.common << " 条\n\n"
      << "| 属性 | 一致数 | 一致率 |\n|---|---:|---:|\n"
      << std::fixed << std::setprecision(2);
    auto ap = [&](int64_t n){ return wa.common>0 ? n*100.0/wa.common : 0.0; };
    f << "| 价格 | " << wa.price_match << " | " << ap(wa.price_match) << "% |\n"
      << "| 挂量 | " << wa.vol_match   << " | " << ap(wa.vol_match)   << "% |\n"
      << "| 方向 | " << wa.dir_match   << " | " << ap(wa.dir_match)   << "% |\n"
      << "| 全部 | " << wa.all_match   << " | " << ap(wa.all_match)   << "% |\n\n"
      << "### 撤单(cl) — 共有 " << ca.common << " 条 sysid\n\n"
      << "| 属性 | 一致数 | 一致率 |\n|---|---:|---:|\n";
    auto cp = [&](int64_t n){ return ca.common>0 ? n*100.0/ca.common : 0.0; };
    f << "| 累计撤量 | " << ca.vol_match << " | " << cp(ca.vol_match) << "% |\n\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::vector<std::string> symbols;
    std::string date, market = "sh";
    std::string src_type = "flow", src_path, src_path2, src_label;
    std::string cmp_type = "tl",   cmp_path, cmp_path2, cmp_label;
    std::string output_path;    // single-symbol mode
    std::string output_dir;     // multi-symbol: summary + (with --detail) per-symbol MD
    std::string lob_dir;        // LOB CSV output dir (default: output_dir/lob)
    double start_sec = 9*3600.0 + 15*60.0;
    double end_sec   = 15*3600.0;
    double interval  = 1800.0;

    bool run_all      = false;
    bool detail       = false;
    bool gen_lob      = false;
    int  n_workers    = 40;
    int  min_src_events = 0;
    double lob_start_sec = 0.0;
    double lob_end_sec   = 1e9;
    std::string sym_prefix;     // e.g. "6" to keep only 6xxxxx stocks
    std::string output_format = "csv"; // csv | h5 | both

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a=="--symbol"         && i+1<argc) symbols.push_back(argv[++i]);
        else if (a=="--symbols"        && i+1<argc) {
            std::istringstream ss(argv[++i]); std::string tok;
            while (std::getline(ss, tok, ',')) {
                while (!tok.empty() && tok.front()==' ') tok.erase(tok.begin());
                while (!tok.empty() && tok.back()==' ')  tok.pop_back();
                if (!tok.empty()) symbols.push_back(tok);
            }
        }
        else if (a=="--all")                         run_all         = true;
        else if (a=="--detail")                      detail          = true;
        else if (a=="--gen-lob")                     gen_lob         = true;
        else if (a=="--workers"          && i+1<argc) n_workers      = std::stoi(argv[++i]);
        else if (a=="--min-src-events"   && i+1<argc) min_src_events = std::stoi(argv[++i]);
        else if (a=="--date"             && i+1<argc) date           = argv[++i];
        else if (a=="--market"           && i+1<argc) market         = argv[++i];
        else if (a=="--primary-type"     && i+1<argc) src_type       = argv[++i];
        else if (a=="--primary-path"     && i+1<argc) src_path       = argv[++i];
        else if (a=="--primary-path2"    && i+1<argc) src_path2      = argv[++i];
        else if (a=="--compare-type"     && i+1<argc) cmp_type       = argv[++i];
        else if (a=="--compare-path"     && i+1<argc) cmp_path       = argv[++i];
        else if (a=="--compare-path2"    && i+1<argc) cmp_path2      = argv[++i];
        else if (a=="--primary-label"    && i+1<argc) src_label      = argv[++i];
        else if (a=="--compare-label"    && i+1<argc) cmp_label      = argv[++i];
        else if (a=="--output"           && i+1<argc) output_path    = argv[++i];
        else if (a=="--output-dir"       && i+1<argc) output_dir     = argv[++i];
        else if (a=="--lob-dir"          && i+1<argc) lob_dir        = argv[++i];
        else if (a=="--lob-start"        && i+1<argc) lob_start_sec  = timeStrToSec(std::string(argv[++i])+":00");
        else if (a=="--lob-end"          && i+1<argc) lob_end_sec    = timeStrToSec(std::string(argv[++i])+":00");
        else if (a=="--sym-prefix"       && i+1<argc) sym_prefix     = argv[++i];
        else if (a=="--output-format"    && i+1<argc) output_format  = argv[++i];
        else if (a=="--interval"         && i+1<argc) interval       = std::stod(argv[++i]);
        else if (a=="--start"            && i+1<argc) start_sec      = timeStrToSec(std::string(argv[++i])+":00");
        else if (a=="--end"              && i+1<argc) end_sec        = timeStrToSec(std::string(argv[++i])+":00");
        // compat aliases
        else if (a=="--flow-dir"         && i+1<argc) { src_type="flow"; src_path=argv[++i]; }
        else if (a=="--tl-file"          && i+1<argc) { cmp_type="tl";   cmp_path=argv[++i]; }
    }

    if (src_path.empty()) {
        std::cerr << "用法: datacheck\n"
                  << "  --symbol 600638 | --symbols 600638,600519 | --all\n"
                  << "  --date 20260608 --market sh\n"
                  << "  --primary-type flow|tl  --primary-path <dir|csv>\n"
                  << "  [--primary-path2 <csv>]    深圳 TL 主源的成交文件（mdl_6_36），--market sz 时必填\n"
                  << "  [--compare-type flow|tl  --compare-path <dir|csv>]  (省略则仅生成LOB)\n"
                  << "  [--compare-path2 <csv>]    深圳 TL 比对源的成交文件（mdl_6_36）\n"
                  << "  [--output-dir /reports/]    事件对比报告目录\n"
                  << "  [--detail]                  同时写每只股票详情 MD\n"
                  << "  [--gen-lob]                 根据主源重建 LOB\n"
                  << "  [--lob-dir /lob/]           LOB 输出目录（默认 output-dir/lob）\n"
                  << "  [--lob-start HH:MM]         LOB 时间范围起点（默认全天）\n"
                  << "  [--lob-end   HH:MM]         LOB 时间范围终点（默认全天）\n"
                  << "  [--workers 40]\n"
                  << "  [--min-src-events 100]      过滤主源事件不足的股票\n"
                  << "  [--sym-prefix 6]            只处理指定前缀的股票代码（如 6=普通A股）\n"
                  << "  [--output-format csv|h5|both]  LOB 输出格式（默认 csv）\n";
        return 1;
    }
    if (cmp_path.empty() && !gen_lob) {
        std::cerr << "错误: 未指定 --compare-path，请同时加 --gen-lob 以仅生成 LOB\n";
        return 1;
    }
    if (market == "sz") {
        if (src_type == "tl" && src_path2.empty()) {
            std::cerr << "错误: --market sz 且 --primary-type tl 时必须指定 --primary-path2（mdl_6_36 成交文件）\n";
            return 1;
        }
        if (cmp_type == "tl" && !cmp_path.empty() && cmp_path2.empty()) {
            std::cerr << "错误: --market sz 且 --compare-type tl 时必须指定 --compare-path2（mdl_6_36 成交文件）\n";
            return 1;
        }
    }

    bool skip_lunch = (market == "sh");
    auto defaultLabel = [](const std::string& t) {
        if (t == "tl") return std::string("TL");
        std::string s = t; s[0] = toupper(s[0]); return s;
    };
    if (src_label.empty()) src_label = defaultLabel(src_type);
    if (cmp_label.empty()) cmp_label = defaultLabel(cmp_type);

    // LOB 时间段字符串（用于目录命名 + MD 报告）
    auto fmtHHMM = [](double s, const char* def) -> std::string {
        if (s <= 0 || s >= 1e8) return def;
        int h = (int)s/3600, m = ((int)s%3600)/60;
        char b[6]; snprintf(b, sizeof(b), "%02d%02d", h, m); return std::string(b);
    };
    auto fmtDisp = [](double s, const char* def) -> std::string {
        if (s <= 0 || s >= 1e8) return def;
        int h = (int)s/3600, m = ((int)s%3600)/60;
        char b[6]; snprintf(b, sizeof(b), "%02d:%02d", h, m); return std::string(b);
    };
    bool has_range = (lob_start_sec > 0 || lob_end_sec < 1e8);
    std::string lob_range_dir  = has_range ? ("_" + fmtHHMM(lob_start_sec,"0915")
                                               + "-" + fmtHHMM(lob_end_sec,"1500")) : "";
    std::string lob_range_disp = has_range ? (fmtDisp(lob_start_sec,"09:15")
                                               + "~" + fmtDisp(lob_end_sec,"15:00")) : "全天";

    // LOB output dir: --lob-dir > output-dir/lob_{market}[_HHMM-HHMM]
    if (gen_lob && lob_dir.empty()) {
        lob_dir = output_dir.empty() ? "lob_" + date + "_" + market + lob_range_dir
                                     : output_dir + "/lob_" + market + lob_range_dir;
    }

    // ── 发现股票列表
    if (run_all) {
        if (cmp_type == "tl") {
            std::cerr << "[发现] 扫描 " << cmp_label << " 获取股票列表...\n";
            symbols = (market == "sz") ? discoverSZSymbols(cmp_path) : discoverTLSymbols(cmp_path);
        } else if (src_type == "tl") {
            std::cerr << "[发现] 扫描 " << src_label << " 获取股票列表...\n";
            symbols = (market == "sz") ? discoverSZSymbols(src_path) : discoverTLSymbols(src_path);
        } else {
            std::cerr << "--all 需要 --primary-type tl 或 --compare-type tl\n"; return 1;
        }
        std::cerr << "[发现] 共 " << symbols.size() << " 只股票\n";
    }
    if (!sym_prefix.empty()) {
        symbols.erase(std::remove_if(symbols.begin(), symbols.end(),
            [&](const std::string& s){ return s.rfind(sym_prefix, 0) != 0; }),
            symbols.end());
        std::cerr << "[过滤] --sym-prefix=" << sym_prefix
                  << "，保留 " << symbols.size() << " 只股票\n";
    }
    if (symbols.empty()) {
        std::cerr << "错误: 未指定股票，请用 --symbol / --symbols / --all\n"; return 1;
    }

    std::cerr << "=== datacheck: " << symbols.size() << " 只股票  " << date << "  ";
    if (!cmp_path.empty())
        std::cerr << src_label << " vs " << cmp_label;
    else
        std::cerr << src_label << " [仅LOB]";
    if (gen_lob) {
        std::cerr << "  [+LOB";
        if (has_range) std::cerr << " " << lob_range_disp;
        std::cerr << "]";
    }
    std::cerr << " ===\n";

    // ── IO: 主源（combined）+ 比对源（events only，可选），并行
    bool do_compare = !cmp_path.empty();
    std::cerr << "[IO] 读取主源(" << src_label << (gen_lob?" + LOB records":"") << ")";
    if (do_compare) std::cerr << " + 比对源(" << cmp_label << ")";
    std::cerr << "...\n";

    // src/cmp 各自的 IO 线程数：两者都可能并行跑（std::async），都用 tl 时合用 n_workers 防止超订
    int io_workers_src = (do_compare && cmp_type == "tl") ? std::max(1, n_workers / 2) : n_workers;
    int io_workers_cmp = (src_type == "tl") ? std::max(1, n_workers / 2) : n_workers;

    auto src_fut = std::async(std::launch::async, [&]() -> PrimaryData {
        if (src_type == "flow") return parseFlowDirCombined(src_path, symbols, market, gen_lob, lob_start_sec, lob_end_sec);
        if (src_type == "tl") {
            if (market == "sz") return parseTLSZFilesCombined(src_path, src_path2, symbols, gen_lob, lob_start_sec, lob_end_sec, io_workers_src);
            return parseTLFileCombined(src_path, symbols, gen_lob, lob_start_sec, lob_end_sec, io_workers_src);
        }
        std::cerr << "Unknown primary type: " << src_type << "\n"; return {};
    });
    auto cmp_fut = std::async(std::launch::async, [&]() -> EventMap {
        if (!do_compare) return {};
        if (cmp_type == "flow") return parseFlowDirCombined(cmp_path, symbols, market, false).events;
        if (cmp_type == "tl") {
            if (market == "sz") return parseTLSZFilesCombined(cmp_path, cmp_path2, symbols, false, 0.0, 1e9, io_workers_cmp).events;
            return parseTLFileCombined(cmp_path, symbols, false, 0.0, 1e9, io_workers_cmp).events;
        }
        std::cerr << "Unknown compare type: " << cmp_type << "\n"; return {};
    });

    PrimaryData primary = src_fut.get();
    EventMap    cmp_all = cmp_fut.get();
    if (do_compare)
        std::cerr << "[IO] 读取完成，开始事件对比 (workers=" << n_workers << ")...\n";
    else
        std::cerr << "[IO] 读取完成\n";

    // ── 创建输出目录
    if (!output_dir.empty()) { std::string cmd = "mkdir -p " + output_dir; ::system(cmd.c_str()); }
    if (gen_lob)              { std::string cmd = "mkdir -p " + lob_dir;   ::system(cmd.c_str()); }

    // ── Phase 1: 事件对比（线程池，仅有比对源时执行）
    std::mutex print_mu;
    std::atomic<int> done_count{0};
    const int total = (int)symbols.size();
    std::vector<SummaryRow> summary_rows;

    if (do_compare) {
    summary_rows.reserve(symbols.size());
    ThreadPool pool(std::min(n_workers, (int)symbols.size()));
    std::vector<std::future<void>> futs;
    futs.reserve(symbols.size());

    for (const auto& sym : symbols) {
        futs.push_back(pool.submit([&, sym]() {
            static const std::vector<Event> empty_evs;
            const auto& se = primary.events.count(sym) ? primary.events.at(sym) : empty_evs;
            const auto& ce = cmp_all.count(sym)        ? cmp_all.at(sym)        : empty_evs;

            if (min_src_events > 0 && (int)se.size() < min_src_events) return;

            auto si    = buildIndex(se);
            auto ci    = buildIndex(ce);
            auto wcs   = layer1(se, ce, start_sec, end_sec, interval, skip_lunch);
            auto stats = computeStats(si, ci);

            SummaryRow row;
            row.sym          = sym;
            row.s_wt=si.n_wt; row.s_cl=si.n_cl; row.s_tr=si.n_tr;
            row.c_wt=ci.n_wt; row.c_cl=ci.n_cl; row.c_tr=ci.n_tr;
            row.wt_both      = stats.wt_cov.both;
            row.wt_s_only    = stats.wt_cov.f_only;
            row.wt_c_only    = stats.wt_cov.t_only;
            row.wt_common    = stats.wt_attr.common;
            row.wt_all_match = stats.wt_attr.all_match;
            row.cl_both      = stats.cl_cov.both;
            row.cl_s_only    = stats.cl_cov.f_only;
            row.cl_c_only    = stats.cl_cov.t_only;
            row.cl_common    = stats.cl_attr.common;
            row.cl_vol_match = stats.cl_attr.vol_match;
            row.tr_both      = stats.tr_cov.both;
            row.tr_s_only    = stats.tr_cov.f_only;
            row.tr_c_only    = stats.tr_cov.t_only;

            {
                std::lock_guard<std::mutex> lk(print_mu);
                summary_rows.push_back(row);
                int n = ++done_count;
                std::cerr << "[evnt " << n << "/" << total << " " << sym << "] "
                          << "wt=" << si.n_wt << "/" << ci.n_wt
                          << " cl=" << si.n_cl << "/" << ci.n_cl
                          << " all_match=" << stats.wt_attr.all_match
                          << "/" << stats.wt_attr.common << "\n";
            }

            if (detail && !output_dir.empty())
                writeMd(output_dir+"/"+sym+"_"+date+"_events.md",
                        sym, date, src_label, cmp_label, si, ci, wcs, stats);
            else if (symbols.size()==1 && !output_path.empty())
                writeMd(output_path, sym, date, src_label, cmp_label, si, ci, wcs, stats);
        }));
    }
    for (auto& f : futs) f.get();
    std::cerr << "=== 事件对比完成: " << total << " 只股票 ===\n";

    if (!output_dir.empty() && !summary_rows.empty()) {
        std::string sp = output_dir + "/_summary_" + date + "_" + market + ".md";
        writeSummary(sp, date, market, src_label, cmp_label, summary_rows,
                     gen_lob ? lob_range_disp : "");
        std::cerr << "[汇总] " << sp << "\n";
    }
    } // end do_compare

    // ── Phase 2: LOB 重建（如有 --gen-lob）
    if (!gen_lob) return 0;

    std::cerr << "[LOB] 开始重建 LOB -> " << lob_dir << " (workers=" << n_workers << ")...\n";
    done_count = 0;

    ThreadPool lob_pool(std::min(n_workers, (int)symbols.size()));
    std::vector<std::future<void>> lob_futs;
    lob_futs.reserve(symbols.size());

    for (const auto& sym : symbols) {
        lob_futs.push_back(lob_pool.submit([&, sym]() {
            try {
                if (!primary.lob_recs.count(sym)) return;
                auto recs = std::move(primary.lob_recs.at(sym));
                if (min_src_events > 0 && (int)recs.size() < min_src_events) return;

                dc::LobBuilder builder(sym, date, /*isSH=*/(market == "sh"));
                builder.load(std::move(recs));
                builder.build();

                bool do_csv = (output_format == "csv" || output_format == "both");
                bool do_h5  = (output_format == "h5"  || output_format == "both");
                if (do_csv) {
                    builder.writeOrderTableCSV(lob_dir + "/" + sym + "_order.csv");
                    builder.writeTradeTableCSV(lob_dir + "/" + sym + "_trade.csv");
                }
                if (do_h5) {
                    builder.writeOrderTableH5 (lob_dir + "/" + sym + ".h5");
                    builder.writeTradeTableH5 (lob_dir + "/" + sym + ".h5");
                }
                int n = ++done_count;
                std::lock_guard<std::mutex> lk(print_mu);
                std::cerr << "[lob  " << n << "/" << total << " " << sym << "] "
                          << "order=" << builder.getOrderTable().size()
                          << " trade=" << builder.getTradeTable().size() << "\n";
            } catch (const std::exception& ex) {
                int n = ++done_count;
                std::lock_guard<std::mutex> lk(print_mu);
                std::cerr << "[lob  " << n << "/" << total << " " << sym << "] ERROR: " << ex.what() << "\n";
            }
        }));
    }
    for (auto& f : lob_futs) f.get();
    std::cerr << "=== LOB 完成: " << done_count.load() << " 只股票 -> " << lob_dir << " ===\n";
    return 0;
}
