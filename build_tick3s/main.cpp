/**
 * LOB数据3秒降频处理 - 主程序
 * 
 * 用法:
 *   lob_processor                      # 处理当天数据
 *   lob_processor 20251225             # 处理指定日期
 *   lob_processor 20251201 20251225    # 处理日期范围
 */

#include "lob_data_processor.h"
#include <iostream>
#include <ctime>
#include <cstdio>
#include <iomanip>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;

void printUsage(const char* prog_name) {
    std::cout << "\nLOB数据3秒降频处理工具\n\n";
    std::cout << "用法:\n";
    std::cout << "  " << prog_name << " [选项]                     # 处理当天数据\n";
    std::cout << "  " << prog_name << " [选项] 20251225            # 处理指定日期\n";
    std::cout << "  " << prog_name << " [选项] 20251201 20251225   # 处理日期范围\n\n";
    std::cout << "选项:\n";
    std::cout << "  --market <sz|sh|all>     指定处理的市场 (默认: all)\n";
    std::cout << "                           sz  - 仅处理深圳市场\n";
    std::cout << "                           sh  - 仅处理上海市场\n";
    std::cout << "                           all - 处理所有市场\n";
    std::cout << "  -s, --symbol <code>      指定处理的股票代码 (如: 000001, 600000)\n";
    std::cout << "  -t, --table <name>       指定ClickHouse表名 (默认: Tick_3sec_data)\n";
    std::cout << "  --time-start <HH:MM:SS>  指定插入数据的起始时间 (如: 09:30:00)\n";
    std::cout << "  --time-end <HH:MM:SS>    指定插入数据的结束时间 (如: 15:00:00)\n";
    std::cout << "  --output <ch|parquet|both>  输出目标 (默认: ch)\n";
    std::cout << "                           ch      - 仅写 ClickHouse (默认, 兼容旧行为)\n";
    std::cout << "                           parquet - 仅写 parquet 分片 (跳过 CH)\n";
    std::cout << "                           both    - 同时写 CH 和 parquet 分片\n";
    std::cout << "  --parquet-out-dir <path>  Parquet 输出根目录 (默认: /tmp/lob_in_nolevel2_MX)\n";
    std::cout << "                           分片写入 {path}/.staging/{YYYYMMDD}/{symbol}.bin\n";
    std::cout << "                           需手动执行 merge_3s_parquet.py 合并为 {YYYYMMDD}_tick.parquet\n\n";
    std::cout << "参数:\n";
    std::cout << "  日期格式: YYYYMMDD 或 YYYY-MM-DD\n\n";
    std::cout << "示例:\n";
    std::cout << "  " << prog_name << "                          # 处理今天所有市场\n";
    std::cout << "  " << prog_name << " --market sz              # 处理今天深圳市场\n";
    std::cout << "  " << prog_name << " -s 000001                # 处理今天的 sz000001\n";
    std::cout << "  " << prog_name << " -t my_table 20251225     # 插入到自定义表名\n";
    std::cout << "  " << prog_name << " --time-start 09:30:00 --time-end 11:30:00 20251225  # 只插入上午时段\n";
    std::cout << "  " << prog_name << " --market sh -s 600000 20251225  # 处理2025-12-25的sh600000\n";
    std::cout << "  " << prog_name << " --market sz 20251201 20251225   # 处理12月1-25日深圳市场\n";
    std::cout << std::endl;
}

// 处理单个文件的Worker函数
// 辅助函数: 将 HH:MM:SS 格式转换为秒数
int parseTimeToSeconds(const std::string& time_str) {
    int h = 0, m = 0, s = 0;
    if (sscanf(time_str.c_str(), "%d:%d:%d", &h, &m, &s) >= 2) {
        return h * 3600 + m * 60 + s;
    }
    throw std::runtime_error("Invalid time format: " + time_str + " (expected HH:MM:SS)");
}

