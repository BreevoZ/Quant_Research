#include "auction_lob_builder.h"
#include <iomanip>
#include <cstring>  // for strncpy
#include <future>   // 并行分块读取CSV用 (std::async)

#ifdef HAS_HDF5
#include <H5Cpp.h>
#endif

namespace TLLob {

// ============================================================
// 辅助函数
// ============================================================

/**
 * 解析时间字符串为秒数
 * 支持格式: "HH:MM:SS.mmm" 或 "HHMMSS.mmm"
 */
double TLLobBuilder::parseTimeToSeconds(const std::string& timeStr) {
    if (timeStr.empty()) return 0.0;
    
    int hour = 0, minute = 0, second = 0;
    int millisecond = 0;
    
    // 尝试解析 HH:MM:SS.mmm 格式
    if (timeStr.find(':') != std::string::npos) {
        size_t pos1 = timeStr.find(':');
        size_t pos2 = timeStr.find(':', pos1 + 1);
        size_t posDot = timeStr.find('.');
        
        if (pos1 != std::string::npos && pos2 != std::string::npos) {
            hour = std::stoi(timeStr.substr(0, pos1));
            minute = std::stoi(timeStr.substr(pos1 + 1, pos2 - pos1 - 1));
            
            if (posDot != std::string::npos) {
                second = std::stoi(timeStr.substr(pos2 + 1, posDot - pos2 - 1));
                std::string msStr = timeStr.substr(posDot + 1);
                // 补齐到3位
                while (msStr.length() < 3) msStr += '0';
                if (msStr.length() > 3) msStr = msStr.substr(0, 3);
                millisecond = std::stoi(msStr);
            } else {
                second = std::stoi(timeStr.substr(pos2 + 1));
            }
        }
    } else {
        // HHMMSS.mmm 格式
        if (timeStr.length() >= 6) {
            hour = std::stoi(timeStr.substr(0, 2));
            minute = std::stoi(timeStr.substr(2, 2));
            
            size_t posDot = timeStr.find('.');
            if (posDot != std::string::npos) {
                second = std::stoi(timeStr.substr(4, posDot - 4));
                std::string msStr = timeStr.substr(posDot + 1);
                while (msStr.length() < 3) msStr += '0';
                if (msStr.length() > 3) msStr = msStr.substr(0, 3);
                millisecond = std::stoi(msStr);
            } else {
                second = std::stoi(timeStr.substr(4, 2));
            }
        }
    }
    
    return hour * 3600.0 + minute * 60.0 + second + millisecond / 1000.0;
}

std::string TLLobBuilder::formatTimeStr(const std::string& rawTime) {
    // 已经是 HH:MM:SS.mmm 格式
    if (rawTime.find(':') != std::string::npos) {
        return rawTime;
    }
    // 转换 HHMMSS.mmm 为 HH:MM:SS.mmm
    if (rawTime.length() >= 6) {
        std::string result = rawTime.substr(0, 2) + ":" + 
                             rawTime.substr(2, 2) + ":" + 
                             rawTime.substr(4);
        return result;
    }
    return rawTime;
}

// ============================================================
// CSV 读取函数 (并行分块扫描, 参考 datacheck_final 的
// computeFileChunks/parseTLChunk 思路: 按字节区间切成 N 块,
// 每块独立线程扫描+解析+过滤, 最后按块序拼接, 结果与单线程完全一致)
// ============================================================

namespace {

struct FileChunk { std::streampos start, end; };

// 把 [headerEnd, fileSize) 切成 n 段, 每段边界都对齐到完整行(向后找下一个换行符)
std::vector<FileChunk> computeFileChunks(const std::string& filePath,
                                          std::streampos headerEnd, int n) {
    std::ifstream ifs(filePath, std::ios::binary);
    ifs.seekg(0, std::ios::end);
    std::streampos fileSize = ifs.tellg();

    std::vector<FileChunk> chunks;
    if (n <= 1 || fileSize <= headerEnd) {
        chunks.push_back({headerEnd, fileSize});
        return chunks;
    }
    int64_t total = static_cast<int64_t>(fileSize) - static_cast<int64_t>(headerEnd);
    int64_t chunkSize = total / n;
    std::streampos prevEnd = headerEnd;
    std::string tmp;
    for (int i = 0; i < n; ++i) {
        std::streampos realEnd;
        if (i == n - 1) {
            realEnd = fileSize;
        } else {
            std::streampos nominalEnd = headerEnd + static_cast<std::streamoff>(chunkSize * (i + 1));
            ifs.seekg(nominalEnd);
            std::getline(ifs, tmp);  // 消费这一行剩余部分, 对齐到行边界
            realEnd = ifs.eof() ? fileSize : ifs.tellg();
        }
        chunks.push_back({prevEnd, realEnd});
        prevEnd = realEnd;
        if (realEnd >= fileSize) break;
    }
    return chunks;
}

// 快速CSV拆分: 直接切子串(无 stringstream 开销), 与 datacheck_final::splitCSV 等价
void splitCSVFast(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.emplace_back(line, start, i - start);
            start = i + 1;
        }
    }
}

// 读某个文件的表头, 返回表头行结束的字节偏移(供 computeFileChunks 用)
std::streampos readHeaderEnd(const std::string& filename, bool& ok) {
    std::ifstream f(filename, std::ios::binary);
    ok = f.is_open();
    if (!ok) return 0;
    std::string header;
    std::getline(f, header);
    return f.tellg();
}

} // namespace

/**
 * 读取上海逐笔全息数据 (mdl_4_24)
 */
std::vector<SHTickRecord> readSHTickCSV(const std::string& filename, const std::string& symbol, int nWorkers) {
    bool ok = false;
    std::streampos headerEnd = readHeaderEnd(filename, ok);
    if (!ok) {
        std::cerr << "无法打开文件: " << filename << std::endl;
        return {};
    }

    int n = std::max(1, nWorkers);
    auto chunks = computeFileChunks(filename, headerEnd, n);

    auto parseChunk = [&filename, &symbol](FileChunk chunk) {
        std::vector<SHTickRecord> records;
        records.reserve(2000000);
        std::ifstream file(filename, std::ios::binary);
        file.seekg(chunk.start);
        std::string line;
        std::vector<std::string> tokens;

        while (true) {
            std::streampos pos = file.tellg();
            if (pos == std::streampos(-1) || pos >= chunk.end) break;
            if (!std::getline(file, line)) break;
            if (line.empty()) continue;

            splitCSVFast(line, tokens);
            if (tokens.size() < 13) continue;

            SHTickRecord rec;
            try {
                rec.bizIndex = std::stoll(tokens[0]);
                rec.channel = std::stoi(tokens[1]);
                rec.securityID = tokens[2];

                if (!symbol.empty() && rec.securityID != symbol) continue;

                rec.tickTime = tokens[3];
                rec.type = tokens[4].empty() ? ' ' : tokens[4][0];
                rec.buyOrderNO = std::stoll(tokens[5]);
                rec.sellOrderNO = std::stoll(tokens[6]);
                rec.price = std::stod(tokens[7]);
                rec.qty = std::stoll(tokens[8]);
                rec.tradeMoney = std::stod(tokens[9]);
                rec.tickBSFlag = tokens[10].empty() ? ' ' : tokens[10][0];
                rec.localTime = tokens[11];
                rec.seqNo = std::stoll(tokens[12]);

                rec.timeSeconds = TLLobBuilder::parseTimeToSeconds(rec.tickTime);
                rec.intPrice = static_cast<int64_t>((rec.price + 1e-6) * 100);

                if (rec.type == 'A' || rec.type == 'D' || rec.type == 'T') {
                    records.push_back(std::move(rec));
                }
            } catch (const std::exception&) {
                continue;
            }
        }
        return records;
    };

    std::vector<std::future<std::vector<SHTickRecord>>> futs;
    futs.reserve(chunks.size());
    for (auto& ch : chunks) futs.push_back(std::async(std::launch::async, parseChunk, ch));

    // 按块序(非完成序)拼接, 保持与单线程扫描完全一致的相对顺序
    std::vector<std::vector<SHTickRecord>> parts;
    parts.reserve(futs.size());
    size_t total = 0;
    for (auto& f : futs) { parts.push_back(f.get()); total += parts.back().size(); }

    std::vector<SHTickRecord> records;
    records.reserve(total);
    for (auto& p : parts) {
        records.insert(records.end(), std::make_move_iterator(p.begin()), std::make_move_iterator(p.end()));
    }

    std::cout << "读取上海数据: " << records.size() << " 条" << std::endl;
    return records;
}

/**
 * 读取深圳委托数据 (mdl_6_33)
 * 
 * 实际数据格式 (从运行结果):
 * ChannelNo,ApplSeqNum,MDStreamID,SecurityID,SecurityIDSource,Price,OrderQty,Side,TransactTime,OrdType,LocalTime,SeqNo
 * 2013,1,11,2009,102,24.67,223000,49,09:15:00.000,50,09:15:00.019,1
 * 
 * 注意: 根据实际输出，SecurityID是第4列(index 3)，Price是实际价格
 * Side: 49='1'=买, 50='2'=卖
 */
std::vector<SZOrderRecord> readSZOrderCSV(const std::string& filename, const std::string& symbol, int nWorkers) {
    bool ok = false;
    std::streampos headerEnd = readHeaderEnd(filename, ok);
    if (!ok) {
        std::cerr << "无法打开文件: " << filename << std::endl;
        return {};
    }

    int n = std::max(1, nWorkers);
    auto chunks = computeFileChunks(filename, headerEnd, n);

    auto parseChunk = [&filename, &symbol](FileChunk chunk) {
        std::vector<SZOrderRecord> records;
        records.reserve(2000000);
        std::ifstream file(filename, std::ios::binary);
        file.seekg(chunk.start);
        std::string line;
        std::vector<std::string> tokens;

        while (true) {
            std::streampos pos = file.tellg();
            if (pos == std::streampos(-1) || pos >= chunk.end) break;
            if (!std::getline(file, line)) break;
            if (line.empty()) continue;

            // 去除行尾的 \r (Windows CRLF)
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            splitCSVFast(line, tokens);
            // 处理尾部逗号导致的额外空字段
            if (!tokens.empty() && tokens.back().empty()) tokens.pop_back();
            if (tokens.size() < 12) continue;

            SZOrderRecord rec;
            try {
                rec.channelNo = std::stoi(tokens[0]);
                rec.applSeqNum = std::stoll(tokens[1]);
                rec.mdStreamID = std::stoi(tokens[2]);
                rec.securityID = tokens[3];

                // 过滤证券代码 - SecurityID可能是短格式如 "2009", symbol是6位格式如 "002009"
                if (!symbol.empty()) {
                    std::string paddedID = rec.securityID;
                    while (paddedID.length() < 6) paddedID = "0" + paddedID;
                    if (paddedID != symbol) continue;
                }

                rec.price = std::stod(tokens[5]);
                rec.orderQty = std::stoll(tokens[6]);
                rec.side = std::stoi(tokens[7]);
                rec.transactTime = tokens[8];
                rec.ordType = tokens[9].empty() ? 0 : std::stoi(tokens[9]);
                rec.localTime = tokens[10];
                rec.seqNo = tokens[11].empty() || tokens[11] == "NaN" ? 0 : std::stoll(tokens[11]);

                rec.timeSeconds = TLLobBuilder::parseTimeToSeconds(rec.transactTime);
                rec.intPrice = static_cast<int64_t>((rec.price + 1e-6) * 100);
                rec.direction = (rec.side == SZ_SIDE_BUY) ? 'B' : 'S';

                records.push_back(std::move(rec));
            } catch (const std::exception&) {
                continue;
            }
        }
        return records;
    };

    std::vector<std::future<std::vector<SZOrderRecord>>> futs;
    futs.reserve(chunks.size());
    for (auto& ch : chunks) futs.push_back(std::async(std::launch::async, parseChunk, ch));

    std::vector<std::vector<SZOrderRecord>> parts;
    parts.reserve(futs.size());
    size_t total = 0;
    for (auto& f : futs) { parts.push_back(f.get()); total += parts.back().size(); }

    std::vector<SZOrderRecord> records;
    records.reserve(total);
    for (auto& p : parts) {
        records.insert(records.end(), std::make_move_iterator(p.begin()), std::make_move_iterator(p.end()));
    }

    // 打印SeqNo统计信息，用于调试
    if (!records.empty()) {
        int64_t minSeq = records[0].seqNo;
        int64_t maxSeq = records[0].seqNo;
        int64_t zeroCount = 0;
        for (const auto& r : records) {
            if (r.seqNo < minSeq) minSeq = r.seqNo;
            if (r.seqNo > maxSeq) maxSeq = r.seqNo;
            if (r.seqNo == 0) zeroCount++;
        }
        std::cout << "  SeqNo范围: " << minSeq << " ~ " << maxSeq
                  << ", 为0的记录数: " << zeroCount << std::endl;
    }

    std::cout << "读取深圳委托数据: " << records.size() << " 条" << std::endl;
    return records;
}

