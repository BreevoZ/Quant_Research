#pragma once
// 单只股票 TL 逐笔解析 → dc::UnifiedRecord 列表。
// 语义与 datacheck_final/datacheck_main.cpp 的 parseTLChunk / parseSZ*Chunk 完全一致
// (SH: sysid=裸OrderNO, seqNo=BizIndex; SZ: sysid=seqNo=ApplSeqNum)。
// 单线程顺序扫描(单股够快)。LobBuilder::build() 内部会按 seqNo 排序 + SH resequence。

#include "lob_builder.h"
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <future>
#include <thread>
#include <algorithm>

namespace vsim {

inline double timeStrToSec(const std::string& t) {
    if (t.size() < 5) return -1.0;
    try {
        int h = std::stoi(t.substr(0, 2));
        int m = std::stoi(t.substr(3, 2));
        double s = (t.size() >= 8) ? std::stod(t.substr(6)) : 0.0;
        return h * 3600.0 + m * 60.0 + s;
    } catch (...) { return -1.0; }
}

inline void splitCSV(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.emplace_back(line, start, i - start);
            start = i + 1;
        }
    }
}

inline std::string padSZ(const std::string& s) {
    if (s.size() >= 6) return s;
    return std::string(6 - s.size(), '0') + s;
}

// 取第 n 个逗号分隔字段(0基)到 line 里的位置 [beg,end); 找不到返回 false。
// 用于「先拒绝再拆分」: 只比对 SecurityID 一列, 不匹配的行不做整行 splitCSV(省 ~13 次/行 string 构造)。
inline bool fieldRange(const std::string& line, int n, size_t& beg, size_t& end) {
    size_t p = 0; int col = 0;
    while (col < n) { p = line.find(',', p); if (p == std::string::npos) return false; ++p; ++col; }
    end = line.find(',', p); if (end == std::string::npos) end = line.size();
    beg = p; return true;
}

using SymRecMap = std::unordered_map<std::string, std::vector<dc::UnifiedRecord>>;

// 把 [headerEnd, fileSize) 切成 n 段, 每段末尾对齐到完整行(向后吃到下一个换行)。
struct ByteChunk { std::streampos start, end; };
inline std::vector<ByteChunk> computeChunks(const std::string& path, std::streampos headerEnd, int n) {
    std::ifstream ifs(path, std::ios::binary);
    ifs.seekg(0, std::ios::end);
    std::streampos fsz = ifs.tellg();
    std::vector<ByteChunk> ch;
    if (n <= 1 || fsz <= headerEnd) { ch.push_back({headerEnd, fsz}); return ch; }
    int64_t total = (int64_t)fsz - (int64_t)headerEnd, csz = total / n;
    std::streampos prev = headerEnd; std::string tmp;
    for (int i = 0; i < n; ++i) {
        std::streampos re;
        if (i == n - 1) re = fsz;
        else { std::streampos nom = headerEnd + (std::streamoff)(csz * (i + 1));
               ifs.seekg(nom); std::getline(ifs, tmp); re = ifs.eof() ? fsz : ifs.tellg(); }
        ch.push_back({prev, re}); prev = re; if (re >= fsz) break;
    }
    return ch;
}