struct ProcessTask {
    std::string h5_path;
    std::string level2_path;
    std::time_t trade_date;
    bool save_to_ch;
    bool save_to_csv;
    ClickHouseConfig ch_config;
    std::string table_name;
    int time_start;
    int time_end;
    bool save_to_parquet_shard = false;
    std::string parquet_staging_dir;
};

struct ProcessTaskResult {
    std::string h5_path;
    bool success;
    std::string message;
    double elapsed_seconds;
};

ProcessTaskResult processSingleFile(const ProcessTask& task) {
    ProcessTaskResult result;
    result.h5_path = task.h5_path;
    
    fs::path p(task.h5_path);
    std::string stock_code = p.stem().string();
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        LobDataProcessor processor(task.ch_config);
        processor.processAndSave(
            task.h5_path,
            task.level2_path,
            task.trade_date,
            task.save_to_ch,
            task.save_to_csv,
            task.table_name,
            task.time_start,
            task.time_end,
            task.save_to_parquet_shard,
            task.parquet_staging_dir
        );
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        result.success = true;
        result.elapsed_seconds = duration.count() / 1000.0;
        result.message = "Success: " + std::to_string(result.elapsed_seconds) + "s";
        
        std::cout << "[" << stock_code << "] " << result.message << std::endl;
    }
    catch (const std::exception& e) {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        result.success = false;
        result.elapsed_seconds = duration.count() / 1000.0;
        result.message = "Error: " + std::string(e.what());
        
        std::cerr << "[" << stock_code << "] " << result.message << std::endl;
    }
    
    return result;
}