/**
 * 读取深圳成交数据 (mdl_6_36)
 * 
 * 实际数据格式 (从运行结果):
 * ChannelNo,ApplSeqNum,MDStreamID,BidApplSeqNum,OfferApplSeqNum,SecurityID,SecurityIDSource,LastPx,LastQty,ExecType,TransactTime,LocalTime,SeqNo
 * 2035,512,11,511,0,127042,102,0.0,1720,52,09:15:00.040,09:15:00.050,1
 * 
 * 注意: 从输出看，SecurityID是第6列(index 5)，但值是127042
 * 而第4列(BidApplSeqNum=511)和第5列(OfferApplSeqNum=0)看起来正确
 * LastPx=0.0, LastQty=1720, ExecType=52
 * 
 * 但数据可能有错位，需要根据表头确认
 * 实际表头: ChannelNo,ApplSeqNum,MDStreamID,BidApplSeqNum,OfferApplSeqNum,SecurityID,SecurityIDSource,LastPx,LastQty,ExecType,TransactTime,LocalTime,SeqNo
 */
std::vector<SZTradeRecord> readSZTradeCSV(const std::string& filename, const std::string& symbol, int nWorkers) {
    bool ok = false;
    std::streampos headerEnd = readHeaderEnd(filename, ok);
    if (!ok) {
        std::cerr << "无法打开文件: " << filename << std::endl;
        return {};
    }

    int n = std::max(1, nWorkers);
    auto chunks = computeFileChunks(filename, headerEnd, n);

    auto parseChunk = [&filename, &symbol](FileChunk chunk) {
        std::vector<SZTradeRecord> records;
        records.reserve(2000000);
        std::ifstream file(filename, std::ios::binary);
        file.seekg(chunk.start);
        std::string line;
        std::vector<std::string> tokens;

        while (true) {
            std::streampos pos = file.tellg();
            if (pos == std::streampos(-1) || pos >= chunk.end) break;
            if (!std::getline(file, line)) break;
            if (line.empty()) continue;

            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            splitCSVFast(line, tokens);
            if (!tokens.empty() && tokens.back().empty()) tokens.pop_back();
            if (tokens.size() < 13) continue;

            SZTradeRecord rec;
            try {
                rec.channelNo = std::stoi(tokens[0]);
                rec.applSeqNum = std::stoll(tokens[1]);
                rec.mdStreamID = std::stoi(tokens[2]);
                rec.bidApplSeqNum = std::stoll(tokens[3]);
                rec.offerApplSeqNum = std::stoll(tokens[4]);
                rec.securityID = tokens[5];

                if (!symbol.empty()) {
                    std::string paddedID = rec.securityID;
                    while (paddedID.length() < 6) paddedID = "0" + paddedID;
                    if (paddedID != symbol) continue;
                }

                rec.lastPx = std::stod(tokens[7]);
                rec.lastQty = std::stoll(tokens[8]);
                rec.execType = tokens[9].empty() ? 0 : std::stoi(tokens[9]);
                rec.transactTime = tokens[10];
                rec.localTime = tokens[11];
                rec.seqNo = (tokens.size() > 12 && !tokens[12].empty() && tokens[12] != "NaN") ? std::stoll(tokens[12]) : 0;

                rec.timeSeconds = TLLobBuilder::parseTimeToSeconds(rec.transactTime);
                rec.intPrice = static_cast<int64_t>((rec.lastPx + 1e-6) * 100);

                records.push_back(std::move(rec));
            } catch (const std::exception&) {
                continue;
            }
        }
        return records;
    };

    std::vector<std::future<std::vector<SZTradeRecord>>> futs;
    futs.reserve(chunks.size());
    for (auto& ch : chunks) futs.push_back(std::async(std::launch::async, parseChunk, ch));

    std::vector<std::vector<SZTradeRecord>> parts;
    parts.reserve(futs.size());
    size_t total = 0;
    for (auto& f : futs) { parts.push_back(f.get()); total += parts.back().size(); }

    std::vector<SZTradeRecord> records;
    records.reserve(total);
    for (auto& p : parts) {
        records.insert(records.end(), std::make_move_iterator(p.begin()), std::make_move_iterator(p.end()));
    }

    // 打印SeqNo统计信息，用于调试
    if (!records.empty()) {
        int64_t minSeq = records[0].seqNo;
        int64_t maxSeq = records[0].seqNo;
        int64_t zeroCount = 0;
        for (const auto& r : records) {
            if (r.seqNo < minSeq) minSeq = r.seqNo;
            if (r.seqNo > maxSeq) maxSeq = r.seqNo;
            if (r.seqNo == 0) zeroCount++;
        }
        std::cout << "  SeqNo范围: " << minSeq << " ~ " << maxSeq
                  << ", 为0的记录数: " << zeroCount << std::endl;
    }

    std::cout << "读取深圳成交数据: " << records.size() << " 条" << std::endl;
    return records;
}

// ============================================================
// TLLobBuilder 实现
// ============================================================

TLLobBuilder::TLLobBuilder(const std::string& symbol, const std::string& tradeDate, Market market)
    : symbol_(symbol), tradeDate_(tradeDate), market_(market) {
}

void TLLobBuilder::loadSHData(const std::string& filePath) {
    auto shRecords = readSHTickCSV(filePath, symbol_);
    
    records_.reserve(shRecords.size());
    
    for (auto& rec : shRecords) {
        UnifiedRecord unified;
        // SH 排序键用 BizIndex(业务序号): 单文件内对委托/撤单/成交全局单调有序。
        // 末列 SeqNo 不保证跨类型先后，可能让 wt 落到 zb 后面。
        unified.seqNo = rec.bizIndex;
        unified.time = rec.tickTime;
        unified.timeSeconds = rec.timeSeconds;
        
        // 根据Type设置actionType
        if (rec.type == SH_TYPE_ORDER) {
            unified.actionType = 0;  // 委托
            // 上海委托: TickBSFlag=B时是买，sysid用BuyOrderNO
            // TickBSFlag=S时是卖，sysid用SellOrderNO
            if (rec.tickBSFlag == 'B') {
                unified.direction = 'B';
                unified.sysid = rec.buyOrderNO;
            } else {
                unified.direction = 'S';
                unified.sysid = rec.sellOrderNO;
            }
        } else if (rec.type == SH_TYPE_CANCEL) {
            unified.actionType = 1;  // 撤单
            if (rec.tickBSFlag == 'B') {
                unified.direction = 'B';
                unified.sysid = rec.buyOrderNO;
            } else {
                unified.direction = 'S';
                unified.sysid = rec.sellOrderNO;
            }
        } else if (rec.type == SH_TYPE_TRADE) {
            unified.actionType = 2;  // 成交
            unified.buyId = rec.buyOrderNO;
            unified.sellId = rec.sellOrderNO;
            unified.direction = rec.tickBSFlag;  // 主动方向
            // 上海 TickBSFlag: B=买方主动, S=卖方主动; N/空表示集合竞价无主动方，留 0 走 sysid 兜底。
            unified.tradeDirection = (rec.tickBSFlag == 'B') ? 1 :
                                     (rec.tickBSFlag == 'S') ? 2 : 0;
        } else {
            continue;
        }
        
        unified.price = rec.price;
        unified.intPrice = rec.intPrice;
        unified.volume = rec.qty;
        unified.turnover = rec.tradeMoney;
        unified.priceType = '2';  // 默认限价
        
        records_.push_back(std::move(unified));
    }
    
    resequenceSHOrderBeforeTrade();
    std::sort(records_.begin(), records_.end(),
              [](const UnifiedRecord& a, const UnifiedRecord& b) {
                  if (a.seqNo != b.seqNo) return a.seqNo < b.seqNo;
                  return a.actionType < b.actionType;
              });
    
    std::cout << "加载上海统一记录: " << records_.size() << " 条" << std::endl;
    
    // 构建虚拟订单 (上海市场)
    buildDerivedOrders();
}

void TLLobBuilder::loadSHDataFromMemory(const std::vector<SHTickRecord>& shRecords) {
    records_.reserve(shRecords.size());
    
    for (const auto& rec : shRecords) {
        // 过滤证券代码
        if (!symbol_.empty() && rec.securityID != symbol_) continue;
        
        UnifiedRecord unified;
        // SH 排序键用 BizIndex(业务序号): 单文件内对委托/撤单/成交全局单调有序。
        // 末列 SeqNo 不保证跨类型先后，可能让 wt 落到 zb 后面。
        unified.seqNo = rec.bizIndex;
        unified.time = rec.tickTime;
        unified.timeSeconds = rec.timeSeconds;
        
        if (rec.type == SH_TYPE_ORDER) {
            unified.actionType = 0;
            unified.direction = rec.tickBSFlag;
            unified.sysid = (rec.tickBSFlag == 'B') ? rec.buyOrderNO : rec.sellOrderNO;
        } else if (rec.type == SH_TYPE_CANCEL) {
            unified.actionType = 1;
            unified.direction = rec.tickBSFlag;
            unified.sysid = (rec.tickBSFlag == 'B') ? rec.buyOrderNO : rec.sellOrderNO;
        } else if (rec.type == SH_TYPE_TRADE) {
            unified.actionType = 2;
            unified.buyId = rec.buyOrderNO;
            unified.sellId = rec.sellOrderNO;
            unified.direction = rec.tickBSFlag;
            unified.tradeDirection = (rec.tickBSFlag == 'B') ? 1 :
                                     (rec.tickBSFlag == 'S') ? 2 : 0;
        } else {
            continue;
        }
        
        unified.price = rec.price;
        unified.intPrice = rec.intPrice;
        unified.volume = rec.qty;
        unified.turnover = rec.tradeMoney;
        unified.priceType = '2';
        
        records_.push_back(std::move(unified));
    }
    
    resequenceSHOrderBeforeTrade();
    std::sort(records_.begin(), records_.end(),
              [](const UnifiedRecord& a, const UnifiedRecord& b) {
                  if (a.seqNo != b.seqNo) return a.seqNo < b.seqNo;
                  return a.actionType < b.actionType;
              });
    
    std::cout << "加载上海统一记录: " << records_.size() << " 条" << std::endl;
    buildDerivedOrders();
}

void TLLobBuilder::loadSZOrderData(const std::string& filePath) {
    auto szOrders = readSZOrderCSV(filePath, symbol_);
    
    for (auto& rec : szOrders) {
        UnifiedRecord unified;
        // 深圳数据: 记录原始 seqNo 用于调试，但排序按时间+类型+sysid
        unified.seqNo = rec.seqNo;
        unified.time = rec.transactTime;
        unified.timeSeconds = rec.timeSeconds;
        unified.actionType = 0;  // 委托
        unified.direction = rec.direction;
        unified.sysid = rec.applSeqNum;  // 深圳用ApplSeqNum作为订单号，也用于同时间排序
        unified.price = rec.price;
        unified.intPrice = rec.intPrice;
        unified.volume = rec.orderQty;
        unified.turnover = 0;
        // 根据ordType设置priceType: 49=市价, 50=限价, 85=本方最优
        if (rec.ordType == 49) {
            unified.priceType = '1';  // 市价
        } else if (rec.ordType == 85) {
            unified.priceType = 'U';  // 本方最优
        } else {
            unified.priceType = '2';  // 限价(默认)
        }
        
        records_.push_back(std::move(unified));
    }
    
    std::cout << "加载深圳委托记录: " << szOrders.size() << " 条" << std::endl;
}

void TLLobBuilder::loadSZOrderDataFromMemory(const std::vector<SZOrderRecord>& szOrders) {
    for (const auto& rec : szOrders) {
        // 过滤证券代码
        if (!symbol_.empty()) {
            std::string paddedID = rec.securityID;
            while (paddedID.length() < 6) {
                paddedID = "0" + paddedID;
            }
            if (paddedID != symbol_) continue;
        }
        
        UnifiedRecord unified;
        unified.seqNo = rec.seqNo;
        unified.time = rec.transactTime;
        unified.timeSeconds = rec.timeSeconds;
        unified.actionType = 0;
        unified.direction = rec.direction;
        unified.sysid = rec.applSeqNum;
        unified.price = rec.price;
        unified.intPrice = rec.intPrice;
        unified.volume = rec.orderQty;
        unified.turnover = 0;
        
        if (rec.ordType == 49) {
            unified.priceType = '1';
        } else if (rec.ordType == 85) {
            unified.priceType = 'U';
        } else {
            unified.priceType = '2';
        }
        
        records_.push_back(std::move(unified));
    }
    
    std::cout << "加载深圳委托记录: " << szOrders.size() << " 条" << std::endl;
}

