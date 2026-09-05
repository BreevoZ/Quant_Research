/**
 * 通联历史数据 LOB Builder - 主程序
 * 
 * 与 Flow LOB Builder 保持一致的目录结构和配置
 * 输出到 /home/sharedriver1/public/Lob_new/<DATE>/ 
 * 
 * 用法:
 *   tl_lob_main -d 20251231                              # 使用默认路径
 *   tl_lob_main -d 20251231 -s 600638 -m sh              # 处理单只股票
 *   tl_lob_main -d 20251201 -e 20251231                  # 处理日期区间
 *   tl_lob_main -d 20251231 -f /path/to/TLData/ -o /path/to/output
 */

#include "auction_lob_builder.h"
#include <filesystem>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <set>
#include <ctime>
#include <sstream>
#include <iomanip>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace fs = std::filesystem;

using namespace TLLob;

// ============================================================
// 默认路径配置 (与 Flow LOB Builder 保持一致)
// ============================================================
const std::string DEFAULT_TL_BASE_PATH = "/home/TLData";
const std::string DEFAULT_OUTPUT_PATH = "/home/sharedriver1/public/Lob_new";

// ============================================================
// 辅助函数
// ============================================================

bool isHDF5Supported() {
#ifdef HAS_HDF5
    return true;
#else
    return false;
#endif
}

void printUsage(const char* progName) {
    std::cout << "用法: " << progName << " [选项]\n\n"
              << "选项:\n"
              << "  -d, --date DATE       交易日期 (YYYYMMDD, 必需)\n"
              << "  -e, --end-date DATE   结束日期 (YYYYMMDD, 处理日期区间)\n"
              << "  -f, --flow PATH       通联数据基础目录\n"
              << "                        (默认: " << DEFAULT_TL_BASE_PATH << ")\n"
              << "  -o, --output DIR      输出基础目录\n"
              << "                        (默认: " << DEFAULT_OUTPUT_PATH << ")\n"
              << "  -s, --symbol CODE     证券代码过滤 (如 600638)\n"
              << "  -m, --market MARKET   市场过滤 (SH 或 SZ)\n"
              << "  -w, --workers N       并行线程数 (默认: CPU核心数)\n"
              << "  -v, --verbose         详细输出\n"
              << "  -h, --help            显示帮助\n\n"
              << "输出格式:\n"
              << "  每只股票输出一个集合竞价 action 分片: {sh|sz}{symbol}_action.bin\n"
              << "  (紧凑列主序二进制; 内含开盘/收盘集合竞价逐事件虚拟撮合快照,\n"
              << "   供 Python 端 merge 成当日 parquet)\n"
              << "  注: 逐笔 LOB order/trade 表由 TLLobBuilder::writeH5 支持, 但主程序当前\n"
              << "      只输出 action 分片, 不写 order/trade H5。\n\n"
              << "示例:\n"
              << "  " << progName << " -d 20251231                      # 处理单日\n"
              << "  " << progName << " -d 20251201 -e 20251231          # 处理日期区间\n"
              << "  " << progName << " -d 20251231 -s 600638 -m SH      # 处理单只股票\n"
              << "  " << progName << " -d 20251231 -m SZ                # 只处理深圳市场\n"
              << "  " << progName << " -d 20251231 -f /path/to/TLData/ -o /path/to/output\n";
}

/**
 * 扫描通联数据目录，获取所有可用的证券代码
 */
std::set<std::string> scanSHSymbols(const std::string& filePath) {
    std::set<std::string> symbols;
    std::ifstream file(filePath);
    if (!file.is_open()) return symbols;
    
    std::string line;
    std::getline(file, line);  // 跳过表头
    
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        // SecurityID 是第3列 (index 2)
        size_t pos1 = 0, pos2 = 0;
        int col = 0;
        while (col < 2 && pos2 != std::string::npos) {
            pos1 = pos2 + (pos2 > 0 ? 1 : 0);
            pos2 = line.find(',', pos1);
            col++;
        }
        if (col == 2) {
            pos1 = pos2 + 1;
            pos2 = line.find(',', pos1);
            std::string securityID = line.substr(pos1, pos2 - pos1);
            if (securityID.length() == 6) {
                symbols.insert(securityID);
            }
        }
    }
    return symbols;
}