// 并行框架: 把 per-chunk 解析函数 fn 跑在各字节区间, 合并各自的 SymRecMap(合并顺序无关,
// LobBuilder::build() 内部按 seqNo 重排)。
template <class Fn>
inline SymRecMap parseParallel(const std::string& path, std::streampos headerEnd, Fn fn, int nWorkers) {
    int hw = std::max<int>(1, (int)std::thread::hardware_concurrency());
    int nw = std::max<int>(1, nWorkers > 0 ? nWorkers : hw);
    auto chunks = computeChunks(path, headerEnd, nw);
    std::vector<std::future<SymRecMap>> futs;
    for (auto& c : chunks) futs.push_back(std::async(std::launch::async, fn, c));
    SymRecMap out;
    for (auto& f : futs) {
        SymRecMap part = f.get();
        for (auto& [sym, v] : part) {
            auto& dst = out[sym];
            dst.insert(dst.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
        }
    }
    return out;
}

// ── 上海 mdl_4_24 (委托A/撤单D/成交T 合一): 分块并行扫描, 过滤多股票 ─────────────
inline SymRecMap parseSHSymbols(const std::string& path,
                                const std::unordered_set<std::string>& symSet, int nWorkers = 60,
                                bool acceptAll = false) {   // acceptAll: 不过滤, 扫全市场(预缓存用)
    std::ifstream ifs(path);
    if (!ifs) { std::cerr << "无法打开: " << path << "\n"; SymRecMap m; for(auto&s:symSet)m[s]; return m; }
    std::string line;
    if (!std::getline(ifs, line)) return {};
    std::vector<std::string> f0; splitCSV(line, f0);
    std::unordered_map<std::string,int> col;
    for (int i = 0; i < (int)f0.size(); ++i) col[f0[i]] = i;
    int c_sec=col["SecurityID"], c_tick=col["TickTime"], c_type=col["Type"],
        c_buy=col["BuyOrderNO"], c_sell=col["SellOrderNO"], c_prc=col["Price"],
        c_qty=col["Qty"], c_flag=col["TickBSFlag"];
    int c_biz = col.count("BizIndex")   ? col["BizIndex"]   : -1;
    int c_tm  = col.count("TradeMoney") ? col["TradeMoney"] : -1;
    std::streampos headerEnd = ifs.tellg(); ifs.close();

    auto worker = [=, &symSet](ByteChunk ch) -> SymRecMap {
        SymRecMap out;
        std::ifstream in(path, std::ios::binary); in.seekg(ch.start);
        std::string line; std::vector<std::string> f;
        while (true) {
            std::streampos pos = in.tellg();
            if (pos == std::streampos(-1) || pos >= ch.end) break;
            if (!std::getline(in, line)) break;
            if (line.empty()) continue;
            size_t sb, se;
            if (!fieldRange(line, c_sec, sb, se)) continue;
            if (!acceptAll && !symSet.count(line.substr(sb, se - sb))) continue;   // 先拒绝
            splitCSV(line, f);
            if ((int)f.size() <= c_flag) continue;
            const std::string& type = f[c_type];
            if (type == "S") continue;
            double tsec = timeStrToSec(f[c_tick]); if (tsec < 0) continue;
            double prc = std::stod(f[c_prc]);
            int64_t vol = std::stoll(f[c_qty]);
            int64_t buy = std::stoll(f[c_buy]), sell = std::stoll(f[c_sell]);
            char flag = f[c_flag].empty() ? ' ' : f[c_flag][0];
            dc::UnifiedRecord ur;
            ur.seqNo = (c_biz>=0 && (int)f.size()>c_biz && !f[c_biz].empty()) ? std::stoll(f[c_biz]) : 0;
            ur.time = f[c_tick]; ur.timeSeconds = tsec; ur.price = prc;
            ur.intPrice = static_cast<int64_t>((prc + 1e-6) * 100); ur.volume = vol;
            ur.turnover = (c_tm>=0 && (int)f.size()>c_tm && !f[c_tm].empty()) ? std::stod(f[c_tm]) : 0.0;
            ur.priceType = '2';
            if (type == "A") { ur.actionType=0; ur.direction=flag; ur.sysid=(flag=='B')?buy:sell; }
            else if (type == "D") { ur.actionType=1; ur.direction=(buy>0&&sell==0)?'B':'S'; ur.sysid=(buy>0)?buy:sell; }
            // 主动方向: SH TickBSFlag 三值 B/S/N。N=竞价撮合等无主动方 → 标 0(未知),
            // 与 tltoflow(sh_trade_direction)口径一致;别把 N 偷懒归成卖方主动。
            else { ur.actionType=2; ur.buyId=buy; ur.sellId=sell; ur.direction=flag;
                   ur.tradeDirection=(flag=='B')?1:(flag=='S')?2:0; }
            out[f[c_sec]].push_back(std::move(ur));
        }
        return out;
    };

    SymRecMap out = parseParallel(path, headerEnd, worker, nWorkers);
    for (const auto& s : symSet) out[s];   // 无数据也留空条目
    return out;
}

// ── 深圳 mdl_6_33(委托) + mdl_6_36(成交/撤单): 两文件各分块并行, 过滤多股票 ────────
inline SymRecMap parseSZSymbols(const std::string& orderPath, const std::string& tradePath,
                                const std::unordered_set<std::string>& symRawSet, int nWorkers = 60,
                                bool acceptAll = false) {   // acceptAll: 不过滤, 扫全市场(预缓存用)
    std::unordered_set<std::string> symSet;
    for (const auto& s : symRawSet) symSet.insert(padSZ(s));
    SymRecMap out;

    auto colIndex = [](const std::string& path, std::unordered_map<std::string,int>& col,
                       std::streampos& hend) -> bool {
        std::ifstream ifs(path);
        if (!ifs) { std::cerr << "无法打开: " << path << "\n"; return false; }
        std::string line; if (!std::getline(ifs, line)) return false;
        std::vector<std::string> f; splitCSV(line, f);
        for (int i = 0; i < (int)f.size(); ++i) col[f[i]] = i;
        hend = ifs.tellg(); return true;
    };

    // 委托文件
    { std::unordered_map<std::string,int> col; std::streampos hend;
      if (colIndex(orderPath, col, hend)) {
        int c_sec=col["SecurityID"], c_prc=col["Price"], c_qty=col["OrderQty"],
            c_side=col["Side"], c_time=col["TransactTime"], c_appl=col["ApplSeqNum"];
        int c_ord = col.count("OrdType") ? col["OrdType"] : -1;
        auto worker = [=, &symSet](ByteChunk ch) -> SymRecMap {
            SymRecMap o; std::ifstream in(orderPath, std::ios::binary); in.seekg(ch.start);
            std::string line; std::vector<std::string> f;
            while (true) {
                std::streampos pos = in.tellg();
                if (pos == std::streampos(-1) || pos >= ch.end) break;
                if (!std::getline(in, line)) break;
                if (line.empty()) continue;
                size_t sb, se; if (!fieldRange(line, c_sec, sb, se)) continue;
                std::string sec = padSZ(line.substr(sb, se - sb));
                if (!acceptAll && !symSet.count(sec)) continue;
                splitCSV(line, f); if ((int)f.size() <= c_appl) continue;
                double tsec = timeStrToSec(f[c_time]); if (tsec < 0) continue;
                double prc = std::stod(f[c_prc]); int64_t vol = std::stoll(f[c_qty]);
                int side = std::stoi(f[c_side]); int64_t appl = std::stoll(f[c_appl]);
                dc::UnifiedRecord ur;
                ur.seqNo=appl; ur.time=f[c_time]; ur.timeSeconds=tsec; ur.actionType=0;
                ur.direction=(side==49)?'B':'S'; ur.sysid=appl; ur.price=prc;
                ur.intPrice=static_cast<int64_t>((prc+1e-6)*100); ur.volume=vol;
                int ot = (c_ord>=0 && (int)f.size()>c_ord && !f[c_ord].empty()) ? std::stoi(f[c_ord]) : 0;
                ur.priceType = (ot==49)?'1':(ot==85)?'U':'2';
                o[sec].push_back(std::move(ur));
            }
            return o;
        };
        out = parseParallel(orderPath, hend, worker, nWorkers);
      }
    }
    // 成交/撤单文件
    { std::unordered_map<std::string,int> col; std::streampos hend;
      if (colIndex(tradePath, col, hend)) {
        int c_bid=col["BidApplSeqNum"], c_off=col["OfferApplSeqNum"], c_sec=col["SecurityID"],
            c_prc=col["LastPx"], c_qty=col["LastQty"], c_exec=col["ExecType"],
            c_time=col["TransactTime"], c_appl=col["ApplSeqNum"];
        auto worker = [=, &symSet](ByteChunk ch) -> SymRecMap {
            SymRecMap o; std::ifstream in(tradePath, std::ios::binary); in.seekg(ch.start);
            std::string line; std::vector<std::string> f;
            while (true) {
                std::streampos pos = in.tellg();
                if (pos == std::streampos(-1) || pos >= ch.end) break;
                if (!std::getline(in, line)) break;
                if (line.empty()) continue;
                size_t sb, se; if (!fieldRange(line, c_sec, sb, se)) continue;
                std::string sec = padSZ(line.substr(sb, se - sb));
                if (!acceptAll && !symSet.count(sec)) continue;
                splitCSV(line, f); if ((int)f.size() <= c_exec) continue;
                double tsec = timeStrToSec(f[c_time]); if (tsec < 0) continue;
                double prc = std::stod(f[c_prc]); int64_t vol = std::stoll(f[c_qty]);
                int64_t bidId = std::stoll(f[c_bid]), askId = std::stoll(f[c_off]);
                int exec = std::stoi(f[c_exec]); int64_t appl = std::stoll(f[c_appl]);
                dc::UnifiedRecord ur;
                ur.time=f[c_time]; ur.timeSeconds=tsec; ur.price=prc;
                ur.intPrice=static_cast<int64_t>((prc+1e-6)*100); ur.volume=vol; ur.priceType='2'; ur.seqNo=appl;
                if (exec==52) { ur.actionType=1; if(bidId>0){ur.direction='B';ur.sysid=bidId;} else {ur.direction='S';ur.sysid=askId;} }
                else if (exec==70) { ur.actionType=2; ur.buyId=bidId; ur.sellId=askId; ur.turnover=prc*vol; }
                else continue;
                o[sec].push_back(std::move(ur));
            }
            return o;
        };
        SymRecMap tmap = parseParallel(tradePath, hend, worker, nWorkers);
        for (auto& [sym, v] : tmap) {
            auto& dst = out[sym];
            dst.insert(dst.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
        }
      }
    }
    for (const auto& s : symSet) out[s];
    return out;
}

// ── Flow 统一格式(tltoflow 产物, 单文件含全市场): 分块并行, 过滤多合约 ──────────
// instrToKey: instrumentID("600000.SH"/"000001.SZ") -> 分组键(如 "sh|600000"); 返回按分组键归并。
// 语义与 datacheck_final parseFlowDirCombined 一致: norm() 把 channel*1e12+订单号 还原成裸订单号
// (与 TL 的 sysid 对齐); seqNo 取 appSeq(通道内保序, 与 BizIndex 同序)。
inline SymRecMap parseFlowSymbols(const std::string& path,
                                  const std::unordered_map<std::string,std::string>& instrToKey, int nWorkers = 60,
                                  bool acceptAll = false) {   // acceptAll: 扫全市场, key 由 instrument 推导(预缓存用)
    std::ifstream ifs(path);
    if (!ifs) { std::cerr << "无法打开: " << path << "\n"; return {}; }
    std::string line;
    if (!std::getline(ifs, line)) return {};
    std::vector<std::string> f0; splitCSV(line, f0);
    std::unordered_map<std::string,int> col;
    for (int i = 0; i < (int)f0.size(); ++i) col[f0[i]] = i;
    if (!col.count("actionType")) { std::cerr << "Flow 表头缺 actionType\n"; return {}; }
    int c_time=col["exchangeTime"], c_at=col["actionType"], c_dir=col["dir"],
        c_bid=col["bidId"], c_ask=col["askId"], c_prc=col["price"], c_vol=col["volume"];
    int c_to  = col.count("turnover")       ? col["turnover"]       : -1;
    int c_pt  = col.count("priceType")      ? col["priceType"]      : -1;
    int c_td  = col.count("tradeDirection") ? col["tradeDirection"] : -1;
    int c_seq = col.count("appSeq")         ? col["appSeq"]         : -1;
    std::streampos headerEnd = ifs.tellg(); ifs.close();
    const int64_t MOD = 1000000000000LL;

    auto worker = [=, &instrToKey](ByteChunk ch) -> SymRecMap {
        SymRecMap out;
        std::ifstream in(path, std::ios::binary); in.seekg(ch.start);
        std::string line; std::vector<std::string> f;
        while (true) {
            std::streampos pos = in.tellg();
            if (pos == std::streampos(-1) || pos >= ch.end) break;
            if (!std::getline(in, line)) break;
            if (line.empty()) continue;
            size_t comma = line.find(',');                       // 先拒绝: instrumentID = 第0列
            if (comma == std::string::npos) continue;
            std::string instr = line.substr(0, comma);
            std::string key;
            if (acceptAll) {                                     // "600000.SH" -> "sh|600000"
                size_t dot = instr.rfind('.');
                if (dot == std::string::npos) continue;
                std::string suf = instr.substr(dot + 1);
                std::string mk = (suf=="SH"||suf=="sh") ? "sh" : (suf=="SZ"||suf=="sz") ? "sz" : "";
                if (mk.empty()) continue;
                key = mk + "|" + instr.substr(0, dot);
            } else {
                auto it = instrToKey.find(instr);
                if (it == instrToKey.end()) continue;
                key = it->second;
            }
            splitCSV(line, f);
            if ((int)f.size() <= c_vol) continue;
            const std::string& ts = f[c_time];
            auto sp = ts.find(' ');
            double tsec = timeStrToSec(sp != std::string::npos ? ts.substr(sp+1) : ts);
            if (tsec < 0) continue;
            int at = std::stoi(f[c_at]);
            int diri = std::stoi(f[c_dir]);
            char dir = (diri == 0) ? 'B' : 'S';                  // Flow: 0=买 1=卖
            double prc = std::stod(f[c_prc]);
            int64_t vol = std::stoll(f[c_vol]);
            int64_t bid = f[c_bid].empty() ? 0 : std::stoll(f[c_bid]);
            int64_t ask = f[c_ask].empty() ? 0 : std::stoll(f[c_ask]);
            auto norm = [&](int64_t id){ return (id > MOD) ? id % MOD : id; };
            dc::UnifiedRecord ur;
            ur.seqNo = (c_seq>=0 && (int)f.size()>c_seq && !f[c_seq].empty()) ? std::stoll(f[c_seq]) : 0;
            ur.time = (sp != std::string::npos) ? ts.substr(sp+1) : ts;
            ur.timeSeconds = tsec; ur.actionType = at; ur.direction = dir;
            ur.price = prc; ur.intPrice = static_cast<int64_t>((prc + 1e-6) * 100); ur.volume = vol;
            ur.turnover = (c_to>=0 && (int)f.size()>c_to && !f[c_to].empty()) ? std::stod(f[c_to]) : 0.0;
            ur.priceType = (c_pt>=0 && (int)f.size()>c_pt && !f[c_pt].empty()) ? f[c_pt][0] : '2';
            ur.tradeDirection = (c_td>=0 && (int)f.size()>c_td && !f[c_td].empty()) ? std::stoi(f[c_td]) : 0;
            if (at == 0 || at == 1) ur.sysid = norm((bid > 0) ? bid : ask);
            else { ur.buyId = norm(bid); ur.sellId = norm(ask); }
            out[key].push_back(std::move(ur));
        }
        return out;
    };
    return parseParallel(path, headerEnd, worker, nWorkers);
}

} // namespace vsim