void TLLobBuilder::loadSZTradeData(const std::string& filePath) {
    auto szTrades = readSZTradeCSV(filePath, symbol_);
    
    for (auto& rec : szTrades) {
        UnifiedRecord unified;
        // 深圳数据: 记录原始 seqNo 用于调试，但不用于排序
        unified.seqNo = rec.seqNo;
        unified.time = rec.transactTime;
        unified.timeSeconds = rec.timeSeconds;
        
        if (rec.execType == SZ_EXEC_CANCEL) {
            unified.actionType = 1;  // 撤单
            // 深圳撤单: bidApplSeqNum或offerApplSeqNum非0的那个
            if (rec.bidApplSeqNum > 0) {
                unified.direction = 'B';
                unified.sysid = rec.bidApplSeqNum;
            } else {
                unified.direction = 'S';
                unified.sysid = rec.offerApplSeqNum;
            }
        } else if (rec.execType == SZ_EXEC_TRADE) {
            unified.actionType = 2;  // 成交
            unified.buyId = rec.bidApplSeqNum;
            unified.sellId = rec.offerApplSeqNum;
            // 成交记录用 applSeqNum 作为 sysid，用于同时间同类型记录的排序
            unified.sysid = rec.applSeqNum;
        } else {
            continue;
        }
        
        unified.price = rec.lastPx;
        unified.intPrice = rec.intPrice;
        unified.volume = rec.lastQty;
        unified.turnover = rec.lastPx * rec.lastQty;
        unified.priceType = '2';  // 成交/撤单记录无订单类型信息
        
        records_.push_back(std::move(unified));
    }
    
    std::cout << "加载深圳成交/撤单记录: " << szTrades.size() << " 条" << std::endl;
    
    // 深圳数据排序: 
    // 注意: mdl_6_33 和 mdl_6_36 的 SeqNo 是各自独立的序列，不能直接用于全局排序
    // 正确的排序方式: 
    //   1. 按时间 (timeSeconds) 排序
    //   2. 同一时间内，按动作类型排序 (委托=0 < 撤单=1 < 成交=2)
    //   3. 同一时间同一类型，按 sysid 排序
    std::sort(records_.begin(), records_.end(),
              [](const UnifiedRecord& a, const UnifiedRecord& b) {
                  // 首先按时间排序
                  if (std::abs(a.timeSeconds - b.timeSeconds) > 0.0001) {
                      return a.timeSeconds < b.timeSeconds;
                  }
                  // 时间相同，按动作类型排序 (委托先于撤单先于成交)
                  if (a.actionType != b.actionType) {
                      return a.actionType < b.actionType;
                  }
                  // 类型也相同，按 sysid 排序
                  return a.sysid < b.sysid;
              });
    
    std::cout << "深圳统一记录排序完成: " << records_.size() << " 条" << std::endl;
    
    // 打印排序后的信息
    if (!records_.empty()) {
        std::cout << "  时间范围: " << records_.front().time << " ~ " << records_.back().time << std::endl;
        
        // 统计各类型记录数
        int orderCount = 0, cancelCount = 0, tradeCount = 0;
        for (const auto& r : records_) {
            if (r.actionType == 0) orderCount++;
            else if (r.actionType == 1) cancelCount++;
            else if (r.actionType == 2) tradeCount++;
        }
        std::cout << "  委托: " << orderCount << ", 撤单: " << cancelCount << ", 成交: " << tradeCount << std::endl;
    }
}

void TLLobBuilder::loadSZTradeDataFromMemory(const std::vector<SZTradeRecord>& szTrades) {
    for (const auto& rec : szTrades) {
        // 过滤证券代码
        if (!symbol_.empty()) {
            std::string paddedID = rec.securityID;
            while (paddedID.length() < 6) {
                paddedID = "0" + paddedID;
            }
            if (paddedID != symbol_) continue;
        }
        
        UnifiedRecord unified;
        unified.seqNo = rec.seqNo;
        unified.time = rec.transactTime;
        unified.timeSeconds = rec.timeSeconds;
        
        if (rec.execType == SZ_EXEC_CANCEL) {
            unified.actionType = 1;
            if (rec.bidApplSeqNum > 0) {
                unified.direction = 'B';
                unified.sysid = rec.bidApplSeqNum;
            } else {
                unified.direction = 'S';
                unified.sysid = rec.offerApplSeqNum;
            }
        } else if (rec.execType == SZ_EXEC_TRADE) {
            unified.actionType = 2;
            unified.buyId = rec.bidApplSeqNum;
            unified.sellId = rec.offerApplSeqNum;
            unified.sysid = rec.applSeqNum;
        } else {
            continue;
        }
        
        unified.price = rec.lastPx;
        unified.intPrice = rec.intPrice;
        unified.volume = rec.lastQty;
        unified.turnover = rec.lastPx * rec.lastQty;
        unified.priceType = '2';
        
        records_.push_back(std::move(unified));
    }
    
    std::cout << "加载深圳成交/撤单记录: " << szTrades.size() << " 条" << std::endl;
    
    std::sort(records_.begin(), records_.end(),
              [](const UnifiedRecord& a, const UnifiedRecord& b) {
                  if (a.timeSeconds != b.timeSeconds) return a.timeSeconds < b.timeSeconds;
                  if (a.actionType != b.actionType) return a.actionType < b.actionType;
                  return a.sysid < b.sysid;
              });
    
    std::cout << "深圳统一记录排序完成: " << records_.size() << " 条" << std::endl;
    
    if (!records_.empty()) {
        std::cout << "  时间范围: " << records_.front().time << " ~ " << records_.back().time << std::endl;
        
        int orderCount = 0, cancelCount = 0, tradeCount = 0;
        for (const auto& r : records_) {
            if (r.actionType == 0) orderCount++;
            else if (r.actionType == 1) cancelCount++;
            else if (r.actionType == 2) tradeCount++;
        }
        std::cout << "  委托: " << orderCount << ", 撤单: " << cancelCount << ", 成交: " << tradeCount << std::endl;
    }
}

bool TLLobBuilder::isContinuousTradingTime(double timeSeconds) const {
    if (CONTINUOUS_AM_START <= timeSeconds && timeSeconds < CONTINUOUS_AM_END) {
        return true;
    }
    // 沪深 A 股下午连续竞价均为 13:00-14:57，14:57-15:00 为收盘集合竞价。
    if (CONTINUOUS_PM_START <= timeSeconds && timeSeconds < CLOSE_AUCTION_START) {
        return true;
    }
    return false;
}

bool TLLobBuilder::isAuctionTradingTime(double timeSeconds) const {
    return (OPEN_AUCTION_START <= timeSeconds && timeSeconds < OPEN_AUCTION_END) ||
           (CLOSE_AUCTION_START <= timeSeconds && timeSeconds < CLOSE_AUCTION_END);
}

void TLLobBuilder::handleOrder(int64_t sysid, const std::string& timeStr, double price,
                               int64_t volume, char direction, double timeSeconds, char priceType) {
    if (volume <= 0 || sysid <= 0) return;

    auto order = std::make_unique<Order>(sysid, timeStr, price, volume, direction, timeSeconds);
    Order* orderPtr = order.get();

    int64_t intPrice = orderPtr->intPrice;
    bool isContinuous = isContinuousTradingTime(timeSeconds);
    bool isAuction = isAuctionTradingTime(timeSeconds);

    // 市价单(priceType=='1')在对手盘为空时 price 仍是占位值(解析不出真实价), 只登记
    // 备后续成交扣减、绝不挂簿, 否则 intPrice=0 会被当真挂进 bestBidList_[0]/bestAskList_[0]
    // 形成价格 0.00 的幽灵档(移植自 datacheck_final)。
    if (priceType == '1') {
        bool hasOpposite = (direction == 'B') ? !bestAskList_.empty() : !bestBidList_.empty();
        if (!hasOpposite) {
            allOrders_[sysid] = std::move(order);
            return;
        }
    }

    if (direction == 'B') {
        if (isContinuous && !bestAskList_.empty()) {
            int64_t bestAskIntPrice = bestAskList_.begin()->first;
            if (intPrice >= bestAskIntPrice) {
                waitEnqueue(orderPtr);
                allOrders_[sysid] = std::move(order);
                return;
            }
        }
        // 集合竞价委托直接进入全深度簿，允许交叉累积；虚拟撮合只在副本上 uncross。
        if (!isContinuous && !isAuction) {
            waitEnqueue(orderPtr);
            allOrders_[sysid] = std::move(order);
            return;
        }
        bidList_[orderPtr->getBidKey()] = orderPtr;
        bestBidList_[-intPrice] += volume;
    } else {
        if (isContinuous && !bestBidList_.empty()) {
            int64_t bestBidIntPrice = -bestBidList_.begin()->first;
            if (intPrice <= bestBidIntPrice) {
                waitEnqueue(orderPtr);
                allOrders_[sysid] = std::move(order);
                return;
            }
        }
        // 集合竞价委托直接进入全深度簿，允许交叉累积；虚拟撮合只在副本上 uncross。
        if (!isContinuous && !isAuction) {
            waitEnqueue(orderPtr);
            allOrders_[sysid] = std::move(order);
            return;
        }
        askList_[orderPtr->getAskKey()] = orderPtr;
        bestAskList_[intPrice] += volume;
    }

    allOrders_[sysid] = std::move(order);

    if (isContinuous) {
        checkWaitQueueReturn();
    }
}

bool TLLobBuilder::handleCancel(int64_t sysid, const std::string& timeStr, int64_t volume,
                                double& outPrice, int64_t& outVolume, char& outDirection) {
    auto it = allOrders_.find(sysid);
    if (it == allOrders_.end()) {
        return false;
    }
    
    Order* order = it->second.get();
    if (!order) {
        return false;
    }
    
    outPrice = order->price;
    outDirection = order->direction;
    
    // 检查订单量有效性
    if (order->volume <= 0) {
        outVolume = 0;
        return false;
    }
    
    // 计算撤单量
    int64_t cancelVolume = (volume > 0) ? std::min(volume, order->volume) : order->volume;
    outVolume = cancelVolume;
    
    // 更新订单量
    order->volume -= cancelVolume;
    
    // 从Wait Queue或订单簿中移除/更新
    if (order->direction == 'B') {
        if (bidWaitQueue_.count(sysid)) {
            if (order->volume <= 0) {
                waitDequeue(order);
            }
        } else {
            auto key = order->getBidKey();
            if (order->volume <= 0 && bidList_.count(key)) {
                bidList_.erase(key);
            }
            bestBidList_[-order->intPrice] -= cancelVolume;
            if (bestBidList_[-order->intPrice] <= 0) {
                bestBidList_.erase(-order->intPrice);
            }
        }
    } else {
        if (askWaitQueue_.count(sysid)) {
            if (order->volume <= 0) {
                waitDequeue(order);
            }
        } else {
            auto key = order->getAskKey();
            if (order->volume <= 0 && askList_.count(key)) {
                askList_.erase(key);
            }
            bestAskList_[order->intPrice] -= cancelVolume;
            if (bestAskList_[order->intPrice] <= 0) {
                bestAskList_.erase(order->intPrice);
            }
        }
    }
    
    // 撤单后检查 Wait Queue 回归（卖单撤单可能使买单可以回归）
    checkWaitQueueReturn();
    
    return true;
}