// 处理指定日期的所有文件
LobDataProcessor::ProcessResult processDate(
    const std::time_t& trade_date,
    const ClickHouseConfig& ch_config,
    const std::string& market = "all",
    const std::string& symbol = "",
    const std::string& h5_base_path = "/tmp/lob_in",
    const std::string& level2_base_path = "/tmp/lob_in_nolevel2",
    const std::string& csv_base_path = "/home/sharedriver1/public/Lob_new_3s",
    bool save_to_ch = true,
    bool save_to_csv = false,
    int max_workers = 20,
    const std::string& table_name = "Tick_3sec_data",
    int time_start = -1,
    int time_end = -1,
    bool save_to_parquet_shard = false,
    const std::string& parquet_staging_dir = "") {
    
    LobDataProcessor::ProcessResult final_result;
    
    // 构建目录路径
    std::string year_str = Utils::formatDate(trade_date, "%Y");
    std::string date_str = Utils::formatDate(trade_date, "%Y%m%d");
    
    std::string h5_dir = h5_base_path + "/" + date_str;
    std::string level2_dir = level2_base_path + "/" + year_str + "/" + date_str;
    
    std::cout << "========================================" << std::endl;
    std::cout << "Processing date: " << date_str << std::endl;
    std::cout << "Market filter: " << market << std::endl;
    if (!symbol.empty()) {
        std::cout << "Symbol filter: " << symbol << std::endl;
    }
    std::cout << "LOB H5 directory: " << h5_dir << std::endl;
    std::cout << "Level2 directory: " << level2_dir << std::endl;
    std::cout << "Max workers: " << max_workers << std::endl;
    std::cout << "Table name: " << table_name << std::endl;
    if (time_start >= 0 || time_end >= 0) {
        auto fmtTime = [](int sec) -> std::string {
            if (sec < 0) return "N/A";
            int h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
            char buf[16]; snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
            return buf;
        };
        std::cout << "Time filter: " << fmtTime(time_start) << " - " << fmtTime(time_end) << std::endl;
    }
    std::cout << "========================================" << std::endl;
    
    // 检查目录是否存在
    if (!fs::exists(h5_dir)) {
        std::cerr << "Error: Directory not found: " << h5_dir << std::endl;
        final_result.date = date_str;
        final_result.total = 0;
        final_result.success = 0;
        final_result.failed = 0;
        final_result.elapsed_seconds = 0.0;
        return final_result;
    }
    
    // 获取所有H5文件
    auto all_h5_files = Utils::getH5Files(h5_dir);
    
    // 根据市场和股票代码过滤文件
    std::vector<std::string> h5_files;
    if (market == "all" && symbol.empty()) {
        h5_files = all_h5_files;
    } else {
        for (const auto& h5_path : all_h5_files) {
            fs::path p(h5_path);
            std::string filename = p.stem().string();
            
            bool match = true;
            
            // 检查市场过滤
            if (market != "all" && filename.size() >= 2) {
                std::string prefix = filename.substr(0, 2);
                if (prefix != market) {
                    match = false;
                }
            }
            
            // 检查股票代码过滤
            if (match && !symbol.empty()) {
                // 文件名格式: sz000001.h5 或 sh600000.h5
                // 查找文件名中是否包含指定的代码
                if (filename.find(symbol) == std::string::npos) {
                    match = false;
                }
            }
            
            if (match) {
                h5_files.push_back(h5_path);
            }
        }
    }
    
    int total_files = h5_files.size();
    std::cout << "Found " << all_h5_files.size() << " H5 files total, ";
    std::cout << total_files << " files match filter";
    if (market != "all" || !symbol.empty()) {
        std::cout << " (market: '" << market << "'";
        if (!symbol.empty()) {
            std::cout << ", symbol: '" << symbol << "'";
        }
        std::cout << ")";
    }
    std::cout << std::endl;
    
    if (total_files == 0) {
        final_result.date = date_str;
        final_result.total = 0;
        final_result.success = 0;
        final_result.failed = 0;
        final_result.elapsed_seconds = 0.0;
        return final_result;
    }
    
    auto overall_start = std::chrono::high_resolution_clock::now();
    
    // 构建任务列表
    std::vector<ProcessTask> tasks;
    for (const auto& h5_path : h5_files) {
        fs::path p(h5_path);
        std::string stock_code = p.filename().string();
        std::string level2_path = level2_dir + "/" + stock_code;
        
        // 检查Level2文件是否存在。缺失时仍处理LOB H5，只是不做Level2价格修正。
        if (!fs::exists(level2_path)) {
            std::cerr << "Warning: Level2 file not found, continue without Level2: " << level2_path << std::endl;
            level2_path.clear();
        }
        
        // 构建CSV路径
        std::string csv_path;
        if (save_to_csv) {
            std::string csv_dir = csv_base_path + "/" + year_str + "/" + date_str;
            fs::create_directories(csv_dir);
            csv_path = csv_dir + "/" + p.stem().string() + ".csv";
        }
        
        ProcessTask task;
        task.h5_path = h5_path;
        task.level2_path = level2_path;
        task.trade_date = trade_date;
        task.save_to_ch = save_to_ch;
        task.save_to_csv = save_to_csv;
        task.ch_config = ch_config;
        task.table_name = table_name;
        task.time_start = time_start;
        task.time_end = time_end;
        task.save_to_parquet_shard = save_to_parquet_shard;
        task.parquet_staging_dir = parquet_staging_dir;

        tasks.push_back(task);
    }
    
    // 多线程处理
    std::atomic<int> success_count(0);
    std::atomic<int> failed_count(0);
    std::atomic<int> processed_count(0);
    int last_progress = 0;  // 用于进度打印
    
    std::vector<std::future<ProcessTaskResult>> futures;
    
    std::cout << "\nProcessing files with " << max_workers << " workers...\n" << std::endl;
    
    // 打印进度的辅助函数
    auto printProgress = [&]() {
        int current = processed_count.load();
        int total = tasks.size();
        // 每处理100个文件或完成5%时打印一次
        if (current - last_progress >= 100 || 
            (total > 0 && (current * 100 / total) > (last_progress * 100 / total) + 4)) {
            double percent = total > 0 ? (current * 100.0 / total) : 0;
            std::cout << "\r[Progress] " << current << "/" << total 
                     << " (" << std::fixed << std::setprecision(1) << percent << "%)"
                     << " | Success: " << success_count 
                     << " | Failed: " << failed_count << "          " << std::flush;
            last_progress = current;
        }
    };
    
    for (const auto& task : tasks) {
        // 等待空闲线程
        while (futures.size() >= static_cast<size_t>(max_workers)) {
            for (auto it = futures.begin(); it != futures.end(); ) {
                if (it->wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
                    auto result = it->get();
                    if (result.success) {
                        success_count++;
                    } else {
                        failed_count++;
                    }
                    processed_count++;
                    printProgress();
                    
                    it = futures.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        // 提交新任务
        futures.push_back(std::async(std::launch::async, processSingleFile, task));
    }
    
    // 等待所有任务完成
    for (auto& f : futures) {
        auto result = f.get();
        if (result.success) {
            success_count++;
        } else {
            failed_count++;
        }
        processed_count++;
        printProgress();
    }
    
    // 最终进度
    std::cout << "\r[Progress] " << processed_count << "/" << tasks.size() 
             << " (100.0%) | Success: " << success_count 
             << " | Failed: " << failed_count << "          " << std::endl;
    
    auto overall_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::seconds>(overall_end - overall_start);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Date " << date_str << " completed!" << std::endl;
    std::cout << "Total files: " << total_files << std::endl;
    std::cout << "Success: " << success_count << std::endl;
    std::cout << "Failed: " << failed_count << std::endl;
    std::cout << "Total time: " << total_duration.count() << " seconds" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    final_result.date = date_str;
    final_result.total = total_files;
    final_result.success = success_count;
    final_result.failed = failed_count;
    final_result.elapsed_seconds = total_duration.count();
    
    return final_result;
}

// 处理日期范围
std::vector<LobDataProcessor::ProcessResult> processDateRange(
    const std::time_t& start_date,
    const std::time_t& end_date,
    const ClickHouseConfig& ch_config,
    const std::string& market = "all",
    const std::string& symbol = "",
    const std::string& h5_base_path = "/tmp/lob_in",
    const std::string& level2_base_path = "/tmp/lob_in_nolevel2",
    const std::string& csv_base_path = "/home/sharedriver1/public/Lob_new_3s",
    bool save_to_ch = true,
    bool save_to_csv = false,
    int max_workers = 20,
    bool skip_weekends = true,
    const std::string& table_name = "Tick_3sec_data",
    int time_start = -1,
    int time_end = -1,
    bool save_to_parquet_shard = false,
    const std::string& parquet_staging_dir = "") {
    
    std::vector<LobDataProcessor::ProcessResult> all_results;
    
    std::cout << "\n########################################" << std::endl;
    std::cout << "Processing date range: " 
             << Utils::formatDate(start_date, "%Y-%m-%d") << " to "
             << Utils::formatDate(end_date, "%Y-%m-%d") << std::endl;
    std::cout << "Market filter: " << market << std::endl;
    if (!symbol.empty()) {
        std::cout << "Symbol filter: " << symbol << std::endl;
    }
    std::cout << "LOB H5 base path: " << h5_base_path << std::endl;
    std::cout << "Level2 base path: " << level2_base_path << std::endl;
    std::cout << "CSV base path: " << csv_base_path << std::endl;
    std::cout << "Max workers: " << max_workers << std::endl;
    std::cout << "Skip weekends: " << (skip_weekends ? "Yes" : "No") << std::endl;
    std::cout << "########################################\n" << std::endl;
    
    std::time_t current_date = start_date;
    
    while (current_date <= end_date) {
        // 跳过周末
        if (skip_weekends && Utils::isWeekend(current_date)) {
            std::cout << "Skipping weekend: " 
                     << Utils::formatDate(current_date, "%Y-%m-%d") << std::endl;
            current_date += 86400;  // +1 day
            continue;
        }
        
        // 处理当日数据
        try {
            auto result = processDate(
                current_date,
                ch_config,
                market,
                symbol,
                h5_base_path,
                level2_base_path,
                csv_base_path,
                save_to_ch,
                save_to_csv,
                max_workers,
                table_name,
                time_start,
                time_end,
                save_to_parquet_shard,
                parquet_staging_dir
            );
            all_results.push_back(result);
        }
        catch (const std::exception& e) {
            std::cerr << "Error processing date " 
                     << Utils::formatDate(current_date, "%Y-%m-%d") 
                     << ": " << e.what() << std::endl;
            
            LobDataProcessor::ProcessResult error_result;
            error_result.date = Utils::formatDate(current_date, "%Y%m%d");
            error_result.total = 0;
            error_result.success = 0;
            error_result.failed = 0;
            error_result.elapsed_seconds = 0.0;
            all_results.push_back(error_result);
        }
        
        current_date += 86400;  // +1 day
    }
    
    // 汇总统计
    int total_files = 0;
    int total_success = 0;
    int total_failed = 0;
    double total_time = 0.0;
    
    for (const auto& r : all_results) {
        total_files += r.total;
        total_success += r.success;
        total_failed += r.failed;
        total_time += r.elapsed_seconds;
    }
    
    std::cout << "\n########################################" << std::endl;
    std::cout << "Date range processing completed!" << std::endl;
    std::cout << "Total dates: " << all_results.size() << std::endl;
    std::cout << "Total files: " << total_files << std::endl;
    std::cout << "Total success: " << total_success << std::endl;
    std::cout << "Total failed: " << total_failed << std::endl;
    std::cout << "Total time: " << total_time << " seconds" << std::endl;
    std::cout << "########################################\n" << std::endl;
    
    return all_results;
}

int main(int argc, char* argv[]) {
    try {
        // ClickHouse配置
        ClickHouseConfig ch_config;
        ch_config.host = "192.168.30.109";
        ch_config.port = 9000;  // clickhouse-cpp使用TCP端口(9000)，不是HTTP端口(8123)
        ch_config.username = "default";
        ch_config.password = "mxzichan@";
        ch_config.database = "Tick_TL";
        
        // 解析参数
        std::string market = "all";
        std::string symbol = "";
        std::string table_name = "Tick_3sec_data";
        int time_start = -1;
        int time_end = -1;
        // --output 默认 ch (向后兼容)
        std::string output_mode = "ch";
        std::string parquet_out_dir = "/tmp/lob_in_nolevel2_MX";
        std::vector<std::string> args;
        
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--market" || arg == "-m") {
                if (i + 1 < argc) {
                    market = argv[i + 1];
                    // 验证market参数
                    if (market != "sz" && market != "sh" && market != "all") {
                        std::cerr << "Error: Invalid market value '" << market << "'. Must be 'sz', 'sh', or 'all'" << std::endl;
                        return 1;
                    }
                    ++i;  // 跳过market值
                } else {
                    std::cerr << "Error: --market requires a value (sz|sh|all)" << std::endl;
                    return 1;
                }
            } else if (arg == "--symbol" || arg == "-s") {
                if (i + 1 < argc) {
                    symbol = argv[i + 1];
                    ++i;  // 跳过symbol值
                } else {
                    std::cerr << "Error: --symbol requires a value (stock code)" << std::endl;
                    return 1;
                }
            } else if (arg == "--table" || arg == "-t") {
                if (i + 1 < argc) {
                    table_name = argv[i + 1];
                    ++i;
                } else {
                    std::cerr << "Error: --table requires a value (table name)" << std::endl;
                    return 1;
                }
            } else if (arg == "--time-start") {
                if (i + 1 < argc) {
                    time_start = parseTimeToSeconds(argv[i + 1]);
                    ++i;
                } else {
                    std::cerr << "Error: --time-start requires a value (HH:MM:SS)" << std::endl;
                    return 1;
                }
            } else if (arg == "--time-end") {
                if (i + 1 < argc) {
                    time_end = parseTimeToSeconds(argv[i + 1]);
                    ++i;
                } else {
                    std::cerr << "Error: --time-end requires a value (HH:MM:SS)" << std::endl;
                    return 1;
                }
            } else if (arg == "--output") {
                if (i + 1 < argc) {
                    output_mode = argv[i + 1];
                    if (output_mode != "ch" && output_mode != "parquet" && output_mode != "both") {
                        std::cerr << "Error: Invalid --output '" << output_mode
                                  << "'. Must be 'ch', 'parquet', or 'both'" << std::endl;
                        return 1;
                    }
                    ++i;
                } else {
                    std::cerr << "Error: --output requires a value (ch|parquet|both)" << std::endl;
                    return 1;
                }
            } else if (arg == "--parquet-out-dir") {
                if (i + 1 < argc) {
                    parquet_out_dir = argv[i + 1];
                    ++i;
                } else {
                    std::cerr << "Error: --parquet-out-dir requires a value" << std::endl;
                    return 1;
                }
            } else {
                args.push_back(arg);
            }
        }
        
        // 解析 output 模式 -> 两个 bool
        bool save_to_ch = (output_mode == "ch" || output_mode == "both");
        bool save_to_parquet_shard = (output_mode == "parquet" || output_mode == "both");
        // parquet 模式打印提示，避免使用者以为还会自动合并
        if (save_to_parquet_shard) {
            std::cout << "[INFO] Parquet shard mode: writing per-symbol .bin to "
                      << parquet_out_dir << "/.staging/{date}/" << std::endl;
            std::cout << "[INFO] After all dates finish, run merge_3s_parquet.py to produce final {date}_tick.parquet" << std::endl;
        }
        std::string parquet_staging_dir = parquet_out_dir + "/.staging";

        if (args.empty()) {
            // 处理当天
            std::time_t today = std::time(nullptr);
            std::cout << "处理当天数据: " << Utils::formatDate(today, "%Y-%m-%d") << std::endl;

            processDate(today, ch_config, market, symbol,
                       "/tmp/lob_in",
                       "/tmp/lob_in_nolevel2",
                       "/home/sharedriver1/public/Lob_new_3s",
                       save_to_ch, false, 20, table_name, time_start, time_end,
                       save_to_parquet_shard, parquet_staging_dir);
        }
        else if (args.size() == 1) {
            std::string arg1 = args[0];

            if (arg1 == "-h" || arg1 == "--help" || arg1 == "help") {
                printUsage(argv[0]);
                return 0;
            }

            // 处理指定日期
            std::time_t target_date = Utils::parseDate(arg1);
            std::cout << "处理指定日期: " << Utils::formatDate(target_date, "%Y-%m-%d") << std::endl;

            processDate(target_date, ch_config, market, symbol,
                       "/tmp/lob_in",
                       "/tmp/lob_in_nolevel2",
                       "/home/sharedriver1/public/Lob_new_3s",
                       save_to_ch, false, 20, table_name, time_start, time_end,
                       save_to_parquet_shard, parquet_staging_dir);
        }
        else if (args.size() == 2) {
            // 处理日期范围
            std::time_t start_date = Utils::parseDate(args[0]);
            std::time_t end_date = Utils::parseDate(args[1]);

            std::cout << "处理日期范围: "
                     << Utils::formatDate(start_date, "%Y-%m-%d") << " 到 "
                     << Utils::formatDate(end_date, "%Y-%m-%d") << std::endl;

            processDateRange(start_date, end_date, ch_config, market, symbol,
                            "/tmp/lob_in",
                            "/tmp/lob_in_nolevel2",
                            "/home/sharedriver1/public/Lob_new_3s",
                            save_to_ch, false, 20, true, table_name, time_start, time_end,
                            save_to_parquet_shard, parquet_staging_dir);
        }
        else {
            std::cerr << "Error: Too many arguments" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