std::set<std::string> scanSZSymbols(const std::string& filePath) {
    std::set<std::string> symbols;
    std::ifstream file(filePath);
    if (!file.is_open()) return symbols;
    
    std::string line;
    std::getline(file, line);  // 跳过表头
    
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        // SecurityID 是第4列 (index 3)
        size_t pos1 = 0, pos2 = 0;
        int col = 0;
        while (col < 3 && pos2 != std::string::npos) {
            pos1 = pos2 + (pos2 > 0 ? 1 : 0);
            pos2 = line.find(',', pos1);
            col++;
        }
        if (col == 3) {
            pos1 = pos2 + 1;
            pos2 = line.find(',', pos1);
            std::string securityID = line.substr(pos1, pos2 - pos1);
            // 接受4-6位的数字代码 (如 "2009" 或 "000001")
            if (securityID.length() >= 4 && securityID.length() <= 6) {
                // 补齐到6位 (前面补0)
                while (securityID.length() < 6) {
                    securityID = "0" + securityID;
                }
                symbols.insert(securityID);
            }
        }
    }
    return symbols;
}

/**
 * 解析日期字符串 YYYYMMDD 为 tm 结构
 */
bool parseDate(const std::string& dateStr, std::tm& tm) {
    if (dateStr.length() != 8) return false;
    try {
        tm.tm_year = std::stoi(dateStr.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(dateStr.substr(4, 2)) - 1;
        tm.tm_mday = std::stoi(dateStr.substr(6, 2));
        tm.tm_hour = 12;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * 格式化日期为 YYYYMMDD
 */
std::string formatDate(const std::tm& tm) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900)
        << std::setw(2) << (tm.tm_mon + 1)
        << std::setw(2) << tm.tm_mday;
    return oss.str();
}

/**
 * 生成日期区间内的所有日期
 */
std::vector<std::string> generateDateRange(const std::string& startDate, const std::string& endDate) {
    std::vector<std::string> dates;
    
    std::tm startTm = {}, endTm = {};
    if (!parseDate(startDate, startTm) || !parseDate(endDate, endTm)) {
        return dates;
    }
    
    std::time_t startTime = std::mktime(&startTm);
    std::time_t endTime = std::mktime(&endTm);
    
    if (startTime > endTime) {
        return dates;
    }
    
    const int SECONDS_PER_DAY = 86400;
    
    for (std::time_t t = startTime; t <= endTime; t += SECONDS_PER_DAY) {
        std::tm* tm = std::localtime(&t);
        // 跳过周末 (0=周日, 6=周六)
        if (tm->tm_wday != 0 && tm->tm_wday != 6) {
            dates.push_back(formatDate(*tm));
        }
    }
    
    return dates;
}

int main(int argc, char* argv[]) {
    std::string startDate;
    std::string endDate;
    std::string flowBasePath;
    std::string outputBasePath;
    std::string filterSymbol;
    std::string filterMarket;
    bool verbose = false;
    bool outputCSV = false;
    
    // 默认使用 CPU 核心数的线程
#ifdef _OPENMP
    int nWorkers = omp_get_num_procs();
#else
    int nWorkers = std::thread::hardware_concurrency();
    if (nWorkers == 0) nWorkers = 4;  // fallback
#endif
    
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-d" || arg == "--date") {
            if (i + 1 < argc) startDate = argv[++i];
        } else if (arg == "-e" || arg == "--end-date") {
            if (i + 1 < argc) endDate = argv[++i];
        } else if (arg == "-f" || arg == "--flow") {
            if (i + 1 < argc) flowBasePath = argv[++i];
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) outputBasePath = argv[++i];
        } else if (arg == "-s" || arg == "--symbol") {
            if (i + 1 < argc) filterSymbol = argv[++i];
        } else if (arg == "-m" || arg == "--market") {
            if (i + 1 < argc) filterMarket = argv[++i];
        } else if (arg == "-w" || arg == "--workers") {
            if (i + 1 < argc) nWorkers = std::stoi(argv[++i]);
        } else if (arg == "--csv") {
            outputCSV = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }
    
    // 验证参数
    if (startDate.empty()) {
        std::cerr << "错误: --date 参数是必需的\n";
        printUsage(argv[0]);
        return 1;
    }
    
    // 如果没有指定结束日期，使用开始日期（单日模式）
    if (endDate.empty()) {
        endDate = startDate;
    }
    
    // 检查 HDF5 支持
    if (!outputCSV && !isHDF5Supported()) {
        std::cerr << "警告: HDF5 支持不可用，使用 CSV 输出。\n";
        std::cerr << "      重新编译时使用 -DWITH_HDF5=ON 启用 H5 格式。\n";
        outputCSV = true;
    }
    
    // 使用默认路径
    if (flowBasePath.empty()) {
        flowBasePath = DEFAULT_TL_BASE_PATH;
    }
    if (outputBasePath.empty()) {
        outputBasePath = DEFAULT_OUTPUT_PATH;
    }
    
    // 生成日期列表
    std::vector<std::string> datesToProcess = generateDateRange(startDate, endDate);
    
    if (datesToProcess.empty()) {
        std::cerr << "错误: 无效的日期范围 " << startDate << " - " << endDate << "\n";
        return 1;
    }
    
    std::cout << "========================================\n"
              << "通联数据 LOB Builder (C++)\n"
              << "========================================\n";
    if (datesToProcess.size() == 1) {
        std::cout << "交易日期: " << startDate << "\n";
    } else {
        std::cout << "日期区间: " << startDate << " - " << endDate << "\n"
                  << "工作日数: " << datesToProcess.size() << "\n";
    }
    std::cout << "数据目录: " << flowBasePath << "/<DATE>\n"
              << "输出目录: " << outputBasePath << "/<DATE>\n"
              << "输出格式: 集合竞价 action 分片 ({sh|sz}{symbol}_action.bin)\n";
    if (!filterSymbol.empty()) {
        std::cout << "证券代码: " << filterSymbol << "\n";
    }
    if (!filterMarket.empty()) {
        std::cout << "市场过滤: " << filterMarket << "\n";
    }
    std::cout << "并行线程: " << nWorkers << "\n"
              << "========================================\n\n";
    
    auto totalStartTime = std::chrono::high_resolution_clock::now();
    
    int totalSuccessCount = 0;
    int totalFailCount = 0;
    int datesProcessed = 0;
    int datesSkipped = 0;
    
    // 遍历每个日期
    for (const auto& tradeDate : datesToProcess) {
        std::cout << "\n########################################\n"
                  << "处理日期: " << tradeDate 
                  << " (" << (datesProcessed + datesSkipped + 1) << "/" << datesToProcess.size() << ")\n"
                  << "########################################\n";
        
        std::string flowPath = flowBasePath + "/" + tradeDate;
        std::string outputDir = outputBasePath + "/" + tradeDate;
        
        // 检查数据目录是否存在
        if (!fs::exists(flowPath)) {
            std::cout << "  跳过: 数据目录不存在 " << flowPath << "\n";
            datesSkipped++;
            continue;
        }
    
    // 创建输出目录
    fs::create_directories(outputDir);
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // 收集需要处理的股票
    std::vector<std::pair<std::string, Market>> symbolsToProcess;
    
    // 上海数据文件
    std::string shFile = flowPath + "/mdl_4_24_0.csv";
    // 深圳数据文件
    std::string szOrderFile = flowPath + "/mdl_6_33_0.csv";
    std::string szTradeFile = flowPath + "/mdl_6_36_0.csv";
    
    // 扫描上海股票
    if ((filterMarket.empty() || filterMarket == "SH" || filterMarket == "sh") &&
        fs::exists(shFile)) {
        std::cout << "[1/4] 扫描上海数据...\n";
        auto shSymbols = scanSHSymbols(shFile);
        for (const auto& sym : shSymbols) {
            if (filterSymbol.empty() || sym == filterSymbol) {
                symbolsToProcess.emplace_back(sym, Market::SH);
            }
        }
        std::cout << "  找到 " << shSymbols.size() << " 只上海股票\n";
    }
    
    // 扫描深圳股票
    if ((filterMarket.empty() || filterMarket == "SZ" || filterMarket == "sz") &&
        fs::exists(szOrderFile)) {
        std::cout << "[2/4] 扫描深圳数据...\n";
        auto szSymbols = scanSZSymbols(szOrderFile);
        for (const auto& sym : szSymbols) {
            if (filterSymbol.empty() || sym == filterSymbol) {
                symbolsToProcess.emplace_back(sym, Market::SZ);
            }
        }
        std::cout << "  找到 " << szSymbols.size() << " 只深圳股票\n";
    }
    
    if (symbolsToProcess.empty()) {
        std::cerr << "错误: 没有找到任何股票数据\n";
        return 1;
    }
    
    // 统计
    int shCount = 0, szCount = 0;
    for (const auto& [sym, mkt] : symbolsToProcess) {
        if (mkt == Market::SH) shCount++;
        else szCount++;
    }
    std::cout << "\n共需处理 " << symbolsToProcess.size() << " 只股票 "
              << "(SH: " << shCount << ", SZ: " << szCount << ")\n\n";
    
    // 处理每只股票
    std::cout << "[3/4] 构建LOB...\n";
    
    // 预加载数据到内存（避免多线程文件读取冲突）
    // 读取本身按 nWorkers 分块并行扫描(见 auction_lob_builder.cpp 的 computeFileChunks),
    // 大文件(SH mdl_4_24 ~20GB / SZ mdl_6_33+6_36 各~14GB)单线程顺序扫需要 10+ 分钟,
    // 分块并行后按核数近似线性加速。
    // 若用户用 -s 指定了单只股票(symbolsToProcess 必然只有这一条), 直接把过滤条件带进
    // 读取阶段: 逐行仍需读到 securityID 字段才能判断, I/O 无法省, 但能跳过该行剩余字段的
    // 解析和 push_back、并且大幅减少内存占用(不必为全市场几千万条记录常驻内存)。
    // 不指定 -s 时(要处理多只/全部股票)必须传空串, 一次性读全量供各股票分别按需筛选。
    std::cout << "  预加载上海数据到内存...\n";
    auto shRecords = (fs::exists(shFile) && (filterMarket.empty() || filterMarket == "SH" || filterMarket == "sh"))
                      ? readSHTickCSV(shFile, filterSymbol, nWorkers) : std::vector<SHTickRecord>();
    std::cout << "  预加载深圳委托数据到内存...\n";
    auto szOrderRecords = (fs::exists(szOrderFile) && (filterMarket.empty() || filterMarket == "SZ" || filterMarket == "sz"))
                          ? readSZOrderCSV(szOrderFile, filterSymbol, nWorkers) : std::vector<SZOrderRecord>();
    std::cout << "  预加载深圳成交数据到内存...\n";
    auto szTradeRecords = (fs::exists(szTradeFile) && (filterMarket.empty() || filterMarket == "SZ" || filterMarket == "sz"))
                          ? readSZTradeCSV(szTradeFile, filterSymbol, nWorkers) : std::vector<SZTradeRecord>();
    std::cout << "  数据预加载完成\n\n";
    
    std::atomic<int> successCount{0};
    std::atomic<int> failCount{0};
    std::atomic<int> processedCount{0};
    std::mutex printMutex;
    
    const size_t totalSymbols = symbolsToProcess.size();
    
    // 多线程并行处理
    if (nWorkers > 1 && totalSymbols > 1) {
        std::cout << "使用 " << nWorkers << " 个线程处理...\n";
        
#ifdef _OPENMP
        #pragma omp parallel for num_threads(nWorkers) schedule(dynamic)
#endif
        for (size_t idx = 0; idx < totalSymbols; ++idx) {
            const auto& [symbol, market] = symbolsToProcess[idx];
            int myCount = ++processedCount;
            
            try {
                // 输出文件名格式: sh600638 或 sz000001
                std::string prefix = (market == Market::SH) ? "sh" : "sz";
                std::string symbolName = prefix + symbol;
                
                {
                    std::lock_guard<std::mutex> lock(printMutex);
                    std::cout << "[" << myCount << "/" << totalSymbols << "] "
                              << "处理 " << symbolName << "...\n";
                }
                
                // 构建LOB
                TLLobBuilder builder(symbol, tradeDate, market);
                builder.setVerbose(false);  // 多线程模式禁用详细日志
                
                if (market == Market::SH) {
                    builder.loadSHDataFromMemory(shRecords);
                } else {
                    builder.loadSZOrderDataFromMemory(szOrderRecords);
                    builder.loadSZTradeDataFromMemory(szTradeRecords);
                }
                
                builder.build();

                // 输出集合竞价 action CSV (每股一个 shard, 供 Python merge 成当日 parquet)
                std::string actionFile = outputDir + "/" + symbolName + "_action.bin";
                builder.writeActionBin(actionFile);
                {
                    std::lock_guard<std::mutex> lock(printMutex);
                    std::cout << "  [" << symbolName << "] -> action ("
                              << builder.getActionTable().size() << " 行) 已保存\n";
                }

                ++successCount;
                
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(printMutex);
                std::cerr << "  [" << symbol << "] 错误: " << e.what() << "\n";
                ++failCount;
            }
        }
    } else {
        // 单线程处理
        for (const auto& [symbol, market] : symbolsToProcess) {
            int myCount = ++processedCount;
            
            try {
                std::string prefix = (market == Market::SH) ? "sh" : "sz";
                std::string symbolName = prefix + symbol;
                
                std::cout << "[" << myCount << "/" << totalSymbols << "] "
                          << "处理 " << symbolName << "...\n";
                
                TLLobBuilder builder(symbol, tradeDate, market);
                builder.setVerbose(verbose);
                
                if (market == Market::SH) {
                    builder.loadSHDataFromMemory(shRecords);
                } else {
                    builder.loadSZOrderDataFromMemory(szOrderRecords);
                    builder.loadSZTradeDataFromMemory(szTradeRecords);
                }
                
                builder.build();

                // 输出集合竞价 action CSV
                std::string actionFile = outputDir + "/" + symbolName + "_action.bin";
                builder.writeActionBin(actionFile);
                std::cout << "  -> action (" << builder.getActionTable().size()
                          << " 行) 已保存\n";

                ++successCount;
                
            } catch (const std::exception& e) {
                std::cerr << "  [" << symbol << "] 错误: " << e.what() << "\n";
                ++failCount;
            }
        }
    }
    
    // 日期统计
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime);
    
    std::cout << "\n[" << tradeDate << "] 完成: 成功 " << successCount.load() 
              << ", 失败 " << failCount.load() 
              << ", 耗时 " << duration.count() << "s\n";
    
    totalSuccessCount += successCount.load();
    totalFailCount += failCount.load();
    datesProcessed++;
    
    }  // 日期循环结束
    
    // 总体统计
    auto totalEndTime = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(totalEndTime - totalStartTime);
    
    std::cout << "\n========================================\n"
              << "全部处理完成\n"
              << "========================================\n";
    if (datesToProcess.size() > 1) {
        std::cout << "日期范围: " << startDate << " - " << endDate << "\n"
                  << "处理日期: " << datesProcessed << "\n"
                  << "跳过日期: " << datesSkipped << "\n";
    }
    std::cout << "总成功数: " << totalSuccessCount << "\n"
              << "总失败数: " << totalFailCount << "\n"
              << "总耗时:   " << totalDuration.count() << " 秒\n"
              << "输出目录: " << outputBasePath << "/<DATE>\n"
              << "========================================\n";
    
    return (totalFailCount == 0) ? 0 : 1;
}