void TLLobBuilder::handleTrade(int64_t buyId, int64_t sellId, double price, int64_t volume,
                               const std::string& timeStr, int tradeDirection,
                               char& activeDir, int64_t& activeSysid, int64_t& passiveSysid,
                               int64_t tradeSeqNo) {
    cumulativeVolume_ += volume;
    
    Order* buyOrder = nullptr;
    Order* sellOrder = nullptr;
    
    auto itBuy = allOrders_.find(buyId);
    if (itBuy != allOrders_.end()) {
        buyOrder = itBuy->second.get();
    }
    
    auto itSell = allOrders_.find(sellId);
    if (itSell != allOrders_.end()) {
        sellOrder = itSell->second.get();
    }
    
    // 与 flow_lob_builder 一致：判断主动方向
    // tradeDirection: 1=买方主动, 2=卖方主动 (上海市场有此信息)
    if (tradeDirection == 1 || tradeDirection == 2) {
        activeDir = (tradeDirection == 1) ? 'B' : 'S';
        activeSysid = (tradeDirection == 1) ? buyId : sellId;
        passiveSysid = (tradeDirection == 1) ? sellId : buyId;
    } else {
        // 深圳市场没有 tradeDirection，用 sysid 大的是主动方
        activeDir = (buyId > sellId) ? 'B' : 'S';
        activeSysid = std::max(buyId, sellId);
        passiveSysid = std::min(buyId, sellId);
    }
    
    // 更新订单量
    auto updateOrder = [this](Order* order, int64_t volume) {
        if (!order) return;
        int64_t applied = std::min(volume, order->volume);
        if (applied <= 0) return;
        order->volume -= applied;
        
        if (order->direction == 'B') {
            if (bidWaitQueue_.count(order->sysid)) {
                if (order->volume <= 0) {
                    waitDequeue(order);
                }
            } else {
                auto key = order->getBidKey();
                if (order->volume <= 0 && bidList_.count(key)) {
                    bidList_.erase(key);
                }
                bestBidList_[-order->intPrice] -= applied;
                if (bestBidList_[-order->intPrice] <= 0) {
                    bestBidList_.erase(-order->intPrice);
                }
            }
        } else {
            if (askWaitQueue_.count(order->sysid)) {
                if (order->volume <= 0) {
                    waitDequeue(order);
                }
            } else {
                auto key = order->getAskKey();
                if (order->volume <= 0 && askList_.count(key)) {
                    askList_.erase(key);
                }
                bestAskList_[order->intPrice] -= applied;
                if (bestAskList_[order->intPrice] <= 0) {
                    bestAskList_.erase(order->intPrice);
                }
            }
        }
    };
    
    // "先吃后挂"单(A 被 resequence 提前): A_qty 已净掉原始 A 序号之前的成交, 该部分不再扣减,
    // 只扣对手方; 否则挂住残量被双扣(先吃部分扣两次)。(Fix5, 移植自 datacheck_final)
    auto isPreAFill = [this](int64_t sysid, int64_t seq) {
        auto it = shOrderOrigSeq_.find(sysid);
        return it != shOrderOrigSeq_.end() && seq > 0 && seq < it->second;
    };
    if (!isPreAFill(buyId, tradeSeqNo))  updateOrder(buyOrder, volume);
    if (!isPreAFill(sellId, tradeSeqNo)) updateOrder(sellOrder, volume);

    // 每次成交后检查 Wait Queue 回归
    checkWaitQueueReturn();
}

void TLLobBuilder::waitEnqueue(Order* o) {
    if (o->direction == 'B') {
        bidWaitQueue_[o->sysid] = o;
        bidWaitByPrice_[o->intPrice].push_back(o);
    } else {
        askWaitQueue_[o->sysid] = o;
        askWaitByPrice_[o->intPrice].push_back(o);
    }
}

void TLLobBuilder::waitDequeue(Order* o) {
    if (o->direction == 'B') {
        bidWaitQueue_.erase(o->sysid);
        auto mit = bidWaitByPrice_.find(o->intPrice);
        if (mit != bidWaitByPrice_.end()) {
            auto& v = mit->second;
            v.erase(std::remove(v.begin(), v.end(), o), v.end());
            if (v.empty()) bidWaitByPrice_.erase(mit);
        }
    } else {
        askWaitQueue_.erase(o->sysid);
        auto mit = askWaitByPrice_.find(o->intPrice);
        if (mit != askWaitByPrice_.end()) {
            auto& v = mit->second;
            v.erase(std::remove(v.begin(), v.end(), o), v.end());
            if (v.empty()) askWaitByPrice_.erase(mit);
        }
    }
}

void TLLobBuilder::checkWaitQueueReturn() {
    // 只在严格离开对手一档时回簿，价等于对手一档的锁价残量留在等待队列。
    // 用按价索引只遍历「该回簿」的价位(O(回簿) 而非 O(队列)), 避免激进单堆积时
    // 每事件全量扫描退化为 O(n²)(移植自 datacheck_final)。
    // 买单 intPrice < 卖一 -> 索引中 [begin, lower_bound(卖一)); 无卖盘则全回(卖一=+∞)。
    int64_t bestAskIP = bestAskList_.empty() ? INT64_MAX : bestAskList_.begin()->first;
    auto bidEnd = bidWaitByPrice_.lower_bound(bestAskIP);  // 首个 >= 卖一(留队列); 之前的全回
    for (auto it = bidWaitByPrice_.begin(); it != bidEnd; ) {
        for (Order* o : it->second) {
            if (o->volume > 0) {
                bidList_[o->getBidKey()] = o;
                bestBidList_[-o->intPrice] += o->volume;
            }
            bidWaitQueue_.erase(o->sysid);
        }
        it = bidWaitByPrice_.erase(it);
    }

    // 卖单 intPrice > 买一 -> 索引中 (upper_bound(买一), end); 无买盘则全回(买一=-∞)。
    int64_t bestBidIP = bestBidList_.empty() ? INT64_MIN : -bestBidList_.begin()->first;
    for (auto it = askWaitByPrice_.upper_bound(bestBidIP); it != askWaitByPrice_.end(); ) {
        for (Order* o : it->second) {
            if (o->volume > 0) {
                askList_[o->getAskKey()] = o;
                bestAskList_[o->intPrice] += o->volume;
            }
            askWaitQueue_.erase(o->sysid);
        }
        it = askWaitByPrice_.erase(it);
    }
}

void TLLobBuilder::resequenceSHOrderBeforeTrade() {
    std::unordered_map<int64_t, int64_t> firstTradeBiz;
    for (const auto& r : records_) {
        if (r.actionType != 2) continue;
        if (r.buyId > 0) {
            auto it = firstTradeBiz.find(r.buyId);
            if (it == firstTradeBiz.end() || r.seqNo < it->second) firstTradeBiz[r.buyId] = r.seqNo;
        }
        if (r.sellId > 0) {
            auto it = firstTradeBiz.find(r.sellId);
            if (it == firstTradeBiz.end() || r.seqNo < it->second) firstTradeBiz[r.sellId] = r.seqNo;
        }
    }
    // 若委托的 seqNo 晚于其首笔成交，把排序键提前到首笔成交的 seqNo。
    // 同时记录原始序号: 这类"先吃后挂"单的 A_qty 是挂住量(交易所已净掉 A 前的立即成交),
    // handleTrade 对 seq < 原始A序号 的成交必须跳过主动方扣减, 否则双重扣减(Fix5)。
    for (auto& r : records_) {
        if (r.actionType != 0) continue;
        auto it = firstTradeBiz.find(r.sysid);
        if (it != firstTradeBiz.end() && it->second < r.seqNo) {
            shOrderOrigSeq_[r.sysid] = r.seqNo;
            r.seqNo = it->second;
        }
    }
}

void TLLobBuilder::buildDerivedOrders() {
    // 与 flow_lob_builder.cpp 逻辑一致
    // 遍历记录，找出成交中引用但没有委托记录的订单
    
    // 首先收集所有委托记录的订单号
    std::unordered_set<int64_t> existingOrderSysids;
    existingOrderSysids.reserve(records_.size() / 2);
    
    for (const auto& rec : records_) {
        if (rec.actionType == 0) {  // 委托
            existingOrderSysids.insert(rec.sysid);
        }
    }
    
    // 遍历成交记录，找出没有委托记录的订单
    for (const auto& rec : records_) {
        if (rec.actionType != 2) continue;  // 只处理成交
        
        int64_t buyId = rec.buyId;
        int64_t sellId = rec.sellId;
        double price = rec.price;
        int64_t volume = rec.volume;
        
        // 处理买方订单
        if (buyId > 0 && existingOrderSysids.find(buyId) == existingOrderSysids.end()) {
            auto it = derivedOrders_.find(buyId);
            if (it == derivedOrders_.end()) {
                // 新建虚拟订单信息
                DerivedOrderInfo info;
                info.sysid = buyId;
                info.direction = 'B';
                info.price = price;
                info.totalVolume = volume;
                info.timeStr = rec.time;
                info.timeSeconds = rec.timeSeconds;
                info.intPrice = rec.intPrice;
                derivedOrders_[buyId] = info;
            } else {
                // 累加成交量，取最早时间
                it->second.totalVolume += volume;
                if (rec.timeSeconds < it->second.timeSeconds) {
                    it->second.timeStr = rec.time;
                    it->second.timeSeconds = rec.timeSeconds;
                }
            }
        }
        
        // 处理卖方订单
        if (sellId > 0 && existingOrderSysids.find(sellId) == existingOrderSysids.end()) {
            auto it = derivedOrders_.find(sellId);
            if (it == derivedOrders_.end()) {
                // 新建虚拟订单信息
                DerivedOrderInfo info;
                info.sysid = sellId;
                info.direction = 'S';
                info.price = price;
                info.totalVolume = volume;
                info.timeStr = rec.time;
                info.timeSeconds = rec.timeSeconds;
                info.intPrice = rec.intPrice;
                derivedOrders_[sellId] = info;
            } else {
                // 累加成交量
                it->second.totalVolume += volume;
                if (rec.timeSeconds < it->second.timeSeconds) {
                    it->second.timeStr = rec.time;
                    it->second.timeSeconds = rec.timeSeconds;
                }
            }
        }
    }
    
    if (verbose_ && !derivedOrders_.empty()) {
        std::cout << "发现 " << derivedOrders_.size()
                  << " 个虚拟订单 (成交引用但无委托记录)" << std::endl;
    }

    // 预计算每张委托的「成交量+撤单量」之和，供 build() 输出 wt 行时取
    // max(A_qty, orderFilled_) 补全展示委托量 —— SH 交易所对立即成交的主动单，
    // A_qty 只记录了挂住部分，可能小于实际下单量(移植自 datacheck_final)。
    // 同时记录每张委托的成交价极值(买单最高价/卖单最低价), 供派生单还原真实限价。
    for (const auto& r : records_) {
        if (r.actionType == 2) {
            if (r.buyId  > 0) orderFilled_[r.buyId]  += r.volume;
            if (r.sellId > 0) orderFilled_[r.sellId] += r.volume;
            // 价补全: 买单最高成交价 / 卖单最低成交价 (= 该单真实限价)
            if (r.buyId > 0) {
                auto it = orderBuyMaxPx_.find(r.buyId);
                if (it == orderBuyMaxPx_.end()) orderBuyMaxPx_[r.buyId] = r.price;
                else if (r.price > it->second) it->second = r.price;
            }
            if (r.sellId > 0) {
                auto it = orderSellMinPx_.find(r.sellId);
                if (it == orderSellMinPx_.end()) orderSellMinPx_[r.sellId] = r.price;
                else if (r.price < it->second) it->second = r.price;
            }
        } else if (r.actionType == 1) {
            if (r.sysid > 0) orderFilled_[r.sysid] += r.volume;
        }
    }
}

bool TLLobBuilder::createDerivedOrder(int64_t sysid, const std::string& timeStr, LobRecord& record) {
    auto it = derivedOrders_.find(sysid);
    if (it == derivedOrders_.end()) {
        return false;
    }
    
    // 检查是否已存在
    if (allOrders_.find(sysid) != allOrders_.end()) {
        return false;
    }
    
    const DerivedOrderInfo& info = it->second;

    // 价补全(仅派生单): 派生单无 A 委托记录, info.price 是首笔成交价(入场价)非限价 ->
    // 买单还原最高成交价、卖单还原最低成交价(=限价); 否则残量挂错顶档、type 分类被带翻。
    double dprice = info.price;
    if (info.direction == 'B') {
        auto pit = orderBuyMaxPx_.find(sysid);
        if (pit != orderBuyMaxPx_.end() && pit->second > dprice) dprice = pit->second;
    } else {
        auto pit = orderSellMinPx_.find(sysid);
        if (pit != orderSellMinPx_.end() && pit->second < dprice) dprice = pit->second;
    }

    // 计算盘口位置和类型
    auto [bidPos, askPos] = calcPosition(dprice, info.direction);
    int orderType = calcType(dprice, info.totalVolume, info.direction);

    // 填充记录
    record.time = info.timeStr;
    record.sysid = sysid;
    record.ordertype = "wt";
    record.direction = info.direction;
    record.price = dprice;
    record.volume = info.totalVolume;
    record.tradeVolume = cumulativeVolume_;
    record.bidPosition = bidPos;
    record.askPosition = askPos;
    record.type = orderType;
    getCurrentLob(record);

    // 计算 bid_distance, ask_distance, bid_ratio, ask_ratio
    if (record.bdp[0] > 0) {
        record.bidDistance = record.bdp[0] - dprice;
        if (record.bdv[0] > 0) {
            record.bidRatio = static_cast<double>(info.totalVolume) / static_cast<double>(record.bdv[0]);
        }
    }
    if (record.akp[0] > 0) {
        record.askDistance = dprice - record.akp[0];
        if (record.akv[0] > 0) {
            record.askRatio = static_cast<double>(info.totalVolume) / static_cast<double>(record.akv[0]);
        }
    }
    // 虚拟订单没有上一条委托时间参考，time_diff = 0
    record.timeDiff = 0;

    // 添加到订单簿
    handleOrder(sysid, info.timeStr, dprice, info.totalVolume, info.direction, info.timeSeconds);

    return true;
}

void TLLobBuilder::getCurrentLob(LobRecord& record) const {
    // Bid side
    int level = 0;
    for (auto& [key, volume] : bestBidList_) {
        if (level >= LOB_LEVELS) break;
        int64_t intPrice = -key;
        record.bdp[level] = intPrice / 100.0;
        record.bdv[level] = volume;
        level++;
    }
    record.bidLevels = level;
    
    // Ask side
    level = 0;
    for (auto& [key, volume] : bestAskList_) {
        if (level >= LOB_LEVELS) break;
        record.akp[level] = key / 100.0;
        record.akv[level] = volume;
        level++;
    }
    record.askLevels = level;
}

// ============================================================
// 集合竞价虚拟撮合 (uncross)
// ============================================================
//
// 对"当前全深度订单簿"做一次集合竞价撮合, 产出:
//   close       = 撮合均衡价 (最大成交量价; A 股集合竞价规则)
//   totalVolume = 可撮合最大成交量
//   totalAmt    = close * totalVolume
//   totalNum    = 订单级 FIFO 配对产生的成交笔数
//   bdp/bdv/akp/akv = 撮合后剩余盘口前 10 档
//
// 不改动真实订单簿 (bestBidList_/bidList_ 等), 全部在副本上计算。
// 集合竞价时段内快照撮合纯属虚拟指示，不改动真实订单簿。
//
// 注: close 的撮合价选取采用 A 股规则 —— 最大成交量优先, 其次买卖不平衡量
//     最小, 再次价高者。若需与交易所"最接近参考价"口径完全一致, 在此处调整
//     tie-break 即可 (参考价/前收盘价目前未接入)。
void TLLobBuilder::computeAuctionSnapshot(const std::string& timeStr, ActionRecord& record) const {
    record.time = timeStr;

    // 整秒 17 位 dateTime: YYYYMMDDHHMMSS000 (毫秒恒 000 -> float64 精确)
    int64_t dateInt = 0;
    try { dateInt = std::stoll(tradeDate_); } catch (...) { dateInt = 0; }
    int hh = 0, mm = 0, ss = 0;
    if (timeStr.size() >= 8) {  // "HH:MM:SS..."
        hh = (timeStr[0] - '0') * 10 + (timeStr[1] - '0');
        mm = (timeStr[3] - '0') * 10 + (timeStr[4] - '0');
        ss = (timeStr[6] - '0') * 10 + (timeStr[7] - '0');
    }
    record.dateTime = dateInt * 1000000000LL +
                      static_cast<int64_t>(hh * 10000 + mm * 100 + ss) * 1000LL;

    // 残余盘口副本 (无撮合时即为当前盘口)
    std::map<int64_t, int64_t> resBid = bestBidList_;  // -intPrice -> vol (升序: 高价在前)
    std::map<int64_t, int64_t> resAsk = bestAskList_;  //  intPrice -> vol (升序: 低价在前)

    if (!bestBidList_.empty() && !bestAskList_.empty()) {
        // --- 1. 全深度求撮合价 p* 与最大成交量 ---
        std::vector<int64_t> prices;
        prices.reserve(bestBidList_.size() + bestAskList_.size());
        for (const auto& kv : bestBidList_) prices.push_back(-kv.first);
        for (const auto& kv : bestAskList_) prices.push_back(kv.first);
        std::sort(prices.begin(), prices.end());
        prices.erase(std::unique(prices.begin(), prices.end()), prices.end());

        int64_t bestExec = 0, bestImb = INT64_MAX, clearIntPrice = 0;
        int64_t bestDemand = 0, bestSupply = 0;  // 撮合价处买/卖累计量 (供失衡)
        bool found = false;
        for (int64_t p : prices) {
            // demand: 买价 >= p (bestBidList_ 高价在前, 一旦 < p 即可停)
            int64_t demand = 0;
            for (const auto& kv : bestBidList_) {
                int64_t bp = -kv.first;
                if (bp >= p) demand += kv.second; else break;
            }
            // supply: 卖价 <= p (bestAskList_ 低价在前, 一旦 > p 即可停)
            int64_t supply = 0;
            for (const auto& kv : bestAskList_) {
                if (kv.first <= p) supply += kv.second; else break;
            }
            int64_t exec = std::min(demand, supply);
            if (exec <= 0) continue;
            int64_t imb = (demand > supply) ? (demand - supply) : (supply - demand);
            if (exec > bestExec ||
                (exec == bestExec && imb < bestImb) ||
                (exec == bestExec && imb == bestImb && p > clearIntPrice)) {
                bestExec = exec; bestImb = imb; clearIntPrice = p; found = true;
                bestDemand = demand; bestSupply = supply;
            }
        }

        if (found && bestExec > 0) {
            record.close = clearIntPrice / 100.0;
            record.totalVolume = bestExec;
            record.totalAmt = record.close * static_cast<double>(bestExec);

            // 撮合价处失衡 (+ = 买方剩余, 看涨)
            record.imbalanceVol = static_cast<double>(bestDemand - bestSupply);
            int64_t denom = bestDemand + bestSupply;
            if (denom > 0)
                record.imbalanceRatio =
                    static_cast<double>(bestDemand - bestSupply) / static_cast<double>(denom);

            // --- 2. 残余盘口: 价优先扣减 bestExec ---
            int64_t rem = bestExec;  // 买方: 从最高价(begin)开始
            for (auto it = resBid.begin(); it != resBid.end() && rem > 0; ) {
                int64_t take = std::min(rem, it->second);
                it->second -= take; rem -= take;
                if (it->second <= 0) it = resBid.erase(it); else ++it;
            }
            rem = bestExec;          // 卖方: 从最低价(begin)开始
            for (auto it = resAsk.begin(); it != resAsk.end() && rem > 0; ) {
                int64_t take = std::min(rem, it->second);
                it->second -= take; rem -= take;
                if (it->second <= 0) it = resAsk.erase(it); else ++it;
            }

            // --- 3. totalNum: 订单级 FIFO 配对成交笔数 ---
            // bidList_ 按 {-intPrice, sysid} 升序 => 高价优先, 同价时间优先
            // askList_ 按 { intPrice, sysid} 升序 => 低价优先, 同价时间优先
            int64_t num = 0;
            auto bIt = bidList_.begin();
            auto aIt = askList_.begin();
            int64_t bRem = (bIt != bidList_.end()) ? bIt->second->volume : 0;
            int64_t aRem = (aIt != askList_.end()) ? aIt->second->volume : 0;
            rem = bestExec;
            while (rem > 0 && bIt != bidList_.end() && aIt != askList_.end()) {
                int64_t take = std::min(std::min(bRem, aRem), rem);
                if (take <= 0) break;
                ++num;
                bRem -= take; aRem -= take; rem -= take;
                if (bRem == 0) { ++bIt; bRem = (bIt != bidList_.end()) ? bIt->second->volume : 0; }
                if (aRem == 0) { ++aIt; aRem = (aIt != askList_.end()) ? aIt->second->volume : 0; }
            }
            record.totalNum = num;
        }
    }

    // --- 全深度聚合 (撮合前真实挂单, 价位级求和/订单级计数) ---
    {
        int64_t bidVol = 0; double bidPV = 0.0;
        for (const auto& kv : bestBidList_) {
            bidVol += kv.second;
            bidPV += (static_cast<double>(-kv.first) / 100.0) * static_cast<double>(kv.second);
        }
        int64_t askVol = 0; double askPV = 0.0;
        for (const auto& kv : bestAskList_) {
            askVol += kv.second;
            askPV += (static_cast<double>(kv.first) / 100.0) * static_cast<double>(kv.second);
        }
        record.totalBidVol = bidVol;
        record.totalAskVol = askVol;
        record.totalBidNum = static_cast<int64_t>(bidList_.size());
        record.totalAskNum = static_cast<int64_t>(askList_.size());
        if (bidVol > 0) record.bidVwapFull = bidPV / static_cast<double>(bidVol);
        if (askVol > 0) record.askVwapFull = askPV / static_cast<double>(askVol);
    }

    // --- 大单(委托额 >= BIG_ORDER_AMOUNT)总量 (订单级) ---
    {
        int64_t bigBid = 0, bigAsk = 0;
        for (const auto& kv : bidList_) {
            const Order* o = kv.second;
            if (o->price * static_cast<double>(o->volume) >= BIG_ORDER_AMOUNT) bigBid += o->volume;
        }
        for (const auto& kv : askList_) {
            const Order* o = kv.second;
            if (o->price * static_cast<double>(o->volume) >= BIG_ORDER_AMOUNT) bigAsk += o->volume;
        }
        record.bigBidVol = bigBid;
        record.bigAskVol = bigAsk;
    }

    // --- 集合竞价内累计撤单 (build 循环维护) ---
    record.cancelVolCum = auctionCancelVol_;
    record.cancelNumCum = auctionCancelNum_;

    // --- 残余盘口前 10 档 ---
    int lvl = 0;
    for (const auto& kv : resBid) {
        if (lvl >= LOB_LEVELS) break;
        record.bdp[lvl] = (-kv.first) / 100.0;
        record.bdv[lvl] = kv.second;
        ++lvl;
    }
    lvl = 0;
    for (const auto& kv : resAsk) {
        if (lvl >= LOB_LEVELS) break;
        record.akp[lvl] = kv.first / 100.0;
        record.akv[lvl] = kv.second;
        ++lvl;
    }
}

// 输出集合竞价 action 表为紧凑列主序二进制 shard
//
// 格式 (little-endian, 与 lob_data_save_3s 一致):
//   Header:
//     [0..8)   magic "ACTBIN01"
//     [8..12)  uint32 num_cols
//     [12..16) uint32 num_rows
//     for each col: uint16 name_len, name bytes, uint8 type_code (2=int64,3=float64)
//   Body: 按列顺序, 逐列 num_rows * 8 字节原始数据
//
// 列: instrumentID, dateTime, eventSeq, close, totalVolume, totalAmt,
//      bdp1,bdv1,...,bdp10,bdv10, akp1,akv1,...,akp10,akv10, totalNum
// (Python merge 端会统一 cast 成 float64 并派生 tradedate)
void TLLobBuilder::writeActionBin(const std::string& filename) const {
    std::ofstream f(filename, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "无法创建 action bin 文件: " << filename << std::endl;
        return;
    }

    int64_t instrumentID = 0;
    try { instrumentID = std::stoll(symbol_); } catch (...) { instrumentID = 0; }

    // 列定义 (name, type_code): 2=int64, 3=float64
    struct ColDef { std::string name; uint8_t type; };
    std::vector<ColDef> cols;
    cols.push_back({"instrumentID", 2});
    cols.push_back({"dateTime", 2});
    cols.push_back({"eventSeq", 2});
    cols.push_back({"close", 3});
    cols.push_back({"totalVolume", 2});
    cols.push_back({"totalAmt", 3});
    for (int i = 1; i <= LOB_LEVELS; ++i) {
        cols.push_back({"bdp" + std::to_string(i), 3});
        cols.push_back({"bdv" + std::to_string(i), 2});
    }
    for (int i = 1; i <= LOB_LEVELS; ++i) {
        cols.push_back({"akp" + std::to_string(i), 3});
        cols.push_back({"akv" + std::to_string(i), 2});
    }
    cols.push_back({"totalNum", 2});
    // 扩充因子列 (Tier 1 + Tier 2)
    cols.push_back({"imbalanceVol", 3});
    cols.push_back({"imbalanceRatio", 3});
    cols.push_back({"totalBidVol", 2});
    cols.push_back({"totalAskVol", 2});
    cols.push_back({"totalBidNum", 2});
    cols.push_back({"totalAskNum", 2});
    cols.push_back({"cancelVolCum", 2});
    cols.push_back({"cancelNumCum", 2});
    cols.push_back({"bidVwapFull", 3});
    cols.push_back({"askVwapFull", 3});
    cols.push_back({"bigBidVol", 2});
    cols.push_back({"bigAskVol", 2});

    // Header
    const char magic[8] = {'A', 'C', 'T', 'B', 'I', 'N', '0', '1'};
    f.write(magic, 8);
    uint32_t numCols = static_cast<uint32_t>(cols.size());
    uint32_t numRows = static_cast<uint32_t>(actionTable_.size());
    f.write(reinterpret_cast<const char*>(&numCols), 4);
    f.write(reinterpret_cast<const char*>(&numRows), 4);
    for (const auto& c : cols) {
        uint16_t nlen = static_cast<uint16_t>(c.name.size());
        f.write(reinterpret_cast<const char*>(&nlen), 2);
        f.write(c.name.data(), nlen);
        f.write(reinterpret_cast<const char*>(&c.type), 1);
    }

    // Body: 列主序. 每列遍历所有行写 8 字节.
    auto writeI64 = [&](auto getter) {
        for (const auto& r : actionTable_) {
            int64_t v = getter(r);
            f.write(reinterpret_cast<const char*>(&v), 8);
        }
    };
    auto writeF64 = [&](auto getter) {
        for (const auto& r : actionTable_) {
            double v = getter(r);
            f.write(reinterpret_cast<const char*>(&v), 8);
        }
    };

    writeI64([&](const ActionRecord&)   { return instrumentID; });
    writeI64([ ](const ActionRecord& r) { return r.dateTime; });
    writeI64([ ](const ActionRecord& r) { return r.eventSeq; });
    writeF64([ ](const ActionRecord& r) { return r.close; });
    writeI64([ ](const ActionRecord& r) { return r.totalVolume; });
    writeF64([ ](const ActionRecord& r) { return r.totalAmt; });
    for (int i = 0; i < LOB_LEVELS; ++i) {
        writeF64([i](const ActionRecord& r) { return r.bdp[i]; });
        writeI64([i](const ActionRecord& r) { return r.bdv[i]; });
    }
    for (int i = 0; i < LOB_LEVELS; ++i) {
        writeF64([i](const ActionRecord& r) { return r.akp[i]; });
        writeI64([i](const ActionRecord& r) { return r.akv[i]; });
    }
    writeI64([ ](const ActionRecord& r) { return r.totalNum; });
    // 扩充因子列 (顺序须与上面 cols 一致)
    writeF64([ ](const ActionRecord& r) { return r.imbalanceVol; });
    writeF64([ ](const ActionRecord& r) { return r.imbalanceRatio; });
    writeI64([ ](const ActionRecord& r) { return r.totalBidVol; });
    writeI64([ ](const ActionRecord& r) { return r.totalAskVol; });
    writeI64([ ](const ActionRecord& r) { return r.totalBidNum; });
    writeI64([ ](const ActionRecord& r) { return r.totalAskNum; });
    writeI64([ ](const ActionRecord& r) { return r.cancelVolCum; });
    writeI64([ ](const ActionRecord& r) { return r.cancelNumCum; });
    writeF64([ ](const ActionRecord& r) { return r.bidVwapFull; });
    writeF64([ ](const ActionRecord& r) { return r.askVwapFull; });
    writeI64([ ](const ActionRecord& r) { return r.bigBidVol; });
    writeI64([ ](const ActionRecord& r) { return r.bigAskVol; });

    f.close();
}

std::pair<int, int> TLLobBuilder::calcPosition(double price, char direction) const {
    // 与 flow_lob_builder 一致的 calcPosition 实现
    int bidPosition = 0;
    int askPosition = 0;
    int64_t intPrice = static_cast<int64_t>((price + 1e-6) * 100);
    
    if (direction == 'B') {
        // 买单: 看卖盘
        if (!bestAskList_.empty()) {
            int64_t bestAskIntPrice = bestAskList_.begin()->first;
            if (intPrice >= bestAskIntPrice) {
                // 超过卖一价, 可能直接成交
                askPosition = 0;
            } else {
                // 在买盘排队 - 使用 lower_bound 高效计算档位 O(log n)
                // bestBidList_ 使用 -intPrice 作为 key，所以找 -intPrice 的 lower_bound
                auto lb = bestBidList_.lower_bound(-intPrice);
                bidPosition = static_cast<int>(std::distance(bestBidList_.begin(), lb)) + 1;
            }
        } else {
            // 没有卖盘，直接计算在买盘的位置
            auto lb = bestBidList_.lower_bound(-intPrice);
            bidPosition = static_cast<int>(std::distance(bestBidList_.begin(), lb)) + 1;
        }
    } else {
        // 卖单: 看买盘
        if (!bestBidList_.empty()) {
            int64_t bestBidIntPrice = -bestBidList_.begin()->first;
            if (intPrice <= bestBidIntPrice) {
                // 低于买一价, 可能直接成交
                bidPosition = 0;
            } else {
                // 在卖盘排队 - 使用 lower_bound 高效计算档位 O(log n)
                auto lb = bestAskList_.lower_bound(intPrice);
                askPosition = static_cast<int>(std::distance(bestAskList_.begin(), lb)) + 1;
            }
        } else {
            // 没有买盘，直接计算在卖盘的位置
            auto lb = bestAskList_.lower_bound(intPrice);
            askPosition = static_cast<int>(std::distance(bestAskList_.begin(), lb)) + 1;
        }
    }
    
    return {bidPosition, askPosition};
}

int TLLobBuilder::calcType(double price, int64_t volume, char direction) const {
    // 与 flow_lob_builder 一致的 calcType 实现
    bool bestBidExists = !bestBidList_.empty();
    bool bestAskExists = !bestAskList_.empty();
    
    if (!bestBidExists && !bestAskExists) {
        return 0;
    }
    
    int64_t intPrice = static_cast<int64_t>((price + 1e-6) * 100);
    int64_t bestBidIntPrice = bestBidExists ? -bestBidList_.begin()->first : 0;
    int64_t bestBidVolume = bestBidExists ? bestBidList_.begin()->second : 0;
    int64_t bestAskIntPrice = bestAskExists ? bestAskList_.begin()->first : INT64_MAX;
    int64_t bestAskVolume = bestAskExists ? bestAskList_.begin()->second : 0;
    
    if (direction == 'B') {
        // 买单：对手盘是卖盘
        if (bestAskExists) {
            if (intPrice > bestAskIntPrice && volume >= bestAskVolume) {
                return 1;  // 激进买入
            } else if (intPrice == bestAskIntPrice && volume >= bestAskVolume) {
                return 2;  // 吃掉卖一
            } else if (intPrice >= bestAskIntPrice && volume < bestAskVolume) {
                return 3;  // 部分吃单
            }
        }
        // 判断在己方盘口的位置
        if (bestBidExists) {
            if (bestAskExists && intPrice < bestAskIntPrice && intPrice > bestBidIntPrice) {
                return 4;  // 介于买一卖一之间
            } else if (intPrice == bestBidIntPrice) {
                return 5;  // 等于买一
            } else if (intPrice < bestBidIntPrice) {
                return 6;  // 低于买一
            }
        } else if (!bestAskExists) {
            return 5;
        }
    } else {  // 'S'
        // 卖单：对手盘是买盘
        if (bestBidExists) {
            if (intPrice < bestBidIntPrice && volume >= bestBidVolume) {
                return 1;  // 激进卖出
            } else if (intPrice == bestBidIntPrice && volume >= bestBidVolume) {
                return 2;  // 吃掉买一
            } else if (intPrice <= bestBidIntPrice && volume < bestBidVolume) {
                return 3;  // 部分吃单
            }
        }
        // 判断在己方盘口的位置
        if (bestAskExists) {
            if (bestBidExists && intPrice > bestBidIntPrice && intPrice < bestAskIntPrice) {
                return 4;  // 介于买一卖一之间
            } else if (intPrice == bestAskIntPrice) {
                return 5;  // 等于卖一
            } else if (intPrice > bestAskIntPrice) {
                return 6;  // 高于卖一
            }
        } else if (!bestBidExists) {
            return 5;
        }
    }
    
    return 0;
}

void TLLobBuilder::build() {
    if (records_.empty()) {
        std::cerr << "没有数据记录" << std::endl;
        return;
    }
    
    std::cout << "开始构建LOB..." << std::endl;
    
    bool continuousStarted = false;
    bool wasOpenAuction = false;
    bool wasCloseAuction = false;
    double lastTimeSeconds = 0.0;
    
    for (size_t i = 0; i < records_.size(); ++i) {
        const auto& rec = records_[i];

        bool inOpenAuction = OPEN_AUCTION_START <= rec.timeSeconds && rec.timeSeconds < OPEN_AUCTION_END;
        bool inCloseAuction = CLOSE_AUCTION_START <= rec.timeSeconds && rec.timeSeconds < CLOSE_AUCTION_END;
        if ((inOpenAuction && !wasOpenAuction) || (inCloseAuction && !wasCloseAuction)) {
            auctionCancelVol_ = 0;
            auctionCancelNum_ = 0;
        }
        wasOpenAuction = inOpenAuction;
        wasCloseAuction = inCloseAuction;
        
        // 检查是否进入连续竞价
        if (!continuousStarted && isContinuousTradingTime(rec.timeSeconds)) {
            continuousStarted = true;
            checkWaitQueueReturn();
            if (verbose_) {
                std::cout << "进入连续竞价 @ " << rec.time << std::endl;
            }
        }
        
        LobRecord lobRec;
        lobRec.time = rec.time;
        lobRec.timeDiff = rec.timeSeconds - lastTimeSeconds;
        lastTimeSeconds = rec.timeSeconds;

        // 盘口快照取「本笔事件发生前」的状态 (与 tl/flow 及 datacheck_final 一致):
        // 在 handleOrder/handleCancel/handleTrade 改簿前先快照, 否则 bdv/akv 等量列会
        // 相对正确口径整体错位一行 (每行看到的是自己事件生效后的状态而非生效前)。
        getCurrentLob(lobRec);

        if (rec.actionType == 0) {
            // 委托
            // SH 立即成交主动单的 A_qty 只记挂住部分, 可能小于实际下单量: 展示量取
            // max(A_qty, 成交量+撤单量之和), 让委托量守恒(SH委托量补全, 移植自 datacheck_final)。
            int64_t stateVol = rec.volume;
            { auto fIt = orderFilled_.find(rec.sysid);
              if (fIt != orderFilled_.end()) stateVol = std::max(rec.volume, fIt->second); }
            // 入簿量: "先吃后挂"单(resequence 提前过 seqNo) A_qty 本身就是挂住量,
            // handleTrade 会跳过其 A 前成交, 必须用原始 A_qty 入簿(用补全量会把先吃部分
            // 留成幻影挂单); 非 resequence 单 orderFilled_ <= 原始量, stateVol == rec.volume,
            // 两者等价(Fix5)。
            int64_t bookVol = shOrderOrigSeq_.count(rec.sysid) ? rec.volume : stateVol;

            // SZ 特殊单价格按盘口解析(源 price 是涨停/地板占位价, 非真实价, 移植自
            // datacheck_final): 'U' 本方最优 -> best-self(买→买一, 卖→卖一);
            // '1' 市价 -> 对手盘最优(买→卖一, 卖→买一)。SH 无该字段(priceType 恒为 '2'),
            // 此分支不触发。
            double wtPrice = rec.price;
            if (market_ == Market::SZ && rec.priceType == 'U') {
                if (rec.direction == 'B' && !bestBidList_.empty())      wtPrice = -bestBidList_.begin()->first / 100.0;
                else if (rec.direction == 'S' && !bestAskList_.empty()) wtPrice = bestAskList_.begin()->first / 100.0;
            } else if (market_ == Market::SZ && rec.priceType == '1') {
                if (rec.direction == 'B' && !bestAskList_.empty()) wtPrice = bestAskList_.begin()->first / 100.0;
                else if (rec.direction == 'S' && !bestBidList_.empty()) wtPrice = -bestBidList_.begin()->first / 100.0;
            }

            // 分类/位置用这笔新订单插入前的盘口状态算 (与 datacheck_final 一致, 在 handleOrder
            // 改簿前算), 再入簿
            auto [bidPos, askPos] = calcPosition(wtPrice, rec.direction);
            int ordType = calcType(wtPrice, stateVol, rec.direction);

            handleOrder(rec.sysid, rec.time, wtPrice, bookVol, rec.direction, rec.timeSeconds,
                        rec.priceType);

            lobRec.ordertype = "wt";
            lobRec.sysid = rec.sysid;
            lobRec.direction = rec.direction;
            lobRec.price = wtPrice;
            lobRec.volume = stateVol;
            lobRec.tradeVolume = cumulativeVolume_;  // 累计成交量
            lobRec.bidPosition = bidPos;
            lobRec.askPosition = askPos;
            lobRec.type = ordType;

        } else if (rec.actionType == 1) {
            // 撤单
            double cancelPrice;
            int64_t cancelVolume;
            char cancelDir;
            
            if (handleCancel(rec.sysid, rec.time, rec.volume, cancelPrice, cancelVolume, cancelDir)) {
                lobRec.ordertype = "cl";
                lobRec.sysid = rec.sysid;
                lobRec.direction = cancelDir;
                lobRec.price = cancelPrice;
                lobRec.volume = cancelVolume;
                lobRec.tradeVolume = cumulativeVolume_;  // 累计成交量
                
                auto [bidPos, askPos] = calcPosition(cancelPrice, cancelDir);
                lobRec.bidPosition = bidPos;
                lobRec.askPosition = askPos;

                // 集合竞价时段累计撤单 (开盘/尾盘分段累计，供 cancelVolCum/cancelNumCum)
                if (isAuctionTradingTime(rec.timeSeconds)) {
                    auctionCancelVol_ += cancelVolume;
                    auctionCancelNum_ += 1;
                }
            } else {
                continue;  // 找不到订单，跳过
            }

        } else if (rec.actionType == 2) {
            // 成交
            // 成交 position 用「成交前」盘口 + 主动方向 (与 datacheck_final 一致), 在
            // handleTrade 改簿前算; 主动方向判定逻辑需与 handleTrade 内部一致。
            char preActiveDir;
            if (rec.tradeDirection == 1 || rec.tradeDirection == 2) {
                preActiveDir = (rec.tradeDirection == 1) ? 'B' : 'S';
            } else {
                preActiveDir = (rec.buyId > rec.sellId) ? 'B' : 'S';
            }
            auto [bidPos, askPos] = calcPosition(rec.price, preActiveDir);

            // 与 flow_lob_builder 一致：先检查并创建虚拟订单
            if (market_ == Market::SH) {
                // 检查买方订单是否需要虚拟订单
                if (allOrders_.find(rec.buyId) == allOrders_.end()) {
                    LobRecord derivedRec;
                    if (createDerivedOrder(rec.buyId, rec.time, derivedRec)) {
                        orderTable_.push_back(derivedRec);
                    }
                }
                // 检查卖方订单是否需要虚拟订单
                if (allOrders_.find(rec.sellId) == allOrders_.end()) {
                    LobRecord derivedRec;
                    if (createDerivedOrder(rec.sellId, rec.time, derivedRec)) {
                        orderTable_.push_back(derivedRec);
                    }
                }
            }

            char activeDir;
            int64_t activeSysid, passiveSysid;

            handleTrade(rec.buyId, rec.sellId, rec.price, rec.volume, rec.time,
                        rec.tradeDirection, activeDir, activeSysid, passiveSysid, rec.seqNo);

            lobRec.ordertype = "zb";
            lobRec.sysid = activeSysid;
            lobRec.direction = activeDir;
            lobRec.price = rec.price;
            lobRec.volume = rec.volume;  // 成交记录存储成交量
            lobRec.tradeVolume = cumulativeVolume_;  // 累计成交量
            lobRec.type = -1;  // 成交类型
            lobRec.bidPosition = bidPos;
            lobRec.askPosition = askPos;
            
            // 添加 TradeRecord
            TradeRecord tradeRec;
            tradeRec.sysid = activeSysid;
            tradeRec.wtVolume = rec.volume;
            tradeRec.wtSec = rec.timeSeconds;
            tradeRec.transSt = rec.timeSeconds;
            tradeRec.transSs = rec.timeSeconds;
            tradeRec.act = (activeDir == 'B') ? 1 : 0;
            tradeRec.transEt = rec.timeSeconds;
            tradeRec.transEs = rec.timeSeconds;
            tradeRec.transVlm = rec.volume;
            tradeRec.transAmt = rec.price * rec.volume;
            tradeRec.vwap = rec.price;
            tradeRec.actVlm = (activeDir == 'B') ? rec.volume : 0;
            tradeRec.actAmt = (activeDir == 'B') ? rec.price * rec.volume : 0;
            tradeRec.actNum = (activeDir == 'B') ? 1 : 0;
            tradeRec.pasVlm = (activeDir == 'B') ? 0 : rec.volume;
            tradeRec.pasAmt = (activeDir == 'B') ? 0 : rec.price * rec.volume;
            tradeRec.pasNum = (activeDir == 'B') ? 0 : 1;
            tradeTable_.push_back(tradeRec);
        }

        // 计算距离和比例 (与Flow格式一致)
        // bid_distance = 买一价 - 委托价
        // ask_distance = 委托价 - 卖一价  
        // bid_ratio = 委托量 / 买一量
        // ask_ratio = 委托量 / 卖一量
        if (lobRec.bdp[0] > 0) {
            lobRec.bidDistance = lobRec.bdp[0] - lobRec.price;
            if (lobRec.bdv[0] > 0 && lobRec.volume > 0) {
                lobRec.bidRatio = static_cast<double>(lobRec.volume) / lobRec.bdv[0];
            }
        }
        if (lobRec.akp[0] > 0) {
            lobRec.askDistance = lobRec.price - lobRec.akp[0];
            if (lobRec.akv[0] > 0 && lobRec.volume > 0) {
                lobRec.askRatio = static_cast<double>(lobRec.volume) / lobRec.akv[0];
            }
        }
        
        orderTable_.push_back(lobRec);

        // === 集合竞价 action 快照 ===
        // 开盘 [09:15,09:25) 与收盘 [14:57,15:00) 每个事件后对全深度订单簿做一次
        // 虚拟撮合，产出 action 记录；撮合只读副本，不改动真实订单簿。
        if (isAuctionTradingTime(rec.timeSeconds)) {
            ActionRecord actRec;
            actRec.eventSeq = static_cast<int64_t>(i) + 1;
            computeAuctionSnapshot(rec.time, actRec);
            actionTable_.push_back(actRec);
        }

        if (verbose_ && (i + 1) % 1000000 == 0) {
            std::cout << "处理进度: " << (i + 1) << "/" << records_.size() << std::endl;
        }
    }
    
    std::cout << "LOB构建完成，order记录: " << orderTable_.size()
              << " 条, trade记录: " << tradeTable_.size() << " 条" << std::endl;
}

void TLLobBuilder::writeOrderTableCSV(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "无法创建输出文件: " << filename << std::endl;
        return;
    }
    
    // 写入表头
    file << "time,sysid,ordertype,direction,price,volume,tradeVolume,"
         << "bidPosition,askPosition,bidDistance,askDistance,bidRatio,askRatio,"
         << "timeDiff,type,bidLevels,askLevels";
    for (int i = 0; i < LOB_LEVELS; ++i) {
        file << ",bdp" << (i+1) << ",bdv" << (i+1);
    }
    for (int i = 0; i < LOB_LEVELS; ++i) {
        file << ",akp" << (i+1) << ",akv" << (i+1);
    }
    file << "\n";
    
    // 写入数据
    for (const auto& rec : orderTable_) {
        file << rec.time << "," << rec.sysid << "," << rec.ordertype << ","
             << rec.direction << "," << rec.price << "," << rec.volume << ","
             << rec.tradeVolume << "," << rec.bidPosition << "," << rec.askPosition << ","
             << rec.bidDistance << "," << rec.askDistance << ","
             << rec.bidRatio << "," << rec.askRatio << ","
             << rec.timeDiff << "," << rec.type << ","
             << rec.bidLevels << "," << rec.askLevels;
        
        for (int i = 0; i < LOB_LEVELS; ++i) {
            file << "," << rec.bdp[i] << "," << rec.bdv[i];
        }
        for (int i = 0; i < LOB_LEVELS; ++i) {
            file << "," << rec.akp[i] << "," << rec.akv[i];
        }
        file << "\n";
    }
    
    std::cout << "写入CSV完成: " << filename << std::endl;
}

void TLLobBuilder::writeTradeTableCSV(const std::string& filename) const {
    std::ofstream ofs(filename);
    if (!ofs) {
        std::cerr << "无法创建输出文件: " << filename << std::endl;
        return;
    }
    
    // 写入表头 (与 flow_lob_builder 一致)
    ofs << "sysid,wt_volume,wt_time,wt_sec,trans_st,trans_ss,act,"
        << "trans_et,trans_es,cl_time,cl_sec,cl_vlm,trans_vlm,trans_amt,vwap,"
        << "act_vlm,act_amt,act_num,act_pnm,pas_vlm,pas_amt,pas_num,pas_pnm,"
        << "wt_ts,ts_te,wt_cl\n";
    
    ofs << std::fixed << std::setprecision(3);
    for (const auto& rec : tradeTable_) {
        ofs << rec.sysid << "," << rec.wtVolume << "," << rec.wtTime << ","
            << rec.wtSec << "," << rec.transSt << "," << rec.transSs << ","
            << rec.act << "," << rec.transEt << "," << rec.transEs << ","
            << rec.clTime << "," << rec.clSec << "," << rec.clVlm << ","
            << rec.transVlm << "," << rec.transAmt << "," << rec.vwap << ","
            << rec.actVlm << "," << rec.actAmt << "," << rec.actNum << ","
            << rec.actPnm << "," << rec.pasVlm << "," << rec.pasAmt << ","
            << rec.pasNum << "," << rec.pasPnm << ","
            << rec.wtTs << "," << rec.tsTe << "," << rec.wtCl << "\n";
    }
    
    std::cout << "写入Trade CSV完成: " << filename << std::endl;
}

} // namespace TLLob

// ============================================================
// HDF5 输出 (与 Flow LOB Builder 兼容)
// ============================================================

#ifdef HAS_HDF5

namespace TLLob {

// HDF5 字符串类型辅助结构 (与 flow_lob_builder 一致)
struct H5OrderRow {
    char time[16];       // HH:MM:SS.mmm
    int64_t sysid;
    char ordertype[4];   // wt/zb/cl
    char direction[2];   // B/S
    double price;
    int64_t volume;
    int64_t tradevolume;
    int64_t bid_position;
    int64_t ask_position;
    double bid_distance;
    double ask_distance;
    double bid_ratio;
    double ask_ratio;
    double time_diff;
    int64_t type;
    int64_t bid_levels;
    int64_t ask_levels;
    // 10档盘口
    double bdp1, bdp2, bdp3, bdp4, bdp5, bdp6, bdp7, bdp8, bdp9, bdp10;
    double bdv1, bdv2, bdv3, bdv4, bdv5, bdv6, bdv7, bdv8, bdv9, bdv10;
    double akp1, akp2, akp3, akp4, akp5, akp6, akp7, akp8, akp9, akp10;
    double akv1, akv2, akv3, akv4, akv5, akv6, akv7, akv8, akv9, akv10;
};

struct H5TradeRow {
    int64_t sysid;
    int64_t wt_volume;
    int64_t wt_time;
    double wt_sec;
    double trans_st;
    double trans_ss;
    int64_t act;
    double trans_et;
    double trans_es;
    double cl_time;
    double cl_sec;
    int64_t cl_vlm;
    int64_t trans_vlm;
    double trans_amt;
    double vwap;
    int64_t act_vlm;
    double act_amt;
    int64_t act_num;
    int64_t act_pnm;
    int64_t pas_vlm;
    double pas_amt;
    int64_t pas_num;
    int64_t pas_pnm;
    double wt_ts;
    double ts_te;
    double wt_cl;
};

bool TLLobBuilder::writeH5(const std::string& filename) const {
    try {
        H5::Exception::dontPrint();
        H5::H5File file(filename, H5F_ACC_TRUNC);
        
        // ---- 写入 order 表 ----
        {
            H5::CompType orderType(sizeof(H5OrderRow));
            H5::StrType strType16(H5::PredType::C_S1, 16);
            H5::StrType strType4(H5::PredType::C_S1, 4);
            H5::StrType strType2(H5::PredType::C_S1, 2);
            
            orderType.insertMember("time", HOFFSET(H5OrderRow, time), strType16);
            orderType.insertMember("sysid", HOFFSET(H5OrderRow, sysid), H5::PredType::NATIVE_INT64);
            orderType.insertMember("ordertype", HOFFSET(H5OrderRow, ordertype), strType4);
            orderType.insertMember("direction", HOFFSET(H5OrderRow, direction), strType2);
            orderType.insertMember("price", HOFFSET(H5OrderRow, price), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("volume", HOFFSET(H5OrderRow, volume), H5::PredType::NATIVE_INT64);
            orderType.insertMember("tradevolume", HOFFSET(H5OrderRow, tradevolume), H5::PredType::NATIVE_INT64);
            orderType.insertMember("bid_position", HOFFSET(H5OrderRow, bid_position), H5::PredType::NATIVE_INT64);
            orderType.insertMember("ask_position", HOFFSET(H5OrderRow, ask_position), H5::PredType::NATIVE_INT64);
            orderType.insertMember("bid_distance", HOFFSET(H5OrderRow, bid_distance), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("ask_distance", HOFFSET(H5OrderRow, ask_distance), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bid_ratio", HOFFSET(H5OrderRow, bid_ratio), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("ask_ratio", HOFFSET(H5OrderRow, ask_ratio), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("time_diff", HOFFSET(H5OrderRow, time_diff), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("type", HOFFSET(H5OrderRow, type), H5::PredType::NATIVE_INT64);
            orderType.insertMember("bid_levels", HOFFSET(H5OrderRow, bid_levels), H5::PredType::NATIVE_INT64);
            orderType.insertMember("ask_levels", HOFFSET(H5OrderRow, ask_levels), H5::PredType::NATIVE_INT64);
            
            // 10档盘口
            orderType.insertMember("bdp1", HOFFSET(H5OrderRow, bdp1), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp2", HOFFSET(H5OrderRow, bdp2), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp3", HOFFSET(H5OrderRow, bdp3), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp4", HOFFSET(H5OrderRow, bdp4), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp5", HOFFSET(H5OrderRow, bdp5), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp6", HOFFSET(H5OrderRow, bdp6), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp7", HOFFSET(H5OrderRow, bdp7), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp8", HOFFSET(H5OrderRow, bdp8), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp9", HOFFSET(H5OrderRow, bdp9), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdp10", HOFFSET(H5OrderRow, bdp10), H5::PredType::NATIVE_DOUBLE);
            
            orderType.insertMember("bdv1", HOFFSET(H5OrderRow, bdv1), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv2", HOFFSET(H5OrderRow, bdv2), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv3", HOFFSET(H5OrderRow, bdv3), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv4", HOFFSET(H5OrderRow, bdv4), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv5", HOFFSET(H5OrderRow, bdv5), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv6", HOFFSET(H5OrderRow, bdv6), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv7", HOFFSET(H5OrderRow, bdv7), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv8", HOFFSET(H5OrderRow, bdv8), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv9", HOFFSET(H5OrderRow, bdv9), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("bdv10", HOFFSET(H5OrderRow, bdv10), H5::PredType::NATIVE_DOUBLE);
            
            orderType.insertMember("akp1", HOFFSET(H5OrderRow, akp1), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp2", HOFFSET(H5OrderRow, akp2), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp3", HOFFSET(H5OrderRow, akp3), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp4", HOFFSET(H5OrderRow, akp4), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp5", HOFFSET(H5OrderRow, akp5), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp6", HOFFSET(H5OrderRow, akp6), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp7", HOFFSET(H5OrderRow, akp7), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp8", HOFFSET(H5OrderRow, akp8), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp9", HOFFSET(H5OrderRow, akp9), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akp10", HOFFSET(H5OrderRow, akp10), H5::PredType::NATIVE_DOUBLE);
            
            orderType.insertMember("akv1", HOFFSET(H5OrderRow, akv1), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv2", HOFFSET(H5OrderRow, akv2), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv3", HOFFSET(H5OrderRow, akv3), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv4", HOFFSET(H5OrderRow, akv4), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv5", HOFFSET(H5OrderRow, akv5), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv6", HOFFSET(H5OrderRow, akv6), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv7", HOFFSET(H5OrderRow, akv7), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv8", HOFFSET(H5OrderRow, akv8), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv9", HOFFSET(H5OrderRow, akv9), H5::PredType::NATIVE_DOUBLE);
            orderType.insertMember("akv10", HOFFSET(H5OrderRow, akv10), H5::PredType::NATIVE_DOUBLE);
            
            // 准备数据
            std::vector<H5OrderRow> orderRows(orderTable_.size());
            for (size_t i = 0; i < orderTable_.size(); ++i) {
                const auto& rec = orderTable_[i];
                H5OrderRow& row = orderRows[i];
                
                strncpy(row.time, rec.time.c_str(), 15);
                row.time[15] = '\0';
                row.sysid = rec.sysid;
                strncpy(row.ordertype, rec.ordertype.c_str(), 3);
                row.ordertype[3] = '\0';
                row.direction[0] = rec.direction;
                row.direction[1] = '\0';
                row.price = rec.price;
                row.volume = rec.volume;
                row.tradevolume = rec.tradeVolume;
                row.bid_position = rec.bidPosition;
                row.ask_position = rec.askPosition;
                row.bid_distance = rec.bidDistance;
                row.ask_distance = rec.askDistance;
                row.bid_ratio = rec.bidRatio;
                row.ask_ratio = rec.askRatio;
                row.time_diff = rec.timeDiff;
                row.type = rec.type;
                row.bid_levels = rec.bidLevels;
                row.ask_levels = rec.askLevels;
                
                // 10档盘口
                row.bdp1 = rec.bdp[0]; row.bdp2 = rec.bdp[1]; row.bdp3 = rec.bdp[2];
                row.bdp4 = rec.bdp[3]; row.bdp5 = rec.bdp[4]; row.bdp6 = rec.bdp[5];
                row.bdp7 = rec.bdp[6]; row.bdp8 = rec.bdp[7]; row.bdp9 = rec.bdp[8];
                row.bdp10 = rec.bdp[9];
                
                row.bdv1 = static_cast<double>(rec.bdv[0]); row.bdv2 = static_cast<double>(rec.bdv[1]);
                row.bdv3 = static_cast<double>(rec.bdv[2]); row.bdv4 = static_cast<double>(rec.bdv[3]);
                row.bdv5 = static_cast<double>(rec.bdv[4]); row.bdv6 = static_cast<double>(rec.bdv[5]);
                row.bdv7 = static_cast<double>(rec.bdv[6]); row.bdv8 = static_cast<double>(rec.bdv[7]);
                row.bdv9 = static_cast<double>(rec.bdv[8]); row.bdv10 = static_cast<double>(rec.bdv[9]);
                
                row.akp1 = rec.akp[0]; row.akp2 = rec.akp[1]; row.akp3 = rec.akp[2];
                row.akp4 = rec.akp[3]; row.akp5 = rec.akp[4]; row.akp6 = rec.akp[5];
                row.akp7 = rec.akp[6]; row.akp8 = rec.akp[7]; row.akp9 = rec.akp[8];
                row.akp10 = rec.akp[9];
                
                row.akv1 = static_cast<double>(rec.akv[0]); row.akv2 = static_cast<double>(rec.akv[1]);
                row.akv3 = static_cast<double>(rec.akv[2]); row.akv4 = static_cast<double>(rec.akv[3]);
                row.akv5 = static_cast<double>(rec.akv[4]); row.akv6 = static_cast<double>(rec.akv[5]);
                row.akv7 = static_cast<double>(rec.akv[6]); row.akv8 = static_cast<double>(rec.akv[7]);
                row.akv9 = static_cast<double>(rec.akv[8]); row.akv10 = static_cast<double>(rec.akv[9]);
            }
            
            // 创建数据空间和数据集
            hsize_t dims[1] = {static_cast<hsize_t>(orderRows.size())};
            H5::DataSpace dataspace(1, dims);
            
            // 启用压缩
            H5::DSetCreatPropList plist;
            hsize_t chunk_size[1] = {std::min<hsize_t>(orderRows.size(), 10000)};
            if (chunk_size[0] > 0) {
                plist.setChunk(1, chunk_size);
                plist.setDeflate(9);
            }
            
            H5::DataSet dataset = file.createDataSet("order", orderType, dataspace, plist);
            if (!orderRows.empty()) {
                dataset.write(orderRows.data(), orderType);
            }
        }
        
        // ---- 写入 trade_1500 表 ----
        {
            H5::CompType tradeType(sizeof(H5TradeRow));
            
            tradeType.insertMember("sysid", HOFFSET(H5TradeRow, sysid), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("wt_volume", HOFFSET(H5TradeRow, wt_volume), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("wt_time", HOFFSET(H5TradeRow, wt_time), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("wt_sec", HOFFSET(H5TradeRow, wt_sec), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("trans_st", HOFFSET(H5TradeRow, trans_st), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("trans_ss", HOFFSET(H5TradeRow, trans_ss), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("act", HOFFSET(H5TradeRow, act), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("trans_et", HOFFSET(H5TradeRow, trans_et), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("trans_es", HOFFSET(H5TradeRow, trans_es), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("cl_time", HOFFSET(H5TradeRow, cl_time), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("cl_sec", HOFFSET(H5TradeRow, cl_sec), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("cl_vlm", HOFFSET(H5TradeRow, cl_vlm), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("trans_vlm", HOFFSET(H5TradeRow, trans_vlm), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("trans_amt", HOFFSET(H5TradeRow, trans_amt), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("vwap", HOFFSET(H5TradeRow, vwap), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("act_vlm", HOFFSET(H5TradeRow, act_vlm), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("act_amt", HOFFSET(H5TradeRow, act_amt), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("act_num", HOFFSET(H5TradeRow, act_num), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("act_pnm", HOFFSET(H5TradeRow, act_pnm), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("pas_vlm", HOFFSET(H5TradeRow, pas_vlm), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("pas_amt", HOFFSET(H5TradeRow, pas_amt), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("pas_num", HOFFSET(H5TradeRow, pas_num), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("pas_pnm", HOFFSET(H5TradeRow, pas_pnm), H5::PredType::NATIVE_INT64);
            tradeType.insertMember("wt_ts", HOFFSET(H5TradeRow, wt_ts), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("ts_te", HOFFSET(H5TradeRow, ts_te), H5::PredType::NATIVE_DOUBLE);
            tradeType.insertMember("wt_cl", HOFFSET(H5TradeRow, wt_cl), H5::PredType::NATIVE_DOUBLE);
            
            // 准备数据
            std::vector<H5TradeRow> tradeRows(tradeTable_.size());
            for (size_t i = 0; i < tradeTable_.size(); ++i) {
                const auto& rec = tradeTable_[i];
                H5TradeRow& row = tradeRows[i];
                
                row.sysid = rec.sysid;
                row.wt_volume = rec.wtVolume;
                row.wt_time = rec.wtTime;
                row.wt_sec = rec.wtSec;
                row.trans_st = rec.transSt;
                row.trans_ss = rec.transSs;
                row.act = rec.act;
                row.trans_et = rec.transEt;
                row.trans_es = rec.transEs;
                row.cl_time = rec.clTime;
                row.cl_sec = rec.clSec;
                row.cl_vlm = rec.clVlm;
                row.trans_vlm = rec.transVlm;
                row.trans_amt = rec.transAmt;
                row.vwap = rec.vwap;
                row.act_vlm = rec.actVlm;
                row.act_amt = rec.actAmt;
                row.act_num = rec.actNum;
                row.act_pnm = rec.actPnm;
                row.pas_vlm = rec.pasVlm;
                row.pas_amt = rec.pasAmt;
                row.pas_num = rec.pasNum;
                row.pas_pnm = rec.pasPnm;
                row.wt_ts = rec.wtTs;
                row.ts_te = rec.tsTe;
                row.wt_cl = rec.wtCl;
            }
            
            // 创建数据空间和数据集
            hsize_t dims[1] = {static_cast<hsize_t>(tradeRows.size())};
            H5::DataSpace dataspace(1, dims);
            
            // 启用压缩
            H5::DSetCreatPropList plist;
            hsize_t chunk_size[1] = {std::min<hsize_t>(tradeRows.size(), 10000)};
            if (chunk_size[0] > 0) {
                plist.setChunk(1, chunk_size);
                plist.setDeflate(9);
            }
            
            H5::DataSet dataset = file.createDataSet("trade_1500", tradeType, dataspace, plist);
            if (!tradeRows.empty()) {
                dataset.write(tradeRows.data(), tradeType);
            }
        }
        
        file.close();
        if (verbose_) {
            std::cout << "写入H5文件: " << filename 
                      << " (order: " << orderTable_.size() 
                      << ", trade_1500: " << tradeTable_.size() << ")" << std::endl;
        }
        return true;
        
    } catch (const H5::Exception& e) {
        std::cerr << "HDF5 错误: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

} // namespace TLLob

#else

namespace TLLob {

bool TLLobBuilder::writeH5(const std::string& filename) const {
    std::cerr << "HDF5 支持未编译。请使用 -DWITH_HDF5=ON 重新编译" << std::endl;
    return false;
}

} // namespace TLLob

#endif  // HAS_HDF5
