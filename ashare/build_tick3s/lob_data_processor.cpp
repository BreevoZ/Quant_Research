/**
 * LOB数据3秒降频处理模块 - C++实现
 * 实现文件
 */

#include "lob_data_processor.h"
#include <H5Cpp.h>
#include <clickhouse/client.h>
#include <clickhouse/columns/column.h>
#include <clickhouse/columns/numeric.h>
#include <clickhouse/columns/string.h>
#include <clickhouse/columns/date.h>
#include <clickhouse/block.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <thread>
#include <future>
#include <iomanip>
#include <numeric>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_set>
#include <mutex>

namespace fs = std::filesystem;

// HDF5 库默认不是线程安全的，多线程同时读写会导致内部状态错乱
// 必须用互斥锁串行化所有 HDF5 I/O 操作
static std::mutex g_hdf5_mutex;

// ============================================================
// AggregatedData构造函数实现
// ============================================================

AggregatedData::AggregatedData() {
    tradedate = 0;
    instrumentID = 0;
    dateTime = 0;
    
    // 基础统计初始化为0
    wtBidNum = wtAskNum = zbBidNum = zbAskNum = clBidNum = clAskNum = 0;
    wtBidVol = wtAskVol = zbBidVol = zbAskVol = clBidVol = clAskVol = 0;
    wtBidAmt = wtAskAmt = zbBidAmt = zbAskAmt = clBidAmt = clAskAmt = 0.0;
    wtBidp = wtAskp = zbBidp = zbAskp = clBidp = clAskp = 0.0;
    
    wtBidp1 = wtAskp1 = zbBidp1 = zbAskp1 = 0.0;
    wtBidv1 = wtAskv1 = zbBidv1 = zbAskv1 = 0;
    
    wtBidWeightPrice = wtAskWeightPrice = 0.0;
    
    wtVol = zbVol = clVol = 0;
    wtNum = zbNum = clNum = 0;
    
    wtBidNumRatio = wtAskNumRatio = 0.0;
    wtBidVolRatio = wtAskVolRatio = 0.0;
    wtBidAvgPos = wtAskAvgPos = 0.0;
    wtABidVol = wtAAskVol = 0;
    wtABidNum = wtAAskNum = 0;
    wtPBidPos = wtPAskPos = 0.0;
    wtPBidVol = wtPAskVol = 0;
    wtPBidNum = wtPAskNum = 0;
    
    bidMaxPos = askMaxPos = 0;
    bidMaxPrice = askMaxPrice = 0.0;
    bidMaxVol = askMaxVol = 0;
    
    // 档位区间初始化
    wtBid1Num = wtBid2_4Num = wtBid5_10Num = wtBid11InfNum = 0;
    wtBid1Vol = wtBid2_4Vol = wtBid5_10Vol = wtBid11InfVol = 0;
    wtBid1Amt = wtBid2_4Amt = wtBid5_10Amt = wtBid11InfAmt = 0.0;
    wtBid2_4AvgPrice = wtBid5_10AvgPrice = wtBid11InfAvgPrice = 0.0;
    
    wtAsk1Num = wtAsk2_4Num = wtAsk5_10Num = wtAsk11InfNum = 0;
    wtAsk1Vol = wtAsk2_4Vol = wtAsk5_10Vol = wtAsk11InfVol = 0;
    wtAsk1Amt = wtAsk2_4Amt = wtAsk5_10Amt = wtAsk11InfAmt = 0.0;
    wtAsk2_4AvgPrice = wtAsk5_10AvgPrice = wtAsk11InfAvgPrice = 0.0;
    
    clBid1Num = clBid2_4Num = clBid5_10Num = 0;
    clBid1Vol = clBid2_4Vol = clBid5_10Vol = 0;
    clBid1Amt = clBid2_4Amt = clBid5_10Amt = 0.0;
    clBid2_4AvgPrice = clBid5_10AvgPrice = 0.0;
    
    clAsk1Num = clAsk2_4Num = clAsk5_10Num = 0;
    clAsk1Vol = clAsk2_4Vol = clAsk5_10Vol = 0;
    clAsk1Amt = clAsk2_4Amt = clAsk5_10Amt = 0.0;
    clAsk2_4AvgPrice = clAsk5_10AvgPrice = 0.0;
    
    // Type分类初始化
    bidType1Vol = bidType2Vol = bidType3Vol = bidType4Vol = bidType5Vol = bidType6Vol = 0;
    bidType1Num = bidType2Num = bidType3Num = bidType4Num = bidType5Num = bidType6Num = 0;
    askType1Vol = askType2Vol = askType3Vol = askType4Vol = askType5Vol = askType6Vol = 0;
    askType1Num = askType2Num = askType3Num = askType4Num = askType5Num = askType6Num = 0;
    
    // Pending初始化
    pendingBid1Num = pendingBid2_4Num = pendingBid5_10Num = pendingBidTotalNum = 0;
    pendingBid1Vol = pendingBid2_4Vol = pendingBid5_10Vol = pendingBidTotalVol = 0;
    pendingBid1Amt = pendingBid2_4Amt = pendingBid5_10Amt = pendingBidTotalAmt = 0.0;
    
    pendingAsk1Num = pendingAsk2_4Num = pendingAsk5_10Num = pendingAskTotalNum = 0;
    pendingAsk1Vol = pendingAsk2_4Vol = pendingAsk5_10Vol = pendingAskTotalVol = 0;
    pendingAsk1Amt = pendingAsk2_4Amt = pendingAsk5_10Amt = pendingAskTotalAmt = 0.0;
    
    // 盘口初始化
    bidPrice.fill(0.0);
    bidVolume.fill(0);
    askPrice.fill(0.0);
    askVolume.fill(0);
    
    // 行情初始化
    totalVolume = 0;
    totalAmount = 0.0;
    openPrice = highPrice = lowPrice = lastPrice = 0.0;
    dayHigh = dayLow = vwap = 0.0;
    
    eventCount = 0;
    dataFlag = 0;
}

// ============================================================
// H5DataReader实现
// ============================================================

H5DataReader::H5DataReader(const std::string& h5_path, const std::string& level2_path)
    : h5_path_(h5_path), level2_path_(level2_path) {
    
    // 从文件名提取股票代码
    fs::path p(h5_path);
    stock_code_ = p.stem().string();
    
    // 加载Level2数据
    if (!level2_path_.empty() && fs::exists(level2_path_)) {
        loadLevel2Data();
    } else if (!level2_path_.empty()) {
        std::cerr << "Warning: Level2 file not found: " << level2_path_ << std::endl;
    }
}

H5DataReader::~H5DataReader() {}

void H5DataReader::loadLevel2Data() {
    try {
        // 禁用HDF5默认错误输出 (避免大量HDF5-DIAG信息)
        H5::Exception::dontPrint();
        
        H5::H5File file(level2_path_, H5F_ACC_RDONLY);
        
        // ========== 读取Level2 order表 ==========
        // 注意: pandas HDFStore格式使用PyTables结构
        // 需要读取 /order/table 或 /order/block0_values 等
        try {
            if (file.nameExists("/order")) {
                // 尝试读取order表获取价格和类型映射
                // PyTables格式通常有 /order/table 作为数据存储
                std::string order_path = "/order";
                try {
                    if (file.nameExists("/order/table")) {
                        order_path = "/order/table";
                    }
                } catch (...) {
                    // 忽略检查失败，使用默认路径
                }
                
                H5::DataSet order_ds = file.openDataSet(order_path);
                H5::DataSpace order_space = order_ds.getSpace();
                hsize_t order_dims[2];
                order_space.getSimpleExtentDims(order_dims);
                
                // 静默加载，不输出每个文件的详情
                // std::cout << "  Level2 order records: " << order_dims[0] << std::endl;
                
                // TODO: 根据实际PyTables结构读取字段
                // 字段: sysid/ApplSeqNum, OrderPrice/price, OrderType/ordertype
            }
        }
        catch (H5::Exception& e) {
            // 静默忽略，Level2 order数据是可选的
        }
        catch (...) {
            // 静默忽略其他异常
        }
        
        // ========== 读取Level2 trade表 ==========
        try {
            if (file.nameExists("/trade")) {
                std::string trade_path = "/trade";
                try {
                    if (file.nameExists("/trade/table")) {
                        trade_path = "/trade/table";
                    }
                } catch (...) {
                    // 忽略检查失败，使用默认路径
                }
                
                H5::DataSet trade_ds = file.openDataSet(trade_path);
                H5::DataSpace trade_space = trade_ds.getSpace();
                hsize_t trade_dims[2];
                trade_space.getSimpleExtentDims(trade_dims);
                
                // 静默加载，不输出每个文件的详情
                // std::cout << "  Level2 trade records: " << trade_dims[0] << std::endl;
                
                // TODO: 根据实际PyTables结构读取字段
                // 字段: sysid, BuyOrderID/BuyNo, SellOrderID/SellNo, TradePrice/price
            }
        }
        catch (H5::Exception& e) {
            // 静默忽略，Level2 trade数据是可选的
        }
        catch (...) {
            // 静默忽略其他异常
        }
        
        // 静默加载，不输出成功信息
        // std::cout << "Loaded Level2 data from " << level2_path_ << std::endl;
    }
    catch (H5::Exception& e) {
        std::cerr << "[WARN] Failed to load Level2: " << level2_path_ << std::endl;
    }
    catch (...) {
        std::cerr << "[WARN] Failed to load Level2: " << level2_path_ << std::endl;
    }
}

double H5DataReader::timeStringToSeconds(const std::string& time_str) const {
    // 格式: "HH:MM:SS.mmm" 如 "09:15:00.070"
    if (time_str.empty()) return 0.0;
    
    try {
        // 分割时:分:秒
        size_t pos1 = time_str.find(':');
        size_t pos2 = time_str.find(':', pos1 + 1);
        size_t pos3 = time_str.find('.');
        
        if (pos1 == std::string::npos || pos2 == std::string::npos) {
            return 0.0;
        }
        
        int h = std::stoi(time_str.substr(0, pos1));
        int m = std::stoi(time_str.substr(pos1 + 1, pos2 - pos1 - 1));
        
        double s = 0.0;
        if (pos3 != std::string::npos) {
            // 有毫秒部分: "SS.mmm"
            int sec = std::stoi(time_str.substr(pos2 + 1, pos3 - pos2 - 1));
            int ms = std::stoi(time_str.substr(pos3 + 1));
            s = sec + ms / 1000.0;
        } else {
            // 无毫秒部分
            s = std::stod(time_str.substr(pos2 + 1));
        }
        
        return h * 3600.0 + m * 60.0 + s;
    }
    catch (const std::exception& e) {
        std::cerr << "Warning: Failed to parse time string '" << time_str << "': " << e.what() << std::endl;
        return 0.0;
    }
}

// 辅助函数：从HDF5属性读取字符串（通用实现）
static std::string readH5StringFromAttr(H5::Attribute& attr) {
    try {
        H5::DataType dtype = attr.getDataType();
        if (dtype.getClass() == H5T_STRING) {
            H5::StrType str_type(dtype.getId());
            if (str_type.isVariableStr()) {
                // 变长字符串
                char* str_ptr = nullptr;
                attr.read(dtype, &str_ptr);
                if (str_ptr) {
                    std::string result(str_ptr);
                    free(str_ptr);
                    return result;
                }
            } else {
                // 定长字符串
                size_t str_size = dtype.getSize();
                std::vector<char> buf(str_size + 1, 0);
                attr.read(dtype, buf.data());
                // 使用 str_size 构造，避免截断含有中间null的pickle数据
                // 但对于 pickle protocol 0 一般不含null，用strlen风格即可
                return std::string(buf.data());
            }
        }
    } catch (...) {}
    return "";
}

// 辅助函数：从HDF5 dataset读取字符串属性
static std::string readH5StringAttribute(H5::DataSet& dataset, const std::string& attr_name) {
    if (!dataset.attrExists(attr_name)) return "";
    try {
        H5::Attribute attr = dataset.openAttribute(attr_name);
        return readH5StringFromAttr(attr);
    } catch (...) {}
    return "";
}

// 辅助函数：从HDF5 group读取字符串属性
static std::string readH5GroupStringAttribute(H5::Group& group, const std::string& attr_name) {
    if (!group.attrExists(attr_name)) return "";
    try {
        H5::Attribute attr = group.openAttribute(attr_name);
        return readH5StringFromAttr(attr);
    } catch (...) {}
    return "";
}

// 辅助函数：解析Python pickle protocol 0格式的字符串列表
// 格式: (lp0\nVname1\np1\naVname2\np2\na...\n.
// 第一个元素: Vname\n  (V = unicode string)
// 后续元素: aVname\n  (a = append, V = unicode string)
static std::vector<std::string> parsePickleList(const std::string& pickle) {
    std::vector<std::string> names;
    std::istringstream iss(pickle);
    std::string line;
    while (std::getline(iss, line)) {
        // 移除尾部\r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        
        // 匹配 'V...' (第一个元素) 或 'aV...' (后续append元素)
        if (line[0] == 'V' && line.size() > 1) {
            names.push_back(line.substr(1));
        } else if (line.size() > 2 && line[0] == 'a' && line[1] == 'V') {
            names.push_back(line.substr(2));
        }
    }
    return names;
}

// 读取PyTables列名映射（从values_block_X_kind属性解析）
// PyTables的FIELD_X_NAME只包含compound成员名(index, values_block_0等)，
// 真正的列名存储在values_block_X_kind属性中，格式为pickle序列化的列表。
// 注意: 属性可能在DATASET(/order/table)上，也可能在GROUP(/order)上,
// PyTables的_v_attrs通常写在GROUP节点上，需要两处都尝试。
std::unordered_map<std::string, int> H5DataReader::readPyTablesColumns(H5::DataSet& dataset) {
    std::unordered_map<std::string, int> column_map;
    
    // 辅助lambda: 从多个来源尝试读取属性
    // 先尝试dataset属性，再尝试group属性
    H5::H5File* file_ptr = nullptr;
    std::string group_path;
    
    // 尝试获取父group (从h5_path_推断)
    // order_path 可能是 "/order/table" 或 "/order"
    // 如果是 "/order/table", 父group是 "/order"
    auto try_read_attr = [&](const std::string& attr_name) -> std::string {
        // 1. 先尝试从dataset读取
        std::string result = readH5StringAttribute(dataset, attr_name);
        if (!result.empty()) return result;
        
        // 2. 尝试从父group读取 (PyTables通常将属性写在group上)
        try {
            H5::H5File file(h5_path_, H5F_ACC_RDONLY);
            // 尝试 /order group
            for (const auto& gpath : {std::string("/order"), std::string("/trade_1500"), std::string("/trade_1400")}) {
                try {
                    H5::Group group = file.openGroup(gpath);
                    result = readH5GroupStringAttribute(group, attr_name);
                    if (!result.empty()) {
                        return result;
                    }
                } catch (...) {
                    continue;
                }
            }
        } catch (...) {}
        
        return "";
    };
    
    try {
        int global_idx = 0;
        
        // Block 0: float64列 (values_block_0_kind)
        std::string kind0 = try_read_attr("values_block_0_kind");
        if (!kind0.empty()) {
            auto names = parsePickleList(kind0);
            for (const auto& name : names) {
                column_map[name] = global_idx++;
            }
        }
        
        // Block 1: int64列 (values_block_1_kind)
        std::string kind1 = try_read_attr("values_block_1_kind");
        if (!kind1.empty()) {
            auto names = parsePickleList(kind1);
            for (const auto& name : names) {
                column_map[name] = global_idx++;
            }
        }
        
        // Block 2: string列 (values_block_2_kind，可能不存在，如trade表)
        std::string kind2 = try_read_attr("values_block_2_kind");
        if (!kind2.empty()) {
            auto names = parsePickleList(kind2);
            for (const auto& name : names) {
                column_map[name] = global_idx++;
            }
        }
        
        // 输出找到的列信息用于调试
        if (!column_map.empty()) {
            // 检查关键列是否存在
            bool has_bdv = column_map.count("bdv1") > 0;
            bool has_akv = column_map.count("akv1") > 0;
            bool has_bdp = column_map.count("bdp1") > 0;
            if (!has_bdv || !has_akv) {
                std::cerr << "[WARN] PyTables column map missing LOB volume columns!"
                          << " bdp1=" << (has_bdp ? "found" : "MISSING")
                          << " bdv1=" << (has_bdv ? "found" : "MISSING")
                          << " akv1=" << (has_akv ? "found" : "MISSING")
                          << " total_cols=" << column_map.size() << std::endl;
            }
        }
    }
    catch (...) {
        // 属性读取失败，返回空映射
        std::cerr << "[WARN] Failed to read PyTables column attributes" << std::endl;
    }
    
    return column_map;
}

// 读取PyTables格式的order数据（完整实现）
// PyTables将数据存储为compound type的子数组:
//   dtype: [('index', '<i8'), ('values_block_0', '<f8', (46,)),
//           ('values_block_1', '<i8', (6,)), ('values_block_2', 'S12', (3,))]
// 注意：数据不是独立的子dataset，而是compound type内的固定大小数组。
std::vector<OrderRecord> H5DataReader::readPyTablesOrderData(H5::H5File& /* file */,
                                                             H5::DataSet& dataset, 
                                                             const std::unordered_map<std::string, int>& column_map) {
    std::vector<OrderRecord> orders;
    
    try {
        // 注意: 不在此处加锁，调用方(processH5File)已持有g_hdf5_mutex
        
        // 获取数据集维度
        H5::DataSpace dataspace = dataset.getSpace();
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        hsize_t n_records = dims[0];
        
        if (n_records == 0) {
            return orders;
        }
        
        orders.reserve(n_records);
        
        // 获取compound type信息
        H5::CompType ctype = dataset.getCompType();
        size_t record_size = ctype.getSize();
        
        // 定位各block在compound type中的成员
        int block0_midx = -1, block1_midx = -1, block2_midx = -1;
        for (int i = 0; i < ctype.getNmembers(); i++) {
            std::string name = ctype.getMemberName(i);
            if (name == "values_block_0") block0_midx = i;
            else if (name == "values_block_1") block1_midx = i;
            else if (name == "values_block_2") block2_midx = i;
        }
        
        // 获取各block在record中的偏移量和子数组维度
        size_t off_block0 = 0, off_block1 = 0, off_block2 = 0;
        size_t n_float_cols = 0, n_int_cols = 0, n_str_cols = 0;
        size_t str_item_size = 0;
        
        if (block0_midx >= 0) {
            off_block0 = ctype.getMemberOffset(block0_midx);
            H5::DataType mt = ctype.getMemberDataType(block0_midx);
            if (mt.getClass() == H5T_ARRAY) {
                H5::ArrayType at(mt.getId());
                hsize_t arr_dims[1];
                at.getArrayDims(arr_dims);
                n_float_cols = arr_dims[0];
            }
        }
        
        if (block1_midx >= 0) {
            off_block1 = ctype.getMemberOffset(block1_midx);
            H5::DataType mt = ctype.getMemberDataType(block1_midx);
            if (mt.getClass() == H5T_ARRAY) {
                H5::ArrayType at(mt.getId());
                hsize_t arr_dims[1];
                at.getArrayDims(arr_dims);
                n_int_cols = arr_dims[0];
            }
        }
        
        if (block2_midx >= 0) {
            off_block2 = ctype.getMemberOffset(block2_midx);
            H5::DataType mt = ctype.getMemberDataType(block2_midx);
            if (mt.getClass() == H5T_ARRAY) {
                H5::ArrayType at(mt.getId());
                hsize_t arr_dims[1];
                at.getArrayDims(arr_dims);
                n_str_cols = arr_dims[0];
                // 获取单个字符串的大小 (如S12 -> 12)
                H5::DataType base = at.getSuper();
                str_item_size = base.getSize();
            }
        }
        
        std::cout << "[DEBUG] PyTables order compound: float=" << n_float_cols 
                  << " int=" << n_int_cols << " str=" << n_str_cols 
                  << " str_size=" << str_item_size << std::endl;
        
        // 全局索引约定: [0, n_float) = float block, [n_float, n_float+n_int) = int block,
        //               [n_float+n_int, total) = string block
        size_t int_offset = n_float_cols;
        size_t str_offset = n_float_cols + n_int_cols;
        
        // 列名查找
        auto get_col_idx = [&](const std::string& name) -> int {
            auto it = column_map.find(name);
            return (it != column_map.end()) ? it->second : -1;
        };
        
        // 关键字段索引
        int idx_sysid = get_col_idx("sysid");
        int idx_ordertype = get_col_idx("ordertype");
        int idx_direction = get_col_idx("direction");
        int idx_time = get_col_idx("time");
        int idx_price = get_col_idx("price");
        int idx_volume = get_col_idx("volume");
        int idx_ask_position = get_col_idx("ask_position");
        int idx_bid_position = get_col_idx("bid_position");
        int idx_type = get_col_idx("type");
        int idx_tradevolume = get_col_idx("tradevolume");
        
        // 盘口字段索引
        std::array<int, 10> idx_bdp, idx_bdv, idx_akp, idx_akv;
        for (int i = 0; i < 10; ++i) {
            idx_bdp[i] = get_col_idx("bdp" + std::to_string(i + 1));
            idx_bdv[i] = get_col_idx("bdv" + std::to_string(i + 1));
            idx_akp[i] = get_col_idx("akp" + std::to_string(i + 1));
            idx_akv[i] = get_col_idx("akv" + std::to_string(i + 1));
        }
        
        // 检查数据集是否使用了压缩 (需要HDF5插件支持)
        H5::DSetCreatPropList plist = dataset.getCreatePlist();
        int n_filters = plist.getNfilters();
        if (n_filters > 0) {
            std::cerr << "[WARN] Dataset uses " << n_filters << " compression filter(s):" << std::endl;
            for (int fi = 0; fi < n_filters; fi++) {
                size_t cd_nelmts = 0;
                unsigned int flags = 0;
                unsigned int filter_config = 0;
                char filter_name[256] = {0};
                H5Z_filter_t filter_id = plist.getFilter(fi, flags, cd_nelmts, nullptr, 256, filter_name, filter_config);
                std::cerr << "[WARN]   Filter " << fi << ": id=" << filter_id 
                          << " name='" << filter_name << "'" << std::endl;
                // 检查过滤器是否可用
                htri_t avail = H5Zfilter_avail(filter_id);
                if (avail <= 0) {
                    std::cerr << "[ERROR] Compression filter '" << filter_name 
                              << "' (id=" << filter_id << ") is NOT available!" << std::endl;
                    std::cerr << "[ERROR] Install the HDF5 plugin or set HDF5_PLUGIN_PATH" << std::endl;
                    return orders;  // 直接返回，不尝试读取
                }
            }
        }
        
        // 读取全部数据到buffer
        std::vector<char> buffer(n_records * record_size);
        std::cout << "[DEBUG] Reading " << n_records << " records (" 
                  << (n_records * record_size / 1024 / 1024) << " MB)..." << std::flush;
        dataset.read(buffer.data(), ctype);
        std::cout << " done." << std::endl;
        
        // 从buffer中提取值的辅助函数
        // 通用数值提取：自动判断在float还是int block中
        auto get_numeric = [&](const char* rec, int col_idx) -> double {
            if (col_idx < 0) return 0.0;
            if (col_idx < static_cast<int>(int_offset)) {
                // float block
                if (block0_midx < 0) return 0.0;
                const double* floats = reinterpret_cast<const double*>(rec + off_block0);
                return floats[col_idx];
            }
            if (col_idx < static_cast<int>(str_offset)) {
                // int block
                if (block1_midx < 0) return 0.0;
                const int64_t* ints = reinterpret_cast<const int64_t*>(rec + off_block1);
                return static_cast<double>(ints[col_idx - int_offset]);
            }
            return 0.0;
        };
        
        auto get_string = [&](const char* rec, int col_idx) -> std::string {
            if (col_idx < static_cast<int>(str_offset) || block2_midx < 0 || str_item_size == 0) return "";
            int block_col = col_idx - str_offset;
            if (block_col < 0 || block_col >= static_cast<int>(n_str_cols)) return "";
            const char* str_base = rec + off_block2 + block_col * str_item_size;
            std::string s(str_base, str_item_size);
            // 去除尾部null
            size_t pos = s.find('\0');
            if (pos != std::string::npos) s.resize(pos);
            return s;
        };
        
        // 逐行解析
        for (hsize_t i = 0; i < n_records; ++i) {
            const char* rec = buffer.data() + i * record_size;
            OrderRecord order;
            
            order.sysid = static_cast<int64_t>(get_numeric(rec, idx_sysid));
            order.ordertype = get_string(rec, idx_ordertype);
            order.direction = get_string(rec, idx_direction);
            order.time = get_string(rec, idx_time);
            order.price = get_numeric(rec, idx_price);
            order.volume = static_cast<int64_t>(get_numeric(rec, idx_volume));
            order.ask_position = static_cast<int>(get_numeric(rec, idx_ask_position));
            order.bid_position = static_cast<int>(get_numeric(rec, idx_bid_position));
            order.type = static_cast<int>(get_numeric(rec, idx_type));
            order.tradevolume = static_cast<int64_t>(get_numeric(rec, idx_tradevolume));
            
            // 盘口价格和数量 (注意：bdv/akv可能在float block中，pandas会将int升级为float)
            // get_numeric返回double，NaN时static_cast<int64_t>是未定义行为，必须先检查
            for (int j = 0; j < 10; ++j) {
                order.bdp[j] = get_numeric(rec, idx_bdp[j]);
                order.akp[j] = get_numeric(rec, idx_akp[j]);
                double bdv_val = get_numeric(rec, idx_bdv[j]);
                double akv_val = get_numeric(rec, idx_akv[j]);
                order.bdv[j] = (std::isfinite(bdv_val) && bdv_val >= 0) ? static_cast<int64_t>(bdv_val) : 0;
                order.akv[j] = (std::isfinite(akv_val) && akv_val >= 0) ? static_cast<int64_t>(akv_val) : 0;
            }
            
            orders.push_back(order);
        }
        
        std::cout << "[INFO] Successfully decoded PyTables order format: " << orders.size() << " records" << std::endl;
        
    } catch (H5::Exception& e) {
        std::cerr << "[ERROR] Failed to read PyTables order data (H5): " 
                  << e.getDetailMsg() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to read PyTables order data: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[ERROR] Failed to read PyTables order data: unknown exception" << std::endl;
    }
    
    return orders;
}

std::vector<OrderRecord> H5DataReader::readOrderData() {
    std::vector<OrderRecord> orders;
    
    try {
        // 禁用HDF5默认错误输出
        H5::Exception::dontPrint();
        
        H5::H5File file(h5_path_, H5F_ACC_RDONLY);
        
        // 检查order数据集是否存在 (PyTables格式: /order/table)
        std::string order_path = "/order/table";
        bool order_exists = false;
        
        try {
            if (file.nameExists("/order")) {
                order_exists = true;
                // 尝试/order/table路径
                try {
                    if (!file.nameExists(order_path)) {
                        order_path = "/order";
                    }
                } catch (...) {
                    order_path = "/order";
                }
            }
        } catch (...) {
            // 尝试直接打开
            try {
                H5::DataSet test_ds = file.openDataSet("/order/table");
                order_path = "/order/table";
                order_exists = true;
            } catch (...) {
                try {
                    H5::DataSet test_ds = file.openDataSet("/order");
                    order_path = "/order";
                    order_exists = true;
                } catch (...) {
                    // 无法找到order数据
                }
            }
        }
        
        if (!order_exists) {
            std::cerr << "[ERROR] /order not found: " << h5_path_ << std::endl;
            return orders;
        }
        
        H5::DataSet dataset = file.openDataSet(order_path);
        H5::DataSpace dataspace = dataset.getSpace();
        H5::CompType dtype = dataset.getCompType();
        
        // 获取维度
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        size_t n_records = dims[0];
        
        if (n_records == 0) {
            return orders;
        }
        
        // 预留空间
        orders.reserve(n_records);
        
        // 获取字段索引
        int idx_sysid = -1, idx_ordertype = -1, idx_direction = -1;
        int idx_time = -1, idx_price = -1, idx_volume = -1;
        int idx_ask_position = -1, idx_bid_position = -1, idx_type = -1;
        int idx_tradevolume = -1;
        std::array<int, 10> idx_bdp, idx_bdv, idx_akp, idx_akv, idx_bid_num, idx_ask_num;
        idx_bdp.fill(-1);
        idx_bdv.fill(-1);
        idx_akp.fill(-1);
        idx_akv.fill(-1);
        idx_bid_num.fill(-1);
        idx_ask_num.fill(-1);

        int n_members = dtype.getNmembers();
        
        // 检测是否为PyTables格式
        bool is_pytables = false;
        for (int i = 0; i < n_members; ++i) {
            std::string name = dtype.getMemberName(i);
            if (name.find("values_block") != std::string::npos) {
                is_pytables = true;
                break;
            }
        }
        
        // 如果是PyTables格式，读取列名映射
        std::unordered_map<std::string, int> pytables_columns;
        if (is_pytables) {
            std::cout << "[INFO] Detected PyTables format for " << stock_code_ << std::endl;
            pytables_columns = readPyTablesColumns(dataset);
            
            if (pytables_columns.empty()) {
                std::cerr << "[WARN] PyTables format detected but column mapping not found" << std::endl;
                std::cerr << "[INFO] Please use Python version to process this file" << std::endl;
                return orders;
            }
            
            std::cout << "[INFO] Found " << pytables_columns.size() << " columns in PyTables format" << std::endl;
            
            // 解码PyTables格式数据
            orders = readPyTablesOrderData(file, dataset, pytables_columns);
            
            if (!orders.empty()) {
                // 修正价格为0的记录
                fixZeroPrice(orders);
                
                // 计算派生字段 (与Python read_order_data一致，始终重算)
                // 左闭右开 [s, s+3) 分桶 (与实盘流式合成一致): time_slot = floor(t/3)*3
                //         amount = price * volume
                //         position = np.where(direction=='B', ...)
                for (auto& order : orders) {
                    order.time_seconds = timeStringToSeconds(order.time);
                    order.time_slot = static_cast<int>(std::floor(order.time_seconds / 3.0)) * 3;
                    order.amount = order.price * order.volume;
                    
                    // 计算position (与Python逻辑一致)
                    if (order.direction == "B") {
                        order.position = (order.ask_position > 0) ? order.ask_position : -order.bid_position;
                    } else {
                        order.position = (order.bid_position > 0) ? order.bid_position : -order.ask_position;
                    }
                }
            }
            
            return orders;
        }
        
        // 标准HDF5格式：直接读取字段名
        for (int i = 0; i < n_members; ++i) {
            std::string name = dtype.getMemberName(i);
            if (name == "sysid") idx_sysid = i;
            else if (name == "ordertype") idx_ordertype = i;
            else if (name == "direction") idx_direction = i;
            else if (name == "time") idx_time = i;
            else if (name == "price") idx_price = i;
            else if (name == "volume") idx_volume = i;
            else if (name == "ask_position") idx_ask_position = i;
            else if (name == "bid_position") idx_bid_position = i;
            else if (name == "type") idx_type = i;
            else if (name == "tradevolume") idx_tradevolume = i;
            else {
                // 盘口字段: bdp1-10, bdv1-10, akp1-10, akv1-10
                for (int j = 1; j <= 10; ++j) {
                    if (name == "bdp" + std::to_string(j)) idx_bdp[j-1] = i;
                    else if (name == "bdv" + std::to_string(j)) idx_bdv[j-1] = i;
                    else if (name == "akp" + std::to_string(j)) idx_akp[j-1] = i;
                    else if (name == "akv" + std::to_string(j)) idx_akv[j-1] = i;
                    else if (name == "bid_num" + std::to_string(j)) idx_bid_num[j-1] = i;
                    else if (name == "ask_num" + std::to_string(j)) idx_ask_num[j-1] = i;
                }
            }
        }
        
        // 检查必需字段
        // 对于深圳市场(sz前缀)，只要求核心字段: sysid, ordertype, time
        // 对于上海市场(sh前缀)，要求所有字段: sysid, ordertype, direction, time, price, volume
        bool is_sz = (stock_code_.size() >= 2 && stock_code_.substr(0, 2) == "sz");
        
        if (is_sz) {
            // 深圳市场：宽松检查（只要求核心字段）
            if (idx_sysid < 0 || idx_ordertype < 0 || idx_time < 0) {
                std::cerr << "[ERROR] Missing core fields (sysid/ordertype/time) in SZ order table" << std::endl;
                return orders;
            }
        } else {
            // 上海市场或其他：严格检查（要求所有字段）
            if (idx_sysid < 0 || idx_ordertype < 0 || idx_direction < 0 || 
                idx_time < 0 || idx_price < 0 || idx_volume < 0) {
                std::cerr << "[ERROR] Missing required fields in H5 order table" << std::endl;
                return orders;
            }
        }
        
        // 预计算所有字段的offset (避免在循环中重复计算)
        size_t off_sysid = (idx_sysid >= 0) ? dtype.getMemberOffset(idx_sysid) : 0;
        size_t off_ordertype = (idx_ordertype >= 0) ? dtype.getMemberOffset(idx_ordertype) : 0;
        size_t off_direction = (idx_direction >= 0) ? dtype.getMemberOffset(idx_direction) : 0;
        size_t off_time = (idx_time >= 0) ? dtype.getMemberOffset(idx_time) : 0;
        size_t off_price = (idx_price >= 0) ? dtype.getMemberOffset(idx_price) : 0;
        size_t off_volume = (idx_volume >= 0) ? dtype.getMemberOffset(idx_volume) : 0;
        size_t off_ask_position = (idx_ask_position >= 0) ? dtype.getMemberOffset(idx_ask_position) : 0;
        size_t off_bid_position = (idx_bid_position >= 0) ? dtype.getMemberOffset(idx_bid_position) : 0;
        size_t off_type = (idx_type >= 0) ? dtype.getMemberOffset(idx_type) : 0;
        size_t off_tradevolume = (idx_tradevolume >= 0) ? dtype.getMemberOffset(idx_tradevolume) : 0;
        
        // 预计算字符串字段大小
        size_t str_size_ordertype = 0, str_size_direction = 0, str_size_time = 0;
        if (idx_ordertype >= 0) {
            H5::DataType mt = dtype.getMemberDataType(idx_ordertype);
            if (mt.getClass() == H5T_STRING) str_size_ordertype = mt.getSize();
        }
        if (idx_direction >= 0) {
            H5::DataType mt = dtype.getMemberDataType(idx_direction);
            if (mt.getClass() == H5T_STRING) str_size_direction = mt.getSize();
        }
        if (idx_time >= 0) {
            H5::DataType mt = dtype.getMemberDataType(idx_time);
            if (mt.getClass() == H5T_STRING) str_size_time = mt.getSize();
        }
        
        // 预计算盘口字段offset
        std::array<size_t, 10> off_bdp, off_bdv, off_akp, off_akv, off_bid_num, off_ask_num;
        // 检测盘口量字段(bdv/akv)的实际数据类型 (可能是float64或int64)
        bool bdv_is_float = false, akv_is_float = false;
        bool num_is_float = false;  // bid_num/ask_num 实际类型(builder 写 int64)
        for (int j = 0; j < 10; ++j) {
            off_bdp[j] = (idx_bdp[j] >= 0) ? dtype.getMemberOffset(idx_bdp[j]) : 0;
            off_bdv[j] = (idx_bdv[j] >= 0) ? dtype.getMemberOffset(idx_bdv[j]) : 0;
            off_akp[j] = (idx_akp[j] >= 0) ? dtype.getMemberOffset(idx_akp[j]) : 0;
            off_akv[j] = (idx_akv[j] >= 0) ? dtype.getMemberOffset(idx_akv[j]) : 0;
            off_bid_num[j] = (idx_bid_num[j] >= 0) ? dtype.getMemberOffset(idx_bid_num[j]) : 0;
            off_ask_num[j] = (idx_ask_num[j] >= 0) ? dtype.getMemberOffset(idx_ask_num[j]) : 0;
        }
        if (idx_bid_num[0] >= 0 &&
            dtype.getMemberDataType(idx_bid_num[0]).getClass() == H5T_FLOAT) num_is_float = true;
        // 检测bdv/akv数据类型: tl_lob_builder和Python都以float64存储
        if (idx_bdv[0] >= 0) {
            H5::DataType mdt = dtype.getMemberDataType(idx_bdv[0]);
            if (mdt.getClass() == H5T_FLOAT) bdv_is_float = true;
        }
        if (idx_akv[0] >= 0) {
            H5::DataType mdt = dtype.getMemberDataType(idx_akv[0]);
            if (mdt.getClass() == H5T_FLOAT) akv_is_float = true;
        }
        
        // 检测其他可能存为int64但读为int32的字段的实际类型
        bool ask_pos_is_i64 = false, bid_pos_is_i64 = false, type_is_i64 = false;
        if (idx_ask_position >= 0) {
            H5::DataType mdt = dtype.getMemberDataType(idx_ask_position);
            if (mdt.getSize() == 8) ask_pos_is_i64 = true;
        }
        if (idx_bid_position >= 0) {
            H5::DataType mdt = dtype.getMemberDataType(idx_bid_position);
            if (mdt.getSize() == 8) bid_pos_is_i64 = true;
        }
        if (idx_type >= 0) {
            H5::DataType mdt = dtype.getMemberDataType(idx_type);
            if (mdt.getSize() == 8) type_is_i64 = true;
        }
        
        // 输出标准格式字段类型检测结果
        std::cout << "[INFO] Standard H5 format field types for " << stock_code_ << ":"
                  << " bdv=" << (idx_bdv[0] >= 0 ? (bdv_is_float ? "float64" : "int64") : "MISSING")
                  << " akv=" << (idx_akv[0] >= 0 ? (akv_is_float ? "float64" : "int64") : "MISSING")
                  << " bdp=" << (idx_bdp[0] >= 0 ? "found" : "MISSING")
                  << " n_members=" << n_members << std::endl;
        
        // 创建内存数据类型
        size_t record_size = dtype.getSize();
        std::vector<char> buffer(n_records * record_size);
        
        // 读取所有数据
        dataset.read(buffer.data(), dtype);
        
        // 预分配结果空间
        orders.reserve(n_records);
        
        // 解析每条记录 (使用预计算的offset)
        for (size_t i = 0; i < n_records; ++i) {
            char* rec_ptr = buffer.data() + i * record_size;
            OrderRecord order;
            
            // 读取各字段 (使用预计算的偏移量)
            order.sysid = *reinterpret_cast<int64_t*>(rec_ptr + off_sysid);
            
            if (str_size_ordertype > 0) {
                order.ordertype = std::string(rec_ptr + off_ordertype, str_size_ordertype);
                order.ordertype.erase(order.ordertype.find_last_not_of('\0') + 1);
            }
            
            if (str_size_direction > 0) {
                order.direction = std::string(rec_ptr + off_direction, str_size_direction);
                order.direction.erase(order.direction.find_last_not_of('\0') + 1);
            }
            
            if (str_size_time > 0) {
                order.time = std::string(rec_ptr + off_time, str_size_time);
                order.time.erase(order.time.find_last_not_of('\0') + 1);
            }
            
            order.price = *reinterpret_cast<double*>(rec_ptr + off_price);
            order.volume = *reinterpret_cast<int64_t*>(rec_ptr + off_volume);
            
            if (idx_ask_position >= 0) {
                order.ask_position = ask_pos_is_i64 
                    ? static_cast<int>(*reinterpret_cast<int64_t*>(rec_ptr + off_ask_position))
                    : *reinterpret_cast<int32_t*>(rec_ptr + off_ask_position);
            }
            
            if (idx_bid_position >= 0) {
                order.bid_position = bid_pos_is_i64
                    ? static_cast<int>(*reinterpret_cast<int64_t*>(rec_ptr + off_bid_position))
                    : *reinterpret_cast<int32_t*>(rec_ptr + off_bid_position);
            }
            
            if (idx_type >= 0) {
                order.type = type_is_i64
                    ? static_cast<int>(*reinterpret_cast<int64_t*>(rec_ptr + off_type))
                    : *reinterpret_cast<int32_t*>(rec_ptr + off_type);
            }
            
            if (idx_tradevolume >= 0) {
                order.tradevolume = *reinterpret_cast<int64_t*>(rec_ptr + off_tradevolume);
            }
            
            // 读取盘口数据
            // 注意: bdv/akv在H5文件中可能是float64(tl_lob_builder和Python都以double存储)
            // 必须根据实际类型读取，不能直接reinterpret_cast<int64_t*>
            // 当float64值为NaN/Inf时，static_cast<int64_t>是未定义行为，必须先检查
            for (int j = 0; j < 10; ++j) {
                if (idx_bdp[j] >= 0) {
                    order.bdp[j] = *reinterpret_cast<double*>(rec_ptr + off_bdp[j]);
                }
                if (idx_bdv[j] >= 0) {
                    if (bdv_is_float) {
                        double val = *reinterpret_cast<double*>(rec_ptr + off_bdv[j]);
                        order.bdv[j] = (std::isfinite(val) && val >= 0) ? static_cast<int64_t>(val) : 0;
                    } else {
                        order.bdv[j] = *reinterpret_cast<int64_t*>(rec_ptr + off_bdv[j]);
                        if (order.bdv[j] < 0) order.bdv[j] = 0;
                    }
                }
                if (idx_akp[j] >= 0) {
                    order.akp[j] = *reinterpret_cast<double*>(rec_ptr + off_akp[j]);
                }
                if (idx_akv[j] >= 0) {
                    if (akv_is_float) {
                        double val = *reinterpret_cast<double*>(rec_ptr + off_akv[j]);
                        order.akv[j] = (std::isfinite(val) && val >= 0) ? static_cast<int64_t>(val) : 0;
                    } else {
                        order.akv[j] = *reinterpret_cast<int64_t*>(rec_ptr + off_akv[j]);
                        if (order.akv[j] < 0) order.akv[j] = 0;
                    }
                }
                if (idx_bid_num[j] >= 0) {
                    if (num_is_float) { double val = *reinterpret_cast<double*>(rec_ptr + off_bid_num[j]);
                        order.bid_num[j] = (std::isfinite(val) && val >= 0) ? static_cast<int64_t>(val) : 0; }
                    else { order.bid_num[j] = *reinterpret_cast<int64_t*>(rec_ptr + off_bid_num[j]);
                        if (order.bid_num[j] < 0) order.bid_num[j] = 0; }
                }
                if (idx_ask_num[j] >= 0) {
                    if (num_is_float) { double val = *reinterpret_cast<double*>(rec_ptr + off_ask_num[j]);
                        order.ask_num[j] = (std::isfinite(val) && val >= 0) ? static_cast<int64_t>(val) : 0; }
                    else { order.ask_num[j] = *reinterpret_cast<int64_t*>(rec_ptr + off_ask_num[j]);
                        if (order.ask_num[j] < 0) order.ask_num[j] = 0; }
                }
            }
            
            orders.push_back(std::move(order));
        }
        
        // 修正价格为0的记录
        if (!orders.empty()) {
            fixZeroPrice(orders);
        }
        
        // 计算金额和time_slot (3秒桶)
        for (auto& order : orders) {
            order.time_seconds = timeStringToSeconds(order.time);
            order.time_slot = static_cast<int>(std::floor(order.time_seconds / 3.0)) * 3;
            order.amount = order.price * order.volume;
            
            // 计算position (与Python逻辑一致)
            // 买单: ask_position > 0 表示超价单(打入卖盘), bid_position > 0 表示排队
            // 卖单: bid_position > 0 表示超价单(打入买盘), ask_position > 0 表示排队
            if (order.direction == "B") {
                order.position = (order.ask_position > 0) ? order.ask_position : -order.bid_position;
            } else {
                order.position = (order.bid_position > 0) ? order.bid_position : -order.ask_position;
            }
        }
        
    }
    catch (H5::Exception& e) {
        std::cerr << "[ERROR] Read order failed: " << h5_path_ 
                  << " : " << e.getDetailMsg() << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Read order failed: " << h5_path_ 
                  << " : " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[ERROR] Read order failed: " << h5_path_ << " : unknown exception" << std::endl;
    }
    
    return orders;
}

void H5DataReader::fixZeroPrice(std::vector<OrderRecord>& orders) {
    // 修正价格为0的记录（参考Python版本逻辑）
    for (auto& order : orders) {
        if (order.price <= 0.0 || std::isnan(order.price)) {
            
            // 成交(zb): 从Level2 trade表获取价格
            if (order.ordertype == "zb") {
                auto it = level2_trade_price_map_.find(order.sysid);
                if (it != level2_trade_price_map_.end() && it->second > 0) {
                    order.price = it->second;
                }
            }
            // 撤单(cl): 从Level2 order表获取原始价格
            else if (order.ordertype == "cl") {
                // 通过trade表找到原始订单号
                int64_t original_order_id = 0;
                if (order.direction == "B") {
                    auto it = level2_trade_buy_map_.find(order.sysid);
                    if (it != level2_trade_buy_map_.end()) {
                        original_order_id = it->second;
                    }
                } else {
                    auto it = level2_trade_sell_map_.find(order.sysid);
                    if (it != level2_trade_sell_map_.end()) {
                        original_order_id = it->second;
                    }
                }
                
                if (original_order_id > 0) {
                    auto it = level2_order_price_map_.find(original_order_id);
                    if (it != level2_order_price_map_.end() && it->second > 0) {
                        order.price = it->second;
                    }
                }
            }
            // 委托(wt): 市价单或异常价格
            else if (order.ordertype == "wt") {
                if (isMarketOrder(order.sysid)) {
                    // 市价单用对手盘一档
                    if (order.direction == "B") {
                        order.price = order.akp[0];  // 卖一价
                        order.ask_position = 1;
                        order.bid_position = 0;
                    } else {
                        order.price = order.bdp[0];  // 买一价
                        order.bid_position = 1;
                        order.ask_position = 0;
                    }
                } else {
                    // 尝试从Level2获取
                    auto it = level2_order_price_map_.find(order.sysid);
                    if (it != level2_order_price_map_.end() && it->second > 1.0) {
                        order.price = it->second;
                    } else {
                        // 使用对手盘一档
                        if (order.direction == "B") {
                            order.price = order.akp[0];
                        } else {
                            order.price = order.bdp[0];
                        }
                    }
                }
            }
        }
    }
}

bool H5DataReader::isMarketOrder(int64_t order_no) const {
    auto it = level2_order_type_map_.find(order_no);
    if (it == level2_order_type_map_.end()) {
        return false;
    }
    
    const std::string& type = it->second;
    return (type == "1" || type == "U" || type == "B" || type == "b");
}

std::vector<TradeRecord> H5DataReader::readTradeData() {
    std::vector<TradeRecord> trades;
    
    try {
        // 注意: 不在此处加锁，调用方(processH5File)已持有g_hdf5_mutex
        
        // 禁用HDF5默认错误输出
        H5::Exception::dontPrint();
        
        H5::H5File file(h5_path_, H5F_ACC_RDONLY);
        
        // 尝试trade_1500或trade_1400
        std::string trade_table;
        try {
            if (file.nameExists("/trade_1500/table")) {
                trade_table = "/trade_1500/table";
            } else if (file.nameExists("/trade_1500")) {
                trade_table = "/trade_1500";
            } else if (file.nameExists("/trade_1400/table")) {
                trade_table = "/trade_1400/table";
            } else if (file.nameExists("/trade_1400")) {
                trade_table = "/trade_1400";
            }
        } catch (...) {
            // 尝试直接打开
            try {
                H5::DataSet test = file.openDataSet("/trade_1500/table");
                trade_table = "/trade_1500/table";
            } catch (...) {
                try {
                    H5::DataSet test = file.openDataSet("/trade_1500");
                    trade_table = "/trade_1500";
                } catch (...) {
                    try {
                        H5::DataSet test = file.openDataSet("/trade_1400/table");
                        trade_table = "/trade_1400/table";
                    } catch (...) {
                        try {
                            H5::DataSet test = file.openDataSet("/trade_1400");
                            trade_table = "/trade_1400";
                        } catch (...) {
                            // 没有trade表
                        }
                    }
                }
            }
        }
        
        if (trade_table.empty()) {
            return trades;
        }
        
        H5::DataSet dataset = file.openDataSet(trade_table);
        H5::DataSpace dataspace = dataset.getSpace();
        H5::CompType dtype = dataset.getCompType();
        
        // 获取维度
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        size_t n_records = dims[0];
        
        if (n_records == 0) {
            return trades;
        }
        
        trades.reserve(n_records);
        
        // 检测是否为PyTables格式
        int n_members = dtype.getNmembers();
        bool is_pytables = false;
        for (int i = 0; i < n_members; ++i) {
            std::string name = dtype.getMemberName(i);
            if (name.find("values_block") != std::string::npos) {
                is_pytables = true;
                break;
            }
        }
        
        if (is_pytables) {
            // PyTables格式：读取列映射并解码
            auto column_map = readPyTablesColumns(dataset);
            if (column_map.empty()) {
                std::cerr << "[WARN] PyTables trade table has no column mapping" << std::endl;
                return trades;
            }
            
            // 从compound type读取数据 (与order相同的方式)
            H5::CompType ctype = dataset.getCompType();
            size_t record_size = ctype.getSize();
            
            // 定位各block成员
            int block0_midx = -1, block1_midx = -1, block2_midx = -1;
            for (int i = 0; i < ctype.getNmembers(); i++) {
                std::string name = ctype.getMemberName(i);
                if (name == "values_block_0") block0_midx = i;
                else if (name == "values_block_1") block1_midx = i;
                else if (name == "values_block_2") block2_midx = i;
            }
            
            size_t off_block0 = 0, off_block1 = 0, off_block2 = 0;
            size_t n_float_cols = 0, n_int_cols = 0, n_str_cols = 0;
            size_t str_item_size = 0;
            
            if (block0_midx >= 0) {
                off_block0 = ctype.getMemberOffset(block0_midx);
                H5::DataType mt = ctype.getMemberDataType(block0_midx);
                if (mt.getClass() == H5T_ARRAY) {
                    H5::ArrayType at(mt.getId());
                    hsize_t arr_dims[1];
                    at.getArrayDims(arr_dims);
                    n_float_cols = arr_dims[0];
                }
            }
            if (block1_midx >= 0) {
                off_block1 = ctype.getMemberOffset(block1_midx);
                H5::DataType mt = ctype.getMemberDataType(block1_midx);
                if (mt.getClass() == H5T_ARRAY) {
                    H5::ArrayType at(mt.getId());
                    hsize_t arr_dims[1];
                    at.getArrayDims(arr_dims);
                    n_int_cols = arr_dims[0];
                }
            }
            if (block2_midx >= 0) {
                off_block2 = ctype.getMemberOffset(block2_midx);
                H5::DataType mt = ctype.getMemberDataType(block2_midx);
                if (mt.getClass() == H5T_ARRAY) {
                    H5::ArrayType at(mt.getId());
                    hsize_t arr_dims[1];
                    at.getArrayDims(arr_dims);
                    n_str_cols = arr_dims[0];
                    H5::DataType base = at.getSuper();
                    str_item_size = base.getSize();
                }
            }
            
            size_t int_offset = n_float_cols;
            size_t str_offset = n_float_cols + n_int_cols;
            
            // 获取字段索引
            auto get_col_idx = [&](const std::string& name) -> int {
                auto it = column_map.find(name);
                return (it != column_map.end()) ? it->second : -1;
            };
            
            int idx_sysid = get_col_idx("sysid");
            int idx_trans_ss = get_col_idx("trans_ss");
            int idx_direction = get_col_idx("direction");
            if (idx_direction < 0) {
                idx_direction = get_col_idx("act");
            }
            
            // 检查压缩过滤器
            H5::DSetCreatPropList trade_plist = dataset.getCreatePlist();
            int trade_n_filters = trade_plist.getNfilters();
            if (trade_n_filters > 0) {
                for (int fi = 0; fi < trade_n_filters; fi++) {
                    size_t cd_nelmts = 0;
                    unsigned int flags = 0;
                    unsigned int filter_config = 0;
                    char filter_name[256] = {0};
                    H5Z_filter_t filter_id = trade_plist.getFilter(fi, flags, cd_nelmts, nullptr, 256, filter_name, filter_config);
                    htri_t avail = H5Zfilter_avail(filter_id);
                    if (avail <= 0) {
                        std::cerr << "[ERROR] Trade: compression filter '" << filter_name 
                                  << "' (id=" << filter_id << ") is NOT available!" << std::endl;
                        return trades;
                    }
                }
            }
            
            // 读取全部数据
            std::vector<char> buffer(n_records * record_size);
            dataset.read(buffer.data(), ctype);
            
            // 辅助函数
            auto get_numeric = [&](const char* rec, int col_idx) -> double {
                if (col_idx < 0) return 0.0;
                if (col_idx < static_cast<int>(int_offset)) {
                    if (block0_midx < 0) return 0.0;
                    const double* floats = reinterpret_cast<const double*>(rec + off_block0);
                    return floats[col_idx];
                }
                if (col_idx < static_cast<int>(str_offset)) {
                    if (block1_midx < 0) return 0.0;
                    const int64_t* ints = reinterpret_cast<const int64_t*>(rec + off_block1);
                    return static_cast<double>(ints[col_idx - int_offset]);
                }
                return 0.0;
            };
            
            auto get_string = [&](const char* rec, int col_idx) -> std::string {
                if (col_idx < static_cast<int>(str_offset) || block2_midx < 0 || str_item_size == 0) return "";
                int block_col = col_idx - str_offset;
                if (block_col < 0 || block_col >= static_cast<int>(n_str_cols)) return "";
                const char* str_base = rec + off_block2 + block_col * str_item_size;
                std::string s(str_base, str_item_size);
                size_t pos = s.find('\0');
                if (pos != std::string::npos) s.resize(pos);
                return s;
            };
            
            // 解析数据
            for (size_t i = 0; i < n_records; ++i) {
                const char* rec = buffer.data() + i * record_size;
                TradeRecord trade;
                
                trade.sysid = static_cast<int64_t>(get_numeric(rec, idx_sysid));
                trade.trans_ss = get_numeric(rec, idx_trans_ss);
                
                // 过滤掉 trans_ss 为 NaN 的记录 (与Python一致)
                if (std::isnan(trade.trans_ss)) {
                    continue;
                }
                
                // 始终从 trans_ss 重算 time_slot (3秒桶)
                trade.time_slot = static_cast<int>(std::floor(trade.trans_ss / 3.0)) * 3;
                
                // direction可能在string block或不存在 (trade表可能没有string block)
                if (idx_direction >= 0) {
                    if (idx_direction >= static_cast<int>(str_offset)) {
                        trade.direction = get_string(rec, idx_direction);
                    }
                }
                
                trades.push_back(trade);
            }
            
            std::cout << "[INFO] Successfully decoded PyTables trade format: " << trades.size() << " records" << std::endl;
            
        } else {
            // 标准HDF5格式
            int idx_sysid = -1, idx_trans_ss = -1, idx_time_slot = -1, idx_direction = -1;
            
            for (int i = 0; i < n_members; ++i) {
                std::string name = dtype.getMemberName(i);
                if (name == "sysid") idx_sysid = i;
                else if (name == "trans_ss") idx_trans_ss = i;
                else if (name == "time_slot") idx_time_slot = i;
                else if (name == "direction" || name == "act") idx_direction = i;
            }
            
            // 检查必要字段
            if (idx_sysid < 0) {
                std::cerr << "[WARN] Trade table missing 'sysid' field" << std::endl;
                return trades;
            }
            
            // 读取数据
            std::vector<char> buffer(n_records * dtype.getSize());
            dataset.read(buffer.data(), dtype);
            
            for (size_t i = 0; i < n_records; ++i) {
                TradeRecord trade;
                char* record = buffer.data() + i * dtype.getSize();
                
                if (idx_sysid >= 0) {
                    size_t offset = dtype.getMemberOffset(idx_sysid);
                    H5::DataType member_type = dtype.getMemberDataType(idx_sysid);
                    if (member_type.getClass() == H5T_INTEGER) {
                        trade.sysid = *reinterpret_cast<int64_t*>(record + offset);
                    }
                }
                
                if (idx_trans_ss >= 0) {
                    size_t offset = dtype.getMemberOffset(idx_trans_ss);
                    H5::DataType member_type = dtype.getMemberDataType(idx_trans_ss);
                    if (member_type.getClass() == H5T_FLOAT) {
                        trade.trans_ss = *reinterpret_cast<double*>(record + offset);
                    }
                }
                
                // 过滤掉 trans_ss 为 NaN 的记录 (与Python一致)
                if (std::isnan(trade.trans_ss)) {
                    continue;
                }
                
                // 始终从 trans_ss 重算 time_slot (3秒桶)
                trade.time_slot = static_cast<int>(std::floor(trade.trans_ss / 3.0)) * 3;
                
                if (idx_direction >= 0) {
                    size_t offset = dtype.getMemberOffset(idx_direction);
                    H5::DataType member_type = dtype.getMemberDataType(idx_direction);
                    if (member_type.getClass() == H5T_STRING) {
                        H5::StrType strtype(member_type.getId());
                        size_t str_size = strtype.getSize();
                        if (strtype.isVariableStr()) {
                            char* str_ptr = *reinterpret_cast<char**>(record + offset);
                            if (str_ptr) {
                                trade.direction = std::string(str_ptr);
                            }
                        } else {
                            trade.direction = std::string(record + offset, str_size);
                            trade.direction.erase(std::find(trade.direction.begin(), trade.direction.end(), '\0'), trade.direction.end());
                        }
                    }
                }
                
                trades.push_back(trade);
            }
        }
    }
    catch (H5::Exception& e) {
        std::cerr << "[WARN] Failed to read trade data: " << e.getDetailMsg() << std::endl;
        // trade数据可选，忽略读取失败
    }
    catch (...) {
        // 忽略所有其他异常
    }
    
    return trades;
}

// ============================================================
// TickAggregator实现
// ============================================================

TickAggregator::TickAggregator(
    const std::vector<OrderRecord>& orders,
    const std::vector<TradeRecord>& trades,
    const std::string& stock_code,
    const std::time_t& trade_date,
    const std::unordered_map<int64_t, std::string>& order_type_map)
    : order_df_(orders), trade_df_(trades), stock_code_(stock_code),
      trade_date_(trade_date), order_type_map_(order_type_map) {
    
    // 初始化盘口数据
    last_lob_bid_price_.fill(0.0);
    last_lob_bid_volume_.fill(0);
    last_lob_ask_price_.fill(0.0);
    last_lob_ask_volume_.fill(0);
    
    // 初始化市场数据
    last_price_ = 0.0;
    last_vwap_ = 0.0;
    last_total_volume_ = 0;
    last_total_amount_ = 0.0;
    day_high_ = 0.0;
    day_low_ = std::numeric_limits<double>::infinity();
    cumulative_amount_ = 0.0;
    auction_has_clearing_ = false;
    auction_clearing_intp_ = 0;
    auction_res_best_bid_intp_ = 0;
    auction_res_best_ask_intp_ = 0;

    // LOB H5 行内十档是 pre-event 口径(jsy_lob/TL/Flow 基线)。
    // 3s Snapshot 需要截至桶末事件处理后的盘口，因此第 i 行 post 盘口
    // 使用第 i+1 行 pre-event 盘口；最后一行无后继时退回自身盘口。
    for (size_t i = 0; i < order_df_.size(); ++i) {
        const OrderRecord& next = (i + 1 < order_df_.size()) ? order_df_[i + 1] : order_df_[i];
        order_df_[i].post_bdp = next.bdp;
        order_df_[i].post_bdv = next.bdv;
        order_df_[i].post_akp = next.akp;
        order_df_[i].post_akv = next.akv;
        order_df_[i].post_bid_num = next.bid_num;
        order_df_[i].post_ask_num = next.ask_num;
    }
}

std::vector<AggregatedData> TickAggregator::aggregateTo3s() {
    std::vector<AggregatedData> results;
    
    // 生成3秒时间槽
    std::vector<int> time_slots;
    
    // 3秒槽改为「左闭右开 [s, s+3)」, 按左端标号 (与实盘流式合成一致)
    // 每段 bar = start, start+3, ..., end-3
    // 集合竞价时段
    for (int t = Constants::CALL_AUCTION_MORNING_START;
         t < Constants::CALL_AUCTION_MORNING_END; t += 3) {
        time_slots.push_back(t);
    }
    for (int t = Constants::CALL_AUCTION_CLOSE_START;
         t < Constants::CALL_AUCTION_CLOSE_END; t += 3) {
        time_slots.push_back(t);
    }

    // 连续竞价时段
    for (int t = Constants::CONTINUOUS_MORNING_START;
         t < Constants::CONTINUOUS_MORNING_END; t += 3) {
        time_slots.push_back(t);
    }
    for (int t = Constants::CONTINUOUS_AFTERNOON_START;
         t < Constants::CONTINUOUS_AFTERNOON_END; t += 3) {
        time_slots.push_back(t);
    }

    std::sort(time_slots.begin(), time_slots.end());
    
    // ts = floor(t/3)*3 已是「[ts, ts+3) 区间左端」(3的倍数)。
    // 左闭右开下: ts 落在某交易段 [start, end) 内即为有效 bar, 直接返回 ts。
    auto mapTo3sSlot = [](int ts) -> int {
        const std::array<std::pair<int, int>, 4> periods = {{
            {Constants::CALL_AUCTION_MORNING_START, Constants::CALL_AUCTION_MORNING_END},
            {Constants::CALL_AUCTION_CLOSE_START, Constants::CALL_AUCTION_CLOSE_END},
            {Constants::CONTINUOUS_MORNING_START, Constants::CONTINUOUS_MORNING_END},
            {Constants::CONTINUOUS_AFTERNOON_START, Constants::CONTINUOUS_AFTERNOON_END},
        }};

        for (const auto& [start, end] : periods) {
            if (start <= ts && ts < end) {
                return ts;  // 已是左端, 直接作为 bar 标号
            }
        }

        // 边界/空档兜底: 集合竞价撮合成交的时间戳常带毫秒(开盘 09:25:00.480 ->
        // floor 33900, 收盘 15:00:00.980 -> floor 54000), 落在交易段结束后的空档,
        // 归到该段「最后一个有效 bar = end-3」, 避免整批成交被丢(totalAmount 少算)。
        // 开盘集合竞价撮合: [09:25:00, 09:30:00) -> 最后一个竞价 bar
        if (Constants::CALL_AUCTION_MORNING_END <= ts &&
            ts < Constants::CONTINUOUS_MORNING_START) {
            return Constants::CALL_AUCTION_MORNING_END - 3;
        }
        // 上午收市残留: [11:30:00, 13:00:00) -> 上午最后一个 bar
        if (Constants::CONTINUOUS_MORNING_END <= ts &&
            ts < Constants::CONTINUOUS_AFTERNOON_START) {
            return Constants::CONTINUOUS_MORNING_END - 3;
        }
        // 收盘集合竞价撮合: >= 15:00:00 -> 最后一个收盘竞价 bar
        if (ts >= Constants::CALL_AUCTION_CLOSE_END) {
            return Constants::CALL_AUCTION_CLOSE_END - 3;
        }
        return -1;
    };
    
    // 预分组: (time_slot, ordertype, direction) -> vector<OrderRecord>
    OrderPregroup order_pregroup;
    
    for (const auto& order : order_df_) {
        int ts = mapTo3sSlot(order.time_slot);

        if (ts > 0) {
            order_pregroup[{ts, order.ordertype, order.direction}].push_back(order);
            order_pregroup[{ts, order.ordertype, "ALL"}].push_back(order);
            order_pregroup[{ts, "ALL", "ALL"}].push_back(order);
        }
    }
    
    // 预分组trade数据
    TradeGroups trade_groups;
    for (const auto& trade : trade_df_) {
        int ts = mapTo3sSlot(trade.time_slot);
        if (ts > 0) {
            trade_groups[ts].push_back(trade);
        }
    }
    
    // 逐3秒聚合
    results.reserve(time_slots.size());
    for (int time_sec : time_slots) {
        auto sec_trade = trade_groups[time_sec];
        auto data = aggregateOneSecond(time_sec, order_pregroup, sec_trade);
        results.push_back(std::move(data));  // 使用move避免拷贝
    }
    
    return results;
}

AggregatedData TickAggregator::aggregateOneSecond(
    int time_sec,
    const OrderPregroup& order_pregroup,
    const std::vector<TradeRecord>& /* sec_trade */) {
    
    AggregatedData data;
    
    // 计算17位dateTime: YYYYMMDDHHMMSSMMM
    //
    // 防未来信息泄漏: 分桶为左闭右开 [s, s+3), 桶内聚合(含末档盘口快照、成交统计)
    // 直到 s+3⁻ 才完整可知。离线落盘的唯一时间基准是该标号, 若仍标在左端 s, 下游
    // 按 dateTime 对齐时会把 [s, s+3) 的全部信息当成 s 时刻已可用 -> 提前 3 秒看到
    // 盘口/成交。因此输出标号必须用「可用时刻 = 右端 s+3」(下一个时刻)。
    // 内部分桶/分组/竞价逻辑仍按左端 time_sec, 仅输出标号 +3。
    const int label_sec = time_sec + 3;
    std::tm* tm_date = std::localtime(&trade_date_);
    int year = tm_date->tm_year + 1900;
    int month = tm_date->tm_mon + 1;
    int day = tm_date->tm_mday;
    int h = label_sec / 3600;
    int m = (label_sec % 3600) / 60;
    int s = label_sec % 60;

    data.tradedate = trade_date_;
    data.instrumentID = getInstrumentId();
    data.symbol = stock_code_;
    data.event_time = secondsToDateTime(label_sec);
    data.dateTime = static_cast<int64_t>(year) * 10000000000000LL +
                    static_cast<int64_t>(month) * 100000000000LL +
                    static_cast<int64_t>(day) * 1000000000LL +
                    static_cast<int64_t>(h) * 10000000LL +
                    static_cast<int64_t>(m) * 100000LL +
                    static_cast<int64_t>(s) * 1000LL;
    
    // 检查是否有数据
    auto it = order_pregroup.find({time_sec, "ALL", "ALL"});
    int event_count = (it != order_pregroup.end()) ? it->second.size() : 0;
    
    if (event_count == 0) {
        // 空数据处理
        data.bidPrice = last_lob_bid_price_;
        data.bidVolume = last_lob_bid_volume_;
        data.askPrice = last_lob_ask_price_;
        data.askVolume = last_lob_ask_volume_;

        // 计算pending统计
        calcPendingStats(data, time_sec, order_pregroup);

        data.eventCount = 0;
        data.dataFlag = isCallAuction(time_sec) ? 1 : 2;

        // 填充市场数据
        data.totalVolume = last_total_volume_;
        data.totalAmount = last_total_amount_;
        data.lastPrice = last_price_;
        data.dayHigh = (day_high_ > 0) ? day_high_ : 0.0;
        data.dayLow = (day_low_ != std::numeric_limits<double>::infinity()) ? day_low_ : 0.0;
        data.vwap = last_vwap_;

        return data;
    }
    
    // 计算各项统计
    // 注: calcPendingStats 须在 calcTypeStats 之前 —— 竞价段 fillAuctionPending 会算出
    //     清算价并缓存, calcTypeStats 据此对竞价 type 重分类。
    calcBasicStats(data, time_sec, order_pregroup);
    calcPositionStats(data, time_sec, order_pregroup);
    calcLevelRangeStats(data, time_sec, order_pregroup);
    calcCancelLevelStats(data, time_sec, order_pregroup);
    getLobSnapshot(data, time_sec, order_pregroup);
    calcPendingStats(data, time_sec, order_pregroup);
    calcTypeStats(data, time_sec, order_pregroup);
    calcMarketSummary(data, time_sec, order_pregroup);
    
    data.eventCount = event_count;
    data.dataFlag = isCallAuction(time_sec) ? 1 : 0;

    return data;
}

void TickAggregator::calcBasicStats(AggregatedData& data, int time_sec, 
                                    const OrderPregroup& order_pregroup) {
    
    // 定义组合
    struct Combo {
        std::string ordertype;
        std::string direction;
        std::string side;
    };
    
    std::vector<Combo> combos = {
        {"wt", "B", "Bid"}, {"wt", "S", "Ask"},
        {"zb", "B", "Bid"}, {"zb", "S", "Ask"},
        {"cl", "B", "Bid"}, {"cl", "S", "Ask"}
    };
    
    for (const auto& combo : combos) {
        auto it = order_pregroup.find({time_sec, combo.ordertype, combo.direction});
        
        if (it == order_pregroup.end() || it->second.empty()) {
            continue;
        }
        
        const auto& records = it->second;
        int num = records.size();
        int64_t vol = 0;
        double amt = 0.0;
        std::vector<double> valid_prices;      // 有效价格 (price > 0)
        std::vector<int64_t> valid_volumes;    // 有效成交量 (volume > 0)
        
        for (const auto& rec : records) {
            vol += rec.volume;
            amt += rec.amount;
            if (rec.price > 0 && !std::isnan(rec.price)) {
                valid_prices.push_back(rec.price);
            }
            if (rec.volume > 0) {
                valid_volumes.push_back(rec.volume);
            }
        }
        
        double avg_price = safeMean(valid_prices);
        // Python safe_last: 分别取最后一个有效price和最后一个有效volume
        double last_price = valid_prices.empty() ? 0.0 : valid_prices.back();
        int64_t last_vol = valid_volumes.empty() ? 0 : valid_volumes.back();
        
        // 根据ordertype和side设置对应字段
        if (combo.ordertype == "wt") {
            if (combo.side == "Bid") {
                data.wtBidNum = num;
                data.wtBidVol = vol;
                data.wtBidAmt = amt;
                data.wtBidp = avg_price;
                data.wtBidp1 = last_price;
                data.wtBidv1 = last_vol;
            } else {
                data.wtAskNum = num;
                data.wtAskVol = vol;
                data.wtAskAmt = amt;
                data.wtAskp = avg_price;
                data.wtAskp1 = last_price;
                data.wtAskv1 = last_vol;
            }
        } else if (combo.ordertype == "zb") {
            if (combo.side == "Bid") {
                data.zbBidNum = num;
                data.zbBidVol = vol;
                data.zbBidAmt = amt;
                data.zbBidp = avg_price;
                data.zbBidp1 = last_price;
                data.zbBidv1 = last_vol;
            } else {
                data.zbAskNum = num;
                data.zbAskVol = vol;
                data.zbAskAmt = amt;
                data.zbAskp = avg_price;
                data.zbAskp1 = last_price;
                data.zbAskv1 = last_vol;
            }
        } else if (combo.ordertype == "cl") {
            if (combo.side == "Bid") {
                data.clBidNum = num;
                data.clBidVol = vol;
                data.clBidAmt = amt;
                data.clBidp = avg_price;
            } else {
                data.clAskNum = num;
                data.clAskVol = vol;
                data.clAskAmt = amt;
                data.clAskp = avg_price;
            }
        }
    }
    
    // 量加权价
    auto wt_bid_it = order_pregroup.find({time_sec, "wt", "B"});
    if (wt_bid_it != order_pregroup.end() && !wt_bid_it->second.empty()) {
        std::vector<double> prices;
        std::vector<int64_t> volumes;
        for (const auto& rec : wt_bid_it->second) {
            if (rec.price > 0 && !std::isnan(rec.price)) {
                prices.push_back(rec.price);
                volumes.push_back(rec.volume);
            }
        }
        data.wtBidWeightPrice = safeWeightedPrice(prices, volumes);
    }
    
    auto wt_ask_it = order_pregroup.find({time_sec, "wt", "S"});
    if (wt_ask_it != order_pregroup.end() && !wt_ask_it->second.empty()) {
        std::vector<double> prices;
        std::vector<int64_t> volumes;
        for (const auto& rec : wt_ask_it->second) {
            if (rec.price > 0 && !std::isnan(rec.price)) {
                prices.push_back(rec.price);
                volumes.push_back(rec.volume);
            }
        }
        data.wtAskWeightPrice = safeWeightedPrice(prices, volumes);
    }
    
    // 总计（不分买卖）
    for (const std::string& ordertype : {"wt", "zb", "cl"}) {
        auto it = order_pregroup.find({time_sec, ordertype, "ALL"});
        if (it != order_pregroup.end() && !it->second.empty()) {
            int64_t vol = 0;
            int num = it->second.size();
            for (const auto& rec : it->second) {
                vol += rec.volume;
            }
            
            if (ordertype == "wt") {
                data.wtVol = vol;
                data.wtNum = num;
            } else if (ordertype == "zb") {
                data.zbVol = vol;
                data.zbNum = num;
            } else if (ordertype == "cl") {
                data.clVol = vol;
                data.clNum = num;
            }
        }
    }
}

void TickAggregator::calcPositionStats(AggregatedData& data, int time_sec,
                                       const OrderPregroup& order_pregroup) {
    
    // 买方和卖方分别统计
    struct SidePair {
        std::string direction;
        std::string side;
    };
    
    std::vector<SidePair> sides = {{"B", "Bid"}, {"S", "Ask"}};
    
    for (const auto& sp : sides) {
        auto it = order_pregroup.find({time_sec, "wt", sp.direction});
        if (it == order_pregroup.end() || it->second.empty()) {
            continue;
        }
        
        const auto& records = it->second;
        int active_num = 0, passive_num = 0, instant_num = 0;
        int active_num_for_avg = 0;  // 含 SH position=0 type1-3, 用于 avg_pos 分子
        int64_t active_vol = 0, passive_vol = 0, instant_vol = 0;
        double active_pos_sum = 0.0, passive_pos_sum = 0.0;
        double total_abs_pos_sum = 0.0;

        for (const auto& rec : records) {
            int64_t vol = rec.volume;
            int abs_pos = std::abs(rec.position);
            total_abs_pos_sum += abs_pos;

            // wtP* 表示进入己方队列的被动委托, 必须按 position<0 统计,
            // 不随 type 重分类变化; wtA* 仅保留为非排队激进单诊断字段。
            // SH 派生单 position==0 (ask/bid position 均为0), 不计入 active。
            if (rec.position < 0) {  // 排队单 (passive)
                passive_num++;
                passive_vol += vol;
                passive_pos_sum += abs_pos;
            } else if (rec.type >= 1 && rec.type <= 3) {  // position>=0 type1-3
                active_num_for_avg++;  // 均贡献 1 到 avg_pos
                active_num++;
                active_vol += vol;
                active_pos_sum += abs_pos;
            } else {  // type==0: 盘口为空/未分类
                instant_num++;
                instant_vol += vol;
            }
        }
        
        int total_num = records.size();
        int64_t total_vol = active_vol + passive_vol + instant_vol;
        
        double num_ratio = (total_num > 0) ? static_cast<double>(active_num) / total_num : 0.0;
        double vol_ratio = (total_vol > 0) ? static_cast<double>(active_vol) / total_vol : 0.0;
        // ref 把所有 type1-3 的 abs_pos 视为 max(1, actual): position=0 贡献1, position!=0 贡献 abs(position)
        double avg_pos = (total_num > 0) ? (total_abs_pos_sum + active_num_for_avg) / total_num : 0.0;
        double passive_avg_pos = (passive_num > 0) ? passive_pos_sum / passive_num : 0.0;
        
        if (sp.side == "Bid") {
            data.wtBidNumRatio = num_ratio;
            data.wtBidVolRatio = vol_ratio;
            data.wtBidAvgPos = avg_pos;
            data.wtABidVol = active_vol;
            data.wtABidNum = active_num;
            data.wtPBidPos = passive_avg_pos;
            data.wtPBidVol = passive_vol;
            data.wtPBidNum = passive_num;
        } else {
            data.wtAskNumRatio = num_ratio;
            data.wtAskVolRatio = vol_ratio;
            data.wtAskAvgPos = avg_pos;
            data.wtAAskVol = active_vol;
            data.wtAAskNum = active_num;
            data.wtPAskPos = passive_avg_pos;
            data.wtPAskVol = passive_vol;
            data.wtPAskNum = passive_num;
        }
    }
    
    // 最大档位统计（挂单总量最大的档位）
    auto wt_all_it = order_pregroup.find({time_sec, "wt", "ALL"});
    if (wt_all_it != order_pregroup.end() && !wt_all_it->second.empty()) {
        const auto& wt_records = wt_all_it->second;
        
        for (const std::string& dir : {"B", "S"}) {
            std::string side = (dir == "B") ? "bid" : "ask";
            
            std::map<int, int64_t> level_vol;
            std::map<int, double> level_price_sum;
            std::map<int, int> level_price_count;
            
            for (const auto& rec : wt_records) {
                if (rec.direction == dir && rec.position < 0) {
                    int abs_pos = std::abs(rec.position);
                    level_vol[abs_pos] += rec.volume;
                    if (rec.price > 0 && !std::isnan(rec.price)) {
                        level_price_sum[abs_pos] += rec.price;
                        level_price_count[abs_pos]++;
                    }
                }
            }
            
            // 找最大
            int max_pos = 0;
            int64_t max_vol = 0;
            for (const auto& [pos, vol] : level_vol) {
                if (vol > max_vol) {
                    max_vol = vol;
                    max_pos = pos;
                }
            }
            
            double max_price = 0.0;
            if (level_price_count[max_pos] > 0) {
                max_price = level_price_sum[max_pos] / level_price_count[max_pos];
            }
            
            if (side == "bid") {
                data.bidMaxPos = std::min(65535, max_pos);  // UInt16限制
                data.bidMaxVol = max_vol;
                data.bidMaxPrice = max_price;
            } else {
                data.askMaxPos = std::min(65535, max_pos);
                data.askMaxVol = max_vol;
                data.askMaxPrice = max_price;
            }
        }
    }
}

void TickAggregator::calcLevelRangeStats(AggregatedData& data, int time_sec,
                                         const OrderPregroup& order_pregroup) {
    
    // 委托档位区间统计: 1, 2-4, 5-10, 11+
    std::vector<std::pair<std::string, std::string>> sides = {{"B", "Bid"}, {"S", "Ask"}};
    
    for (const auto& [direction, side] : sides) {
        auto it = order_pregroup.find({time_sec, "wt", direction});
        if (it == order_pregroup.end() || it->second.empty()) {
            continue;
        }
        
        const auto& records = it->second;
        
        // 分档统计 (只统计排队单 position < 0)
        int num_1 = 0, num_2_4 = 0, num_5_10 = 0, num_11inf = 0;
        int64_t vol_1 = 0, vol_2_4 = 0, vol_5_10 = 0, vol_11inf = 0;
        double amt_1 = 0.0, amt_2_4 = 0.0, amt_5_10 = 0.0, amt_11inf = 0.0;
        double price_sum_2_4 = 0.0, price_sum_5_10 = 0.0, price_sum_11inf = 0.0;
        int price_count_2_4 = 0, price_count_5_10 = 0, price_count_11inf = 0;
        
        for (const auto& rec : records) {
            // Python: passive_mask = positions < 0
            if (rec.position >= 0) continue;  // 只统计排队单 (position < 0)
            
            int abs_pos = std::abs(rec.position);
            
            if (abs_pos == 1) {
                num_1++;
                vol_1 += rec.volume;
                amt_1 += rec.amount;
            } else if (abs_pos >= 2 && abs_pos <= 4) {
                num_2_4++;
                vol_2_4 += rec.volume;
                amt_2_4 += rec.amount;
                if (rec.price > 0 && !std::isnan(rec.price)) {
                    price_sum_2_4 += rec.price;
                    price_count_2_4++;
                }
            } else if (abs_pos >= 5 && abs_pos <= 10) {
                num_5_10++;
                vol_5_10 += rec.volume;
                amt_5_10 += rec.amount;
                if (rec.price > 0 && !std::isnan(rec.price)) {
                    price_sum_5_10 += rec.price;
                    price_count_5_10++;
                }
            } else {  // >= 11
                num_11inf++;
                vol_11inf += rec.volume;
                amt_11inf += rec.amount;
                if (rec.price > 0 && !std::isnan(rec.price)) {
                    price_sum_11inf += rec.price;
                    price_count_11inf++;
                }
            }
        }
        
        double avg_price_2_4 = (price_count_2_4 > 0) ? price_sum_2_4 / price_count_2_4 : 0.0;
        double avg_price_5_10 = (price_count_5_10 > 0) ? price_sum_5_10 / price_count_5_10 : 0.0;
        double avg_price_11inf = (price_count_11inf > 0) ? price_sum_11inf / price_count_11inf : 0.0;
        
        if (side == "Bid") {
            data.wtBid1Num = num_1;
            data.wtBid1Vol = vol_1;
            data.wtBid1Amt = amt_1;
            data.wtBid2_4Num = num_2_4;
            data.wtBid2_4Vol = vol_2_4;
            data.wtBid2_4Amt = amt_2_4;
            data.wtBid2_4AvgPrice = avg_price_2_4;
            data.wtBid5_10Num = num_5_10;
            data.wtBid5_10Vol = vol_5_10;
            data.wtBid5_10Amt = amt_5_10;
            data.wtBid5_10AvgPrice = avg_price_5_10;
            data.wtBid11InfNum = num_11inf;
            data.wtBid11InfVol = vol_11inf;
            data.wtBid11InfAmt = amt_11inf;
            data.wtBid11InfAvgPrice = avg_price_11inf;
        } else {
            data.wtAsk1Num = num_1;
            data.wtAsk1Vol = vol_1;
            data.wtAsk1Amt = amt_1;
            data.wtAsk2_4Num = num_2_4;
            data.wtAsk2_4Vol = vol_2_4;
            data.wtAsk2_4Amt = amt_2_4;
            data.wtAsk2_4AvgPrice = avg_price_2_4;
            data.wtAsk5_10Num = num_5_10;
            data.wtAsk5_10Vol = vol_5_10;
            data.wtAsk5_10Amt = amt_5_10;
            data.wtAsk5_10AvgPrice = avg_price_5_10;
            data.wtAsk11InfNum = num_11inf;
            data.wtAsk11InfVol = vol_11inf;
            data.wtAsk11InfAmt = amt_11inf;
            data.wtAsk11InfAvgPrice = avg_price_11inf;
        }
    }
}

void TickAggregator::calcCancelLevelStats(AggregatedData& data, int time_sec,
                                          const OrderPregroup& order_pregroup) {
    
    // 撤单档位统计: 1, 2-4, 5-10 (无11+)
    std::vector<std::pair<std::string, std::string>> sides = {{"B", "Bid"}, {"S", "Ask"}};
    
    for (const auto& [direction, side] : sides) {
        auto it = order_pregroup.find({time_sec, "cl", direction});
        if (it == order_pregroup.end() || it->second.empty()) {
            continue;
        }
        
        const auto& records = it->second;
        
        // 需要根据撤单价格与盘口价格匹配来确定档位
        // 简化实现：假设position字段已包含档位信息
        int num_1 = 0, num_2_4 = 0, num_5_10 = 0;
        int64_t vol_1 = 0, vol_2_4 = 0, vol_5_10 = 0;
        double amt_1 = 0.0, amt_2_4 = 0.0, amt_5_10 = 0.0;
        double price_sum_2_4 = 0.0, price_sum_5_10 = 0.0;
        int price_count_2_4 = 0, price_count_5_10 = 0;
        
        for (const auto& rec : records) {
            // 根据价格匹配档位
            int matched_level = 0;
            if (direction == "B") {
                for (int i = 0; i < Constants::LOB_LEVELS; ++i) {
                    if (rec.bdp[i] > 0 && std::abs(rec.price - rec.bdp[i]) < 1e-6) {
                        matched_level = i + 1;
                        break;
                    }
                }
            } else {
                for (int i = 0; i < Constants::LOB_LEVELS; ++i) {
                    if (rec.akp[i] > 0 && std::abs(rec.price - rec.akp[i]) < 1e-6) {
                        matched_level = i + 1;
                        break;
                    }
                }
            }
            
            if (matched_level == 1) {
                num_1++;
                vol_1 += rec.volume;
                amt_1 += rec.amount;
            } else if (matched_level >= 2 && matched_level <= 4) {
                num_2_4++;
                vol_2_4 += rec.volume;
                amt_2_4 += rec.amount;
                if (rec.price > 0 && !std::isnan(rec.price)) {
                    price_sum_2_4 += rec.price;
                    price_count_2_4++;
                }
            } else if (matched_level >= 5 && matched_level <= 10) {
                num_5_10++;
                vol_5_10 += rec.volume;
                amt_5_10 += rec.amount;
                if (rec.price > 0 && !std::isnan(rec.price)) {
                    price_sum_5_10 += rec.price;
                    price_count_5_10++;
                }
            }
        }
        
        double avg_price_2_4 = (price_count_2_4 > 0) ? price_sum_2_4 / price_count_2_4 : 0.0;
        double avg_price_5_10 = (price_count_5_10 > 0) ? price_sum_5_10 / price_count_5_10 : 0.0;
        
        if (side == "Bid") {
            data.clBid1Num = num_1;
            data.clBid1Vol = vol_1;
            data.clBid1Amt = amt_1;
            data.clBid2_4Num = num_2_4;
            data.clBid2_4Vol = vol_2_4;
            data.clBid2_4Amt = amt_2_4;
            data.clBid2_4AvgPrice = avg_price_2_4;
            data.clBid5_10Num = num_5_10;
            data.clBid5_10Vol = vol_5_10;
            data.clBid5_10Amt = amt_5_10;
            data.clBid5_10AvgPrice = avg_price_5_10;
        } else {
            data.clAsk1Num = num_1;
            data.clAsk1Vol = vol_1;
            data.clAsk1Amt = amt_1;
            data.clAsk2_4Num = num_2_4;
            data.clAsk2_4Vol = vol_2_4;
            data.clAsk2_4Amt = amt_2_4;
            data.clAsk2_4AvgPrice = avg_price_2_4;
            data.clAsk5_10Num = num_5_10;
            data.clAsk5_10Vol = vol_5_10;
            data.clAsk5_10Amt = amt_5_10;
            data.clAsk5_10AvgPrice = avg_price_5_10;
        }
    }
}

void TickAggregator::calcTypeStats(AggregatedData& data, int time_sec,
                                   const OrderPregroup& order_pregroup) {
    
    // Type分类统计 (1-6)
    // Type3: 市价单 (Level2 OrderType='1')
    // Type5: 己方最优 (Level2 OrderType='U')
    // 连续段其他按LOB type字段统计; 竞价段按清算价重分类(见下)
    const bool in_auction = isCallAuction(time_sec);

    std::vector<std::pair<std::string, std::string>> sides = {{"B", "bid"}, {"S", "ask"}};
    
    for (const auto& [direction, side] : sides) {
        auto it = order_pregroup.find({time_sec, "wt", direction});
        if (it == order_pregroup.end() || it->second.empty()) {
            continue;
        }
        
        const auto& records = it->second;
        
        std::array<int64_t, 6> type_vols = {0};
        std::array<int, 6> type_nums = {0};
        
        for (const auto& rec : records) {
            // 获取Level2 OrderType
            auto type_it = order_type_map_.find(rec.sysid);
            std::string level2_type;
            if (type_it != order_type_map_.end()) {
                level2_type = type_it->second;
                std::transform(level2_type.begin(), level2_type.end(), 
                              level2_type.begin(), ::toupper);
            }
            
            int type_idx = -1;
            
            if (level2_type == "1") {
                // 市价单 -> Type3
                type_idx = 2;
            } else if (level2_type == "U") {
                // 己方最优 -> Type5
                type_idx = 4;
            } else if (in_auction) {
                // 竞价段: lob 的 rec.type 基于交叉盘口(买价>>卖价)算出, 几乎全被误判为
                // 激进(1/2/3)。改按虚拟撮合清算价重分类(口径同 pending 残余簿):
                //   跨清算价 -> 激进(Type1); 等于清算价 -> Type2;
                //   未跨(被动) -> 残余簿最优档=Type5, 更深=Type6。
                int64_t ip = static_cast<int64_t>(std::llround(rec.price * 100.0));
                if (direction == "B") {
                    if (auction_has_clearing_ && ip > auction_clearing_intp_)       type_idx = 0;
                    else if (auction_has_clearing_ && ip == auction_clearing_intp_) type_idx = 1;
                    else type_idx = (ip == auction_res_best_bid_intp_) ? 4 : 5;
                } else {
                    if (auction_has_clearing_ && ip < auction_clearing_intp_)       type_idx = 0;
                    else if (auction_has_clearing_ && ip == auction_clearing_intp_) type_idx = 1;
                    else type_idx = (ip == auction_res_best_ask_intp_) ? 4 : 5;
                }
            } else {
                // 连续段: 按LOB type字段统计 (1-6)
                if (rec.type >= 1 && rec.type <= 6) {
                    type_idx = rec.type - 1;
                }
            }
            
            if (type_idx >= 0 && type_idx < 6) {
                type_vols[type_idx] += rec.volume;
                type_nums[type_idx]++;
            }
        }
        
        if (side == "bid") {
            data.bidType1Vol = type_vols[0];
            data.bidType1Num = type_nums[0];
            data.bidType2Vol = type_vols[1];
            data.bidType2Num = type_nums[1];
            data.bidType3Vol = type_vols[2];
            data.bidType3Num = type_nums[2];
            data.bidType4Vol = type_vols[3];
            data.bidType4Num = type_nums[3];
            data.bidType5Vol = type_vols[4];
            data.bidType5Num = type_nums[4];
            data.bidType6Vol = type_vols[5];
            data.bidType6Num = type_nums[5];
        } else {
            data.askType1Vol = type_vols[0];
            data.askType1Num = type_nums[0];
            data.askType2Vol = type_vols[1];
            data.askType2Num = type_nums[1];
            data.askType3Vol = type_vols[2];
            data.askType3Num = type_nums[2];
            data.askType4Vol = type_vols[3];
            data.askType4Num = type_nums[3];
            data.askType5Vol = type_vols[4];
            data.askType5Num = type_nums[4];
            data.askType6Vol = type_vols[5];
            data.askType6Num = type_nums[5];
        }
    }
}

void TickAggregator::getLobSnapshot(AggregatedData& data, int time_sec,
                                    const OrderPregroup& order_pregroup) {
    
    // 获取10档盘口快照（取最后一条记录）
    auto it = order_pregroup.find({time_sec, "ALL", "ALL"});
    if (it != order_pregroup.end() && !it->second.empty()) {
        const auto& last_rec = it->second.back();
        
        for (int i = 0; i < Constants::LOB_LEVELS; ++i) {
            double bid_price = last_rec.post_bdp[i];
            int64_t bid_vol = last_rec.post_bdv[i];
            int64_t bid_n = last_rec.post_bid_num[i];
            double ask_price = last_rec.post_akp[i];
            int64_t ask_vol = last_rec.post_akv[i];
            int64_t ask_n = last_rec.post_ask_num[i];

            // 修正涨跌停异常价格 (该档清零, 笔数同步清零)
            if (ask_price >= 99999) {
                ask_price = 0.0;
                ask_vol = 0;
                ask_n = 0;
            }
            if (bid_price <= -99999) {
                bid_price = 0.0;
                bid_vol = 0;
                bid_n = 0;
            }

            data.bidPrice[i] = bid_price;
            data.bidVolume[i] = bid_vol;
            data.askPrice[i] = ask_price;
            data.askVolume[i] = ask_vol;
            last_lob_bid_num_[i] = bid_n;
            last_lob_ask_num_[i] = ask_n;
        }

        // 保存到last_lob
        last_lob_bid_price_ = data.bidPrice;
        last_lob_bid_volume_ = data.bidVolume;
        last_lob_ask_price_ = data.askPrice;
        last_lob_ask_volume_ = data.askVolume;
    } else {
        // 使用上一秒的盘口
        data.bidPrice = last_lob_bid_price_;
        data.bidVolume = last_lob_bid_volume_;
        data.askPrice = last_lob_ask_price_;
        data.askVolume = last_lob_ask_volume_;
    }
}

void TickAggregator::calcPendingStats(AggregatedData& data, int time_sec,
                                      const OrderPregroup& order_pregroup) {
    const bool in_auction = isCallAuction(time_sec);

    // 连续段 pendingTotal 直接从 FIFO 全深度簿汇总; 集合竞价段由 pending_orders_ uncross 统计。

    // ========================= 集合竞价段 =========================
    // 没有连续撮合, 所有委托均为被动挂单(position 不可靠), wt 全入池, 由
    // fillAuctionPending 对全深度池做虚拟 uncross 得到残余簿统计。
    if (in_auction) {
        // 进入收盘集合竞价(14:57)时, 用连续段守恒簿重建 pending_orders_ 作为起始簿,
        // 使收盘竞价 uncross 基于 14:57 的真实连续簿(而非开盘残余/旧的虚高池)。
        if (time_sec == Constants::CALL_AUCTION_CLOSE_START) {
            rebuildPendingFromContBook();
        }

        for (const char* dir : {"B", "S"}) {
            auto wt_it = order_pregroup.find({time_sec, "wt", dir});
            if (wt_it != order_pregroup.end()) {
                for (const auto& rec : wt_it->second) {
                    pending_orders_[rec.sysid] = PendingOrder(
                        rec.direction, rec.volume, rec.price, rec.amount, rec.position);
                }
            }
        }
        auto cl_it = order_pregroup.find({time_sec, "cl", "ALL"});
        if (cl_it != order_pregroup.end()) {
            for (const auto& rec : cl_it->second) pending_orders_.erase(rec.sysid);
        }

        // 左闭右开下竞价最后一个 bar = end-3 (撮合成交也被 mapTo3sSlot 归到此 bar)。
        bool finalize = (time_sec == Constants::CALL_AUCTION_MORNING_END - 3 ||
                         time_sec == Constants::CALL_AUCTION_CLOSE_END - 3);
        fillAuctionPending(data, finalize);
        return;
    }

    // ========================= 连续竞价段 =========================
    // 价位 + 时间(FIFO) 守恒(路径A): 不依赖 sysid。
    //   wt(排队/position<0)入簿; zb 在被动方价位按 FIFO 扣; cl 按价位定位扣。
    //   起始簿 = 开盘竞价残余(fillAuctionPending finalize 已播种到 cont_*_book_)。
    // 给出全深度 Vol/Amt 与每档笔数 Num。
    // 近似上限: 仅入"排队单", 越价单(position>=0)立即成交不挂簿, 其部分成交后的挂余
    //   极少数情况会少计(成交记录每笔仅一条、只能精确还原被动方)。
    contAddPassiveWt(time_sec, order_pregroup);
    contReduceByTrade(time_sec, order_pregroup);
    contCancelOrders(time_sec, order_pregroup);
    contUncross();   // 连续段真实盘口不应交叉; 守恒簿中若出现 bid>=ask, 按 FIFO 对撮清掉。
    contFillPending(data);
}

// 撮掉交叉量: 真实连续簿买一价 < 卖一价。若因锁价/穿价挂单导致 best_bid >= best_ask,
// 按价优先+时间(FIFO)成交对撮, 直到不再交叉。仅在最优两档间逐笔抵消。
void TickAggregator::contUncross() {
    while (!cont_bid_book_.empty() && !cont_ask_book_.empty()) {
        auto bIt = std::prev(cont_bid_book_.end());  // 最高买价
        auto aIt = cont_ask_book_.begin();           // 最低卖价
        if (bIt->first < aIt->first) break;          // 未交叉, 结束
        auto& bq = bIt->second;
        auto& aq = aIt->second;
        while (!bq.empty() && !aq.empty()) {
            int64_t m = std::min(bq.front().vol, aq.front().vol);
            bq.front().vol -= m;
            bq.front().amount = bq.front().price * static_cast<double>(bq.front().vol);
            aq.front().vol -= m;
            aq.front().amount = aq.front().price * static_cast<double>(aq.front().vol);
            if (bq.front().vol <= 0) bq.pop_front();
            if (aq.front().vol <= 0) aq.pop_front();
        }
        if (bq.empty()) cont_bid_book_.erase(bIt);
        if (aq.empty()) cont_ask_book_.erase(aIt);
    }
}

// 同价位 FIFO 扣减 qty: 从队首(最旧)开始, 整单撮掉则出队, 末单部分扣减保留。
void TickAggregator::contFifoReduce(std::map<int64_t, std::deque<RestingOrder>>& book,
                                    int64_t intp, int64_t qty) {
    auto it = book.find(intp);
    if (it == book.end()) return;
    auto& dq = it->second;
    while (qty > 0 && !dq.empty()) {
        auto& front = dq.front();
        if (front.vol > qty) {
            front.vol -= qty;
            front.amount = front.price * static_cast<double>(front.vol);
            qty = 0;
        } else {
            qty -= front.vol;
            dq.pop_front();
        }
    }
    if (dq.empty()) book.erase(it);
}

void TickAggregator::contSysidReduce(std::map<int64_t, std::deque<RestingOrder>>& book,
                                     std::unordered_map<int64_t, int64_t>& sysid_price,
                                     int64_t sysid, int64_t qty) {
    auto pit = sysid_price.find(sysid);
    if (pit == sysid_price.end()) return;  // 普通被动单未注册, 无操作
    int64_t intp = pit->second;
    auto it = book.find(intp);
    if (it == book.end()) return;
    auto& dq = it->second;
    for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
        if (dit->sysid != sysid) continue;
        dit->vol -= qty;
        if (dit->vol <= 0) {
            dq.erase(dit);
            sysid_price.erase(pit);  // 全量消耗, 清理映射
        } else {
            dit->amount = dit->price * static_cast<double>(dit->vol);
        }
        break;
    }
    if (dq.empty()) book.erase(it);
}

void TickAggregator::contAddPassiveWt(int time_sec, const OrderPregroup& order_pregroup) {
    for (const char* dir : {"B", "S"}) {
        auto wt_it = order_pregroup.find({time_sec, "wt", dir});
        if (wt_it == order_pregroup.end()) continue;
        auto& book = (std::string(dir) == "B") ? cont_bid_book_ : cont_ask_book_;
        auto& sysid_price = (std::string(dir) == "B") ? bid_sysid_price_ : ask_sysid_price_;
        bool is_buy = (std::string(dir) == "B");
        for (const auto& rec : wt_it->second) {
            // 买单 pos=0: type=2 (最优五档剩余转限价) 余量转被动限价单, 须入簿;
            //             type=3 余量 cl 配对净零, 同样正确。
            // 卖单 pos=0: 均为市价/IOC单, 不转被动挂单, 跳过 (否则会虚增卖簿)。
            // position < 0: 普通被动单, 直接入簿。
            if (is_buy ? (rec.position > 0) : (rec.position >= 0)) continue;
            if (!(rec.price > 0) || std::isnan(rec.price)) continue;
            int64_t intp = static_cast<int64_t>(std::llround(rec.price * 100.0));
            book[intp].push_back({rec.sysid, rec.volume, rec.price, rec.amount});
            sysid_price[rec.sysid] = intp;              // 登记供 contReduceByTrade 按 sysid 扣主动成交量
        }
    }
}

void TickAggregator::contReduceByTrade(int time_sec, const OrderPregroup& order_pregroup) {
    // zb.direction = 主动方; 被动(挂单)方 = 取反, 成交价 = 被动方价位。
    //   主动买(B) -> FIFO扣卖簿(被动卖方) + sysid扣买簿(主动买的已成交量, 处理 pos=0 型=2 余量)
    //   主动卖(S) -> FIFO扣买簿(被动买方) + sysid扣卖簿(主动卖的已成交量)
    for (const char* dir : {"B", "S"}) {
        auto zb_it = order_pregroup.find({time_sec, "zb", dir});
        if (zb_it == order_pregroup.end()) continue;
        bool isBuy = (std::string(dir) == "B");
        auto& passive_book = isBuy ? cont_ask_book_ : cont_bid_book_;  // 被动方簿
        auto& active_book  = isBuy ? cont_bid_book_ : cont_ask_book_;  // 主动方簿
        auto& active_sysid_price = isBuy ? bid_sysid_price_ : ask_sysid_price_;
        for (const auto& rec : zb_it->second) {
            if (!(rec.price > 0) || std::isnan(rec.price)) continue;
            int64_t intp = static_cast<int64_t>(std::llround(rec.price * 100.0));
            contFifoReduce(passive_book, intp, rec.volume);
            // 若主动方(sysid)已按 pos=0 入了己方簿, 按量精确扣减该笔成交。
            // 不在 sysid_price 中(普通被动单不注册)则 contSysidReduce 无操作。
            contSysidReduce(active_book, active_sysid_price, rec.sysid, rec.volume);
        }
    }
}

void TickAggregator::contCancelOrders(int time_sec, const OrderPregroup& order_pregroup) {
    for (const char* dir : {"B", "S"}) {
        auto cl_it = order_pregroup.find({time_sec, "cl", dir});
        if (cl_it == order_pregroup.end()) continue;
        auto& book = (std::string(dir) == "B") ? cont_bid_book_ : cont_ask_book_;
        for (const auto& rec : cl_it->second) {
            if (!(rec.price > 0) || std::isnan(rec.price)) continue;
            int64_t intp = static_cast<int64_t>(std::llround(rec.price * 100.0));
            auto it = book.find(intp);
            if (it == book.end()) continue;
            auto& dq = it->second;
            // 优先按 sysid 精确定位被撤单; 找不到则按 FIFO 扣撤单量(量守恒兜底)。
            bool matched = false;
            for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
                if (dit->sysid == rec.sysid) {
                    dit->vol -= rec.volume;
                    if (dit->vol <= 0) dq.erase(dit);
                    else dit->amount = dit->price * static_cast<double>(dit->vol);
                    matched = true;
                    break;
                }
            }
            if (!matched) contFifoReduce(book, intp, rec.volume);
            else if (dq.empty()) book.erase(it);
        }
    }
}

void TickAggregator::contFillPending(AggregatedData& data) {
    // 连续段 pending 口径:
    //   分档 Vol/Amt —— 真实十档快照(data.bidVolume/bidPrice), 逐档与参考一致。
    //   分档 Num     —— builder 逐单簿导出的每档精确笔数(last_lob_*_num_, 随快照进位)。
    //   Total Num/Vol/Amt —— 连续段全深度 FIFO 守恒簿当前剩余订单, 三者同源。
    for (int lvl = 0; lvl < Constants::LOB_LEVELS; ++lvl) {
        int64_t bv = data.bidVolume[lvl];
        if (bv > 0) {
            double bamt = data.bidPrice[lvl] * static_cast<double>(bv);
            int64_t num = last_lob_bid_num_[lvl];
            if (lvl == 0)      { data.pendingBid1Vol    += bv; data.pendingBid1Amt    += bamt; data.pendingBid1Num    += num; }
            else if (lvl <= 3) { data.pendingBid2_4Vol  += bv; data.pendingBid2_4Amt  += bamt; data.pendingBid2_4Num  += num; }
            else               { data.pendingBid5_10Vol += bv; data.pendingBid5_10Amt += bamt; data.pendingBid5_10Num += num; }
        }
        int64_t av = data.askVolume[lvl];
        if (av > 0) {
            double aamt = data.askPrice[lvl] * static_cast<double>(av);
            int64_t num = last_lob_ask_num_[lvl];
            if (lvl == 0)      { data.pendingAsk1Vol    += av; data.pendingAsk1Amt    += aamt; data.pendingAsk1Num    += num; }
            else if (lvl <= 3) { data.pendingAsk2_4Vol  += av; data.pendingAsk2_4Amt  += aamt; data.pendingAsk2_4Num  += num; }
            else               { data.pendingAsk5_10Vol += av; data.pendingAsk5_10Amt += aamt; data.pendingAsk5_10Num += num; }
        }
    }

    // TotalNum/Vol/Amt = 全深度 FIFO 簿（所有价位）
    int64_t bid_total_num = 0, bid_total_vol = 0;
    double  bid_total_amt = 0.0;
    for (const auto& [price, orders] : cont_bid_book_) {
        (void)price;
        for (const auto& o : orders) {
            if (o.vol <= 0) continue;
            bid_total_num++;
            bid_total_vol += o.vol;
            bid_total_amt += o.price * static_cast<double>(o.vol);
        }
    }

    int64_t ask_total_num = 0, ask_total_vol = 0;
    double  ask_total_amt = 0.0;
    for (const auto& [price, orders] : cont_ask_book_) {
        (void)price;
        for (const auto& o : orders) {
            if (o.vol <= 0) continue;
            ask_total_num++;
            ask_total_vol += o.vol;
            ask_total_amt += o.price * static_cast<double>(o.vol);
        }
    }

    data.pendingBidTotalNum = bid_total_num;
    data.pendingBidTotalVol = bid_total_vol;
    data.pendingBidTotalAmt = bid_total_amt + auction_phantom_bid_amt_;
    data.pendingAskTotalNum = ask_total_num;
    data.pendingAskTotalVol = ask_total_vol;
    data.pendingAskTotalAmt = ask_total_amt - auction_phantom_ask_amt_;
}

void TickAggregator::rebuildPendingFromContBook() {
    pending_orders_.clear();
    for (const auto& [intp, dq] : cont_bid_book_)
        for (const auto& o : dq)
            pending_orders_[o.sysid] = PendingOrder("B", o.vol, o.price, o.amount, -1);
    for (const auto& [intp, dq] : cont_ask_book_)
        for (const auto& o : dq)
            pending_orders_[o.sysid] = PendingOrder("S", o.vol, o.price, o.amount, -1);
}

// 集合竞价 pending: 对当前全深度挂单簿(pending_orders_)做虚拟集合竞价撮合(uncross),
// 用撮合后的残余簿填 pendingBid*/pendingAsk* 统计(Total + 1/2-4/5-10 档)。
// 档位由残余簿价位 rank 决定(竞价段 lob position 不可用)。
// finalize=true(竞价末槽)时, 把残余簿写回 pending_orders_, 使连续段从开盘残余簿继续。
void TickAggregator::fillAuctionPending(AggregatedData& data, bool finalize) {
    struct OrdRef {
        int64_t sysid; int64_t intp; int64_t vol; double price; double amount;
    };
    std::vector<OrdRef> bids, asks;
    std::map<int64_t, int64_t> bidAgg, askAgg;  // intPrice -> 总量
    bids.reserve(pending_orders_.size());
    asks.reserve(pending_orders_.size());
    for (const auto& [sysid, o] : pending_orders_) {
        int64_t intp = static_cast<int64_t>(std::llround(o.price * 100.0));
        if (o.direction == "B") { bids.push_back({sysid, intp, o.volume, o.price, o.amount}); bidAgg[intp] += o.volume; }
        else                    { asks.push_back({sysid, intp, o.volume, o.price, o.amount}); askAgg[intp] += o.volume; }
    }

    // 1. uncross: 最大成交量价 -> matchVol + 清算价
    int64_t matchVol = 0;
    int64_t clearIntPrice = 0;
    if (!bidAgg.empty() && !askAgg.empty()) {
        std::vector<int64_t> prices;
        prices.reserve(bidAgg.size() + askAgg.size());
        for (const auto& kv : bidAgg) prices.push_back(kv.first);
        for (const auto& kv : askAgg) prices.push_back(kv.first);
        std::sort(prices.begin(), prices.end());
        prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
        for (int64_t p : prices) {
            int64_t dem = 0, sup = 0;
            for (const auto& kv : bidAgg) if (kv.first >= p) dem += kv.second;
            for (const auto& kv : askAgg) if (kv.first <= p) sup += kv.second;
            int64_t ex = std::min(dem, sup);
            if (ex > matchVol) { matchVol = ex; clearIntPrice = p; }
        }
    }

    // 2. 价优先 + 时间(sysid)优先 排序, 扣掉 matchVol 得残余订单
    std::sort(bids.begin(), bids.end(), [](const OrdRef& a, const OrdRef& b) {
        return a.intp != b.intp ? a.intp > b.intp : a.sysid < b.sysid; });   // 买: 高价优先
    std::sort(asks.begin(), asks.end(), [](const OrdRef& a, const OrdRef& b) {
        return a.intp != b.intp ? a.intp < b.intp : a.sysid < b.sysid; });   // 卖: 低价优先
    auto residualSide = [&](std::vector<OrdRef>& side) {
        std::vector<OrdRef> out;
        int64_t rem = matchVol;
        for (auto& o : side) {
            if (rem >= o.vol) { rem -= o.vol; continue; }       // 整单撮掉
            if (rem > 0) { OrdRef r = o; r.vol -= rem; r.amount = r.price * static_cast<double>(r.vol); rem = 0; out.push_back(r); }
            else out.push_back(o);
        }
        return out;
    };
    std::vector<OrdRef> resBid = residualSide(bids);
    std::vector<OrdRef> resAsk = residualSide(asks);

    // 缓存清算价/残余最优买卖价, 供 calcTypeStats 竞价段按清算价重分类 type
    auction_has_clearing_ = (matchVol > 0);
    auction_clearing_intp_ = clearIntPrice;
    auction_res_best_bid_intp_ = resBid.empty() ? 0 : resBid.front().intp;  // resBid 高->低
    auction_res_best_ask_intp_ = resAsk.empty() ? 0 : resAsk.front().intp;  // resAsk 低->高

    // 3. 残余簿按价位档位统计 (1=最优, 同价同档)
    auto fillSide = [&](const std::vector<OrdRef>& res, bool isBid) {
        int level = 0; int64_t lastp = std::numeric_limits<int64_t>::min();
        for (const auto& o : res) {
            if (o.intp != lastp) { ++level; lastp = o.intp; }   // res 已按最优->最差
            if (isBid) {
                data.pendingBidTotalVol += o.vol; data.pendingBidTotalAmt += o.amount;
                if (level == 1)      { data.pendingBid1Num++;   data.pendingBid1Vol   += o.vol; data.pendingBid1Amt   += o.amount; }
                else if (level <= 4) { data.pendingBid2_4Num++; data.pendingBid2_4Vol += o.vol; data.pendingBid2_4Amt += o.amount; }
                else if (level <= 10){ data.pendingBid5_10Num++;data.pendingBid5_10Vol+= o.vol; data.pendingBid5_10Amt+= o.amount; }
            } else {
                data.pendingAskTotalVol += o.vol; data.pendingAskTotalAmt += o.amount;
                if (level == 1)      { data.pendingAsk1Num++;   data.pendingAsk1Vol   += o.vol; data.pendingAsk1Amt   += o.amount; }
                else if (level <= 4) { data.pendingAsk2_4Num++; data.pendingAsk2_4Vol += o.vol; data.pendingAsk2_4Amt += o.amount; }
                else if (level <= 10){ data.pendingAsk5_10Num++;data.pendingAsk5_10Vol+= o.vol; data.pendingAsk5_10Amt+= o.amount; }
            }
        }
    };
    fillSide(resBid, true);
    fillSide(resAsk, false);
    // TotalNum = top-10 笔数之和 (与 ref 口径一致)
    data.pendingBidTotalNum = data.pendingBid1Num + data.pendingBid2_4Num + data.pendingBid5_10Num;
    data.pendingAskTotalNum = data.pendingAsk1Num + data.pendingAsk2_4Num + data.pendingAsk5_10Num;

    // 3b. 竞价段盘口快照口径对齐官方 tick_snap:
    //   集合竞价进行中, 官方"盘口"= 指示清算价(买一=卖一=均衡价), 量=可撮合量, 其余档空。
    //   原先直接读 builder 的原始簿会显示涨停买/跌停卖(保护价挂单)甚至交叉, 与官方不符。
    //   - 进行中且有清算(matchVol>0 且非 finalize): bid1=ask1=清算价, 量=matchVol, 2-10 档清零;
    //   - finalize(撮合完成那一格, 如收盘 15:00:00): 撮合已落地, 官方显示"撮合后的残余簿"
    //     (买一=清算价档残余, 卖一=次优未成交档, bid1≠ask1), 故用残余簿前 10 档填;
    //   - 无清算(无重叠): 同样用残余簿(已 uncross, 不交叉)前 10 档填(单边/正常)。
    data.bidPrice.fill(0.0); data.bidVolume.fill(0);
    data.askPrice.fill(0.0); data.askVolume.fill(0);
    if (matchVol > 0 && !finalize) {
        double clrP = clearIntPrice / 100.0;
        data.bidPrice[0] = clrP; data.bidVolume[0] = matchVol;
        data.askPrice[0] = clrP; data.askVolume[0] = matchVol;
    } else {
        auto fillSnap = [](std::array<double, Constants::LOB_LEVELS>& px,
                           std::array<int64_t, Constants::LOB_LEVELS>& vol,
                           const std::vector<OrdRef>& res) {
            int lvl = -1; int64_t lastp = std::numeric_limits<int64_t>::min();
            for (const auto& o : res) {
                if (o.intp != lastp) {
                    ++lvl; lastp = o.intp;
                    if (lvl >= Constants::LOB_LEVELS) break;
                    px[lvl] = o.price; vol[lvl] = o.vol;
                } else if (lvl < Constants::LOB_LEVELS) {
                    vol[lvl] += o.vol;
                }
            }
        };
        fillSnap(data.bidPrice, data.bidVolume, resBid);
        fillSnap(data.askPrice, data.askVolume, resAsk);
    }

    // 调试日志(默认关闭, 用 AUCTION_SNAP_LOG=1 ./lob_data_main ... 开启): 确认竞价段盘口覆盖块
    // 生效。若线上看到竞价盘口仍是原始交叉簿, 但这里没打印, 说明跑的是未含本块的旧二进制。
    static const bool kAuctionSnapLog = (std::getenv("AUCTION_SNAP_LOG") != nullptr);
    if (kAuctionSnapLog) {
        std::cerr << "[AUCTION_SNAP] " << stock_code_
                  << " dt=" << data.dateTime
                  << " matchVol=" << matchVol
                  << " clearP=" << (clearIntPrice / 100.0)
                  << " -> bid1=" << data.bidPrice[0] << "/" << data.bidVolume[0]
                  << " ask1=" << data.askPrice[0] << "/" << data.askVolume[0]
                  << (matchVol > 0 ? " [indicative]" : " [residual]") << "\n";
    }

    // 4. finalize: 残余簿写回 pending_orders_ (撮掉的整单移除, 部分单调整量),
    //    position 记为 -档位(负=排队), 供连续段按档位继续统计。
    //    同时把残余簿播种到连续段守恒簿 cont_*_book_ —— 开盘竞价残余即连续段起始簿。
    //    resBid 已按高价优先、同价 sysid(时间)升序; resAsk 低价优先, push_back 即保持
    //    同价位时间优先(FIFO)。
    if (finalize) {
        pending_orders_.clear();
        cont_bid_book_.clear();
        cont_ask_book_.clear();
        bid_sysid_price_.clear();
        ask_sysid_price_.clear();
        int level = 0; int64_t lastp = std::numeric_limits<int64_t>::min();
        for (const auto& o : resBid) {
            if (o.intp != lastp) { ++level; lastp = o.intp; }
            pending_orders_[o.sysid] = PendingOrder("B", o.vol, o.price, o.amount, -level);
            cont_bid_book_[o.intp].push_back({o.sysid, o.vol, o.price, o.amount});
        }
        level = 0; lastp = std::numeric_limits<int64_t>::min();
        for (const auto& o : resAsk) {
            if (o.intp != lastp) { ++level; lastp = o.intp; }
            pending_orders_[o.sysid] = PendingOrder("S", o.vol, o.price, o.amount, -level);
            cont_ask_book_[o.intp].push_back({o.sysid, o.vol, o.price, o.amount});
        }

        // 计算老建造器幽灵: 竞价撮合时老版本以限价入账但以清算价出账, 差价残留在 Total Amt 里。
        // bid 幽灵 = Σ(bid_limit - clearing_price) × matched_vol  (需加回 pendingBidTotalAmt)
        // ask 幽灵 = Σ(clearing_price - ask_limit) × matched_vol  (需从 pendingAskTotalAmt 扣)
        if (matchVol > 0) {
            double clrPrice = clearIntPrice / 100.0;
            auction_phantom_bid_amt_ = 0.0;
            auction_phantom_ask_amt_ = 0.0;
            // bids 已按高价优先排序; 按 matchVol 从高到低撮合
            int64_t rem = matchVol;
            for (const auto& o : bids) {
                if (rem <= 0) break;
                int64_t mv = std::min(o.vol, rem);
                auction_phantom_bid_amt_ += (o.price - clrPrice) * static_cast<double>(mv);
                rem -= mv;
            }
            // asks 已按低价优先排序; 按 matchVol 从低到高撮合
            rem = matchVol;
            for (const auto& o : asks) {
                if (rem <= 0) break;
                int64_t mv = std::min(o.vol, rem);
                auction_phantom_ask_amt_ += (clrPrice - o.price) * static_cast<double>(mv);
                rem -= mv;
            }
        }
    }
}

void TickAggregator::calcMarketSummary(AggregatedData& data, int time_sec,
                                       const OrderPregroup& order_pregroup) {
    
    // 累计成交量：从最新tradevolume字段
    auto all_it = order_pregroup.find({time_sec, "ALL", "ALL"});
    if (all_it != order_pregroup.end() && !all_it->second.empty()) {
        data.totalVolume = all_it->second.back().tradevolume;
    } else {
        data.totalVolume = last_total_volume_;
    }
    
    // 累计成交额：自己累加zb的amount
    auto zb_it = order_pregroup.find({time_sec, "zb", "ALL"});
    double sec_amount = 0.0;
    if (zb_it != order_pregroup.end()) {
        for (const auto& rec : zb_it->second) {
            sec_amount += rec.amount;
        }
    }
    cumulative_amount_ += sec_amount;
    data.totalAmount = cumulative_amount_;
    
    // 3s内OHLC (与Python保持一致，无数据时为NaN/0)
    if (zb_it == order_pregroup.end() || zb_it->second.empty()) {
        data.openPrice = 0.0;  // Python用np.nan，但cleanDouble会转为0
        data.highPrice = 0.0;
        data.lowPrice = 0.0;
        data.lastPrice = last_price_;  // 继承上一秒的价格
    } else {
        std::vector<double> valid_prices;
        for (const auto& rec : zb_it->second) {
            if (rec.price > 0 && !std::isnan(rec.price)) {
                valid_prices.push_back(rec.price);
            }
        }
        
        if (valid_prices.empty()) {
            data.openPrice = 0.0;
            data.highPrice = 0.0;
            data.lowPrice = 0.0;
            data.lastPrice = last_price_;  // 继承上一秒的价格
        } else {
            data.openPrice = valid_prices.front();
            data.lastPrice = valid_prices.back();
            data.highPrice = *std::max_element(valid_prices.begin(), valid_prices.end());
            data.lowPrice = *std::min_element(valid_prices.begin(), valid_prices.end());
            
            // 更新日内高低
            if (data.highPrice > day_high_) day_high_ = data.highPrice;
            if (data.lowPrice > 0 && data.lowPrice < day_low_) day_low_ = data.lowPrice;
            
            last_price_ = data.lastPrice;
        }
    }
    
    // 当日最高最低
    data.dayHigh = (day_high_ > 0) ? day_high_ : 0.0;
    data.dayLow = (day_low_ != std::numeric_limits<double>::infinity()) ? day_low_ : 0.0;
    
    // VWAP
    if (data.totalVolume > 0) {
        data.vwap = data.totalAmount / data.totalVolume;
    } else {
        data.vwap = last_vwap_;
    }
    
    // 更新last_market
    last_price_ = data.lastPrice;
    last_vwap_ = data.vwap;
    last_total_volume_ = data.totalVolume;
    last_total_amount_ = data.totalAmount;
}

bool TickAggregator::isContinuousTrading(int seconds) const {
    // 左闭右开 [start, end): 与 3s 槽 floor 口径一致 (bar 标号=左端)
    return (Constants::CONTINUOUS_MORNING_START <= seconds && seconds < Constants::CONTINUOUS_MORNING_END) ||
           (Constants::CONTINUOUS_AFTERNOON_START <= seconds && seconds < Constants::CONTINUOUS_AFTERNOON_END);
}

bool TickAggregator::isCallAuction(int seconds) const {
    // 左闭右开 [start, end)
    return (Constants::CALL_AUCTION_MORNING_START <= seconds && seconds < Constants::CALL_AUCTION_MORNING_END) ||
           (Constants::CALL_AUCTION_CLOSE_START <= seconds && seconds < Constants::CALL_AUCTION_CLOSE_END);
}

int TickAggregator::getInstrumentId() const {
    std::string code = stock_code_;
    // 移除sz/sh前缀
    if (code.substr(0, 2) == "sz" || code.substr(0, 2) == "sh") {
        code = code.substr(2);
    }
    try {
        return std::stoi(code);
    } catch (...) {
        return 0;
    }
}

std::string TickAggregator::secondsToDateTime(int seconds) const {
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    
    std::tm* tm_date = std::localtime(&trade_date_);
    
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_date->tm_year + 1900) << "-"
        << std::setw(2) << (tm_date->tm_mon + 1) << "-"
        << std::setw(2) << tm_date->tm_mday << " "
        << std::setw(2) << h << ":"
        << std::setw(2) << m << ":"
        << std::setw(2) << s;
    
    return oss.str();
}

double TickAggregator::safeMean(const std::vector<double>& prices) const {
    if (prices.empty()) return 0.0;
    
    double sum = 0.0;
    int count = 0;
    for (double p : prices) {
        if (p > 0 && !std::isnan(p)) {
            sum += p;
            count++;
        }
    }
    
    return (count > 0) ? (sum / count) : 0.0;
}

double TickAggregator::safeWeightedPrice(const std::vector<double>& prices,
                                         const std::vector<int64_t>& volumes) const {
    if (prices.empty() || volumes.empty() || prices.size() != volumes.size()) {
        return 0.0;
    }
    
    double weighted_sum = 0.0;
    int64_t vol_sum = 0;
    
    for (size_t i = 0; i < prices.size(); ++i) {
        if (prices[i] > 0 && !std::isnan(prices[i])) {
            weighted_sum += prices[i] * volumes[i];
            vol_sum += volumes[i];
        }
    }
    
    return (vol_sum > 0) ? (weighted_sum / vol_sum) : 0.0;
}

// ============================================================
// ClickHouseManager实现
// ============================================================

ClickHouseManager::ClickHouseManager(const ClickHouseConfig& config)
    : database_(config.database), config_(config) {
    
    clickhouse::ClientOptions options;
    options.SetHost(config.host);
    options.SetPort(config.port);
    options.SetUser(config.username);
    options.SetPassword(config.password);
    options.SetDefaultDatabase(config.database);
    
    client_ = std::make_unique<clickhouse::Client>(options);
    
}

ClickHouseManager::~ClickHouseManager() {}

double ClickHouseManager::cleanDouble(double val) {
    if (std::isnan(val) || std::isinf(val)) {
        return 0.0;
    }
    return val;
}

void ClickHouseManager::cleanDataForClickHouse(AggregatedData& data) {
    // 清理所有double字段的NaN和Inf值
    data.wtBidAmt = cleanDouble(data.wtBidAmt);
    data.wtAskAmt = cleanDouble(data.wtAskAmt);
    data.zbBidAmt = cleanDouble(data.zbBidAmt);
    data.zbAskAmt = cleanDouble(data.zbAskAmt);
    data.clBidAmt = cleanDouble(data.clBidAmt);
    data.clAskAmt = cleanDouble(data.clAskAmt);
    
    data.wtBidp = cleanDouble(data.wtBidp);
    data.wtAskp = cleanDouble(data.wtAskp);
    data.zbBidp = cleanDouble(data.zbBidp);
    data.zbAskp = cleanDouble(data.zbAskp);
    data.clBidp = cleanDouble(data.clBidp);
    data.clAskp = cleanDouble(data.clAskp);
    
    data.wtBidp1 = cleanDouble(data.wtBidp1);
    data.wtAskp1 = cleanDouble(data.wtAskp1);
    data.zbBidp1 = cleanDouble(data.zbBidp1);
    data.zbAskp1 = cleanDouble(data.zbAskp1);
    
    data.wtBidWeightPrice = cleanDouble(data.wtBidWeightPrice);
    data.wtAskWeightPrice = cleanDouble(data.wtAskWeightPrice);
    
    data.wtBidNumRatio = cleanDouble(data.wtBidNumRatio);
    data.wtAskNumRatio = cleanDouble(data.wtAskNumRatio);
    data.wtBidVolRatio = cleanDouble(data.wtBidVolRatio);
    data.wtAskVolRatio = cleanDouble(data.wtAskVolRatio);
    data.wtBidAvgPos = cleanDouble(data.wtBidAvgPos);
    data.wtAskAvgPos = cleanDouble(data.wtAskAvgPos);
    data.wtPBidPos = cleanDouble(data.wtPBidPos);
    data.wtPAskPos = cleanDouble(data.wtPAskPos);
    
    data.bidMaxPrice = cleanDouble(data.bidMaxPrice);
    data.askMaxPrice = cleanDouble(data.askMaxPrice);
    
    // 档位区间字段
    data.wtBid1Amt = cleanDouble(data.wtBid1Amt);
    data.wtBid2_4Amt = cleanDouble(data.wtBid2_4Amt);
    data.wtBid5_10Amt = cleanDouble(data.wtBid5_10Amt);
    data.wtBid11InfAmt = cleanDouble(data.wtBid11InfAmt);
    data.wtBid2_4AvgPrice = cleanDouble(data.wtBid2_4AvgPrice);
    data.wtBid5_10AvgPrice = cleanDouble(data.wtBid5_10AvgPrice);
    data.wtBid11InfAvgPrice = cleanDouble(data.wtBid11InfAvgPrice);
    
    data.wtAsk1Amt = cleanDouble(data.wtAsk1Amt);
    data.wtAsk2_4Amt = cleanDouble(data.wtAsk2_4Amt);
    data.wtAsk5_10Amt = cleanDouble(data.wtAsk5_10Amt);
    data.wtAsk11InfAmt = cleanDouble(data.wtAsk11InfAmt);
    data.wtAsk2_4AvgPrice = cleanDouble(data.wtAsk2_4AvgPrice);
    data.wtAsk5_10AvgPrice = cleanDouble(data.wtAsk5_10AvgPrice);
    data.wtAsk11InfAvgPrice = cleanDouble(data.wtAsk11InfAvgPrice);
    
    data.clBid1Amt = cleanDouble(data.clBid1Amt);
    data.clBid2_4Amt = cleanDouble(data.clBid2_4Amt);
    data.clBid5_10Amt = cleanDouble(data.clBid5_10Amt);
    data.clBid2_4AvgPrice = cleanDouble(data.clBid2_4AvgPrice);
    data.clBid5_10AvgPrice = cleanDouble(data.clBid5_10AvgPrice);
    
    data.clAsk1Amt = cleanDouble(data.clAsk1Amt);
    data.clAsk2_4Amt = cleanDouble(data.clAsk2_4Amt);
    data.clAsk5_10Amt = cleanDouble(data.clAsk5_10Amt);
    data.clAsk2_4AvgPrice = cleanDouble(data.clAsk2_4AvgPrice);
    data.clAsk5_10AvgPrice = cleanDouble(data.clAsk5_10AvgPrice);
    
    // Pending字段
    data.pendingBid1Amt = cleanDouble(data.pendingBid1Amt);
    data.pendingBid2_4Amt = cleanDouble(data.pendingBid2_4Amt);
    data.pendingBid5_10Amt = cleanDouble(data.pendingBid5_10Amt);
    data.pendingBidTotalAmt = cleanDouble(data.pendingBidTotalAmt);
    data.pendingAsk1Amt = cleanDouble(data.pendingAsk1Amt);
    data.pendingAsk2_4Amt = cleanDouble(data.pendingAsk2_4Amt);
    data.pendingAsk5_10Amt = cleanDouble(data.pendingAsk5_10Amt);
    data.pendingAskTotalAmt = cleanDouble(data.pendingAskTotalAmt);
    
    // 盘口字段
    for (int i = 0; i < Constants::LOB_LEVELS; ++i) {
        data.bidPrice[i] = cleanDouble(data.bidPrice[i]);
        data.askPrice[i] = cleanDouble(data.askPrice[i]);
        // bidVolume/askVolume是int64_t但ClickHouse列是UInt64，
        // 负值会溢出为极大正数导致异常值
        if (data.bidVolume[i] < 0) data.bidVolume[i] = 0;
        if (data.askVolume[i] < 0) data.askVolume[i] = 0;
    }
    
    // 行情字段
    data.totalAmount = cleanDouble(data.totalAmount);
    data.openPrice = cleanDouble(data.openPrice);
    data.highPrice = cleanDouble(data.highPrice);
    data.lowPrice = cleanDouble(data.lowPrice);
    data.lastPrice = cleanDouble(data.lastPrice);
    data.dayHigh = cleanDouble(data.dayHigh);
    data.dayLow = cleanDouble(data.dayLow);
    data.vwap = cleanDouble(data.vwap);
}

void ClickHouseManager::insertData(const std::vector<AggregatedData>& data,
                                   const std::string& table_name) {
    if (data.empty()) return;
    
    clickhouse::Block block;
    
    // ========== 基础信息列 ==========
    auto col_tradedate = std::make_shared<clickhouse::ColumnString>();  // 直接传递 YYYY-MM-DD 格式字符串
    auto col_symbol = std::make_shared<clickhouse::ColumnString>();
    auto col_dateTime = std::make_shared<clickhouse::ColumnInt64>();
    auto col_instrumentID = std::make_shared<clickhouse::ColumnInt32>();
    auto col_event_time = std::make_shared<clickhouse::ColumnString>();
    
    // ========== 基础统计列: wt/zb/cl × Bid/Ask ==========
    auto col_wtBidNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtAskNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_zbBidNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_zbAskNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clBidNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clAskNum = std::make_shared<clickhouse::ColumnUInt32>();
    
    auto col_wtBidVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtAskVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_zbBidVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_zbAskVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clBidVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clAskVol = std::make_shared<clickhouse::ColumnUInt64>();
    
    auto col_wtBidAmt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAskAmt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_zbBidAmt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_zbAskAmt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clBidAmt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clAskAmt = std::make_shared<clickhouse::ColumnFloat64>();
    
    auto col_wtBidp = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAskp = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_zbBidp = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_zbAskp = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clBidp = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clAskp = std::make_shared<clickhouse::ColumnFloat64>();
    
    // 最新价量
    auto col_wtBidp1 = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAskp1 = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_zbBidp1 = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_zbAskp1 = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBidv1 = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtAskv1 = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_zbBidv1 = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_zbAskv1 = std::make_shared<clickhouse::ColumnUInt64>();
    
    // 量加权价
    auto col_wtBidWeightPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAskWeightPrice = std::make_shared<clickhouse::ColumnFloat64>();
    
    // 总计
    auto col_wtVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_zbVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_zbNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clNum = std::make_shared<clickhouse::ColumnUInt32>();
    
    // ========== 位置统计列 ==========
    auto col_wtBidNumRatio = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAskNumRatio = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBidVolRatio = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAskVolRatio = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBidAvgPos = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAskAvgPos = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtABidVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtAAskVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtABidNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtAAskNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtPBidPos = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtPAskPos = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtPBidVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtPAskVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtPBidNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtPAskNum = std::make_shared<clickhouse::ColumnUInt32>();
    
    // 最大档位
    auto col_bidMaxPos = std::make_shared<clickhouse::ColumnUInt16>();
    auto col_askMaxPos = std::make_shared<clickhouse::ColumnUInt16>();
    auto col_bidMaxPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_askMaxPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_bidMaxVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_askMaxVol = std::make_shared<clickhouse::ColumnUInt64>();
    
    // ========== 委托档位区间列 ==========
    auto col_wtBid1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtBid2_4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtBid5_10Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtBid11InfNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtBid1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtBid2_4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtBid5_10Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtBid11InfVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtBid1Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBid2_4Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBid5_10Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBid11InfAmt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBid2_4AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBid5_10AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtBid11InfAvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    
    auto col_wtAsk1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtAsk2_4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtAsk5_10Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtAsk11InfNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_wtAsk1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtAsk2_4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtAsk5_10Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtAsk11InfVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_wtAsk1Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAsk2_4Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAsk5_10Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAsk11InfAmt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAsk2_4AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAsk5_10AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_wtAsk11InfAvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    
    // ========== 撤单档位列 ==========
    auto col_clBid1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clBid2_4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clBid5_10Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clBid1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clBid2_4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clBid5_10Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clBid1Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clBid2_4Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clBid5_10Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clBid2_4AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clBid5_10AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    
    auto col_clAsk1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clAsk2_4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clAsk5_10Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_clAsk1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clAsk2_4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clAsk5_10Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_clAsk1Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clAsk2_4Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clAsk5_10Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clAsk2_4AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_clAsk5_10AvgPrice = std::make_shared<clickhouse::ColumnFloat64>();
    
    // ========== Type分类列 ==========
    auto col_bidType1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_bidType2Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_bidType3Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_bidType4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_bidType5Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_bidType6Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_bidType1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_bidType2Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_bidType3Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_bidType4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_bidType5Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_bidType6Num = std::make_shared<clickhouse::ColumnUInt32>();
    
    auto col_askType1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_askType2Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_askType3Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_askType4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_askType5Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_askType6Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_askType1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_askType2Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_askType3Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_askType4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_askType5Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_askType6Num = std::make_shared<clickhouse::ColumnUInt32>();
    
    // ========== Pending订单池列 ==========
    auto col_pendingBid1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingBid2_4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingBid5_10Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingBidTotalNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingBid1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingBid2_4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingBid5_10Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingBidTotalVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingBid1Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_pendingBid2_4Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_pendingBid5_10Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_pendingBidTotalAmt = std::make_shared<clickhouse::ColumnFloat64>();
    
    auto col_pendingAsk1Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingAsk2_4Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingAsk5_10Num = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingAskTotalNum = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_pendingAsk1Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingAsk2_4Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingAsk5_10Vol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingAskTotalVol = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_pendingAsk1Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_pendingAsk2_4Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_pendingAsk5_10Amt = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_pendingAskTotalAmt = std::make_shared<clickhouse::ColumnFloat64>();
    
    // ========== 10档盘口列 ==========
    std::array<std::shared_ptr<clickhouse::ColumnFloat64>, 10> col_bidPrice;
    std::array<std::shared_ptr<clickhouse::ColumnUInt64>, 10> col_bidVolume;
    std::array<std::shared_ptr<clickhouse::ColumnFloat64>, 10> col_askPrice;
    std::array<std::shared_ptr<clickhouse::ColumnUInt64>, 10> col_askVolume;
    for (int i = 0; i < 10; ++i) {
        col_bidPrice[i] = std::make_shared<clickhouse::ColumnFloat64>();
        col_bidVolume[i] = std::make_shared<clickhouse::ColumnUInt64>();
        col_askPrice[i] = std::make_shared<clickhouse::ColumnFloat64>();
        col_askVolume[i] = std::make_shared<clickhouse::ColumnUInt64>();
    }
    
    // ========== 行情汇总列 ==========
    auto col_totalVolume = std::make_shared<clickhouse::ColumnUInt64>();
    auto col_totalAmount = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_openPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_highPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_lowPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_lastPrice = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_dayHigh = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_dayLow = std::make_shared<clickhouse::ColumnFloat64>();
    auto col_vwap = std::make_shared<clickhouse::ColumnFloat64>();
    
    // ========== 元数据列 ==========
    auto col_eventCount = std::make_shared<clickhouse::ColumnUInt32>();
    auto col_dataFlag = std::make_shared<clickhouse::ColumnUInt8>();
    
    // 预计算日期字符串 (同一批数据都是同一天)
    std::string tradedate_str;
    if (!data.empty()) {
        tradedate_str = Utils::formatDate(data[0].tradedate, "%Y-%m-%d");
    }
    
    // ========== 填充数据 ==========
    for (const auto& row : data) {
        AggregatedData r = row;
        cleanDataForClickHouse(r);
        
        // 基础信息
        // tradedate: 直接传递 YYYY-MM-DD 格式字符串，ClickHouse 会自动转换
        col_tradedate->Append(tradedate_str);
        col_symbol->Append(r.symbol);
        col_dateTime->Append(r.dateTime);
        col_instrumentID->Append(r.instrumentID);
        col_event_time->Append(r.event_time);
        
        // 基础统计
        col_wtBidNum->Append(r.wtBidNum);
        col_wtAskNum->Append(r.wtAskNum);
        col_zbBidNum->Append(r.zbBidNum);
        col_zbAskNum->Append(r.zbAskNum);
        col_clBidNum->Append(r.clBidNum);
        col_clAskNum->Append(r.clAskNum);
        
        col_wtBidVol->Append(r.wtBidVol);
        col_wtAskVol->Append(r.wtAskVol);
        col_zbBidVol->Append(r.zbBidVol);
        col_zbAskVol->Append(r.zbAskVol);
        col_clBidVol->Append(r.clBidVol);
        col_clAskVol->Append(r.clAskVol);
        
        col_wtBidAmt->Append(r.wtBidAmt);
        col_wtAskAmt->Append(r.wtAskAmt);
        col_zbBidAmt->Append(r.zbBidAmt);
        col_zbAskAmt->Append(r.zbAskAmt);
        col_clBidAmt->Append(r.clBidAmt);
        col_clAskAmt->Append(r.clAskAmt);
        
        col_wtBidp->Append(r.wtBidp);
        col_wtAskp->Append(r.wtAskp);
        col_zbBidp->Append(r.zbBidp);
        col_zbAskp->Append(r.zbAskp);
        col_clBidp->Append(r.clBidp);
        col_clAskp->Append(r.clAskp);
        
        // 最新价量
        col_wtBidp1->Append(r.wtBidp1);
        col_wtAskp1->Append(r.wtAskp1);
        col_zbBidp1->Append(r.zbBidp1);
        col_zbAskp1->Append(r.zbAskp1);
        col_wtBidv1->Append(r.wtBidv1);
        col_wtAskv1->Append(r.wtAskv1);
        col_zbBidv1->Append(r.zbBidv1);
        col_zbAskv1->Append(r.zbAskv1);
        
        // 量加权价
        col_wtBidWeightPrice->Append(r.wtBidWeightPrice);
        col_wtAskWeightPrice->Append(r.wtAskWeightPrice);
        
        // 总计
        col_wtVol->Append(r.wtVol);
        col_zbVol->Append(r.zbVol);
        col_clVol->Append(r.clVol);
        col_wtNum->Append(r.wtNum);
        col_zbNum->Append(r.zbNum);
        col_clNum->Append(r.clNum);
        
        // 位置统计
        col_wtBidNumRatio->Append(r.wtBidNumRatio);
        col_wtAskNumRatio->Append(r.wtAskNumRatio);
        col_wtBidVolRatio->Append(r.wtBidVolRatio);
        col_wtAskVolRatio->Append(r.wtAskVolRatio);
        col_wtBidAvgPos->Append(r.wtBidAvgPos);
        col_wtAskAvgPos->Append(r.wtAskAvgPos);
        col_wtABidVol->Append(r.wtABidVol);
        col_wtAAskVol->Append(r.wtAAskVol);
        col_wtABidNum->Append(r.wtABidNum);
        col_wtAAskNum->Append(r.wtAAskNum);
        col_wtPBidPos->Append(r.wtPBidPos);
        col_wtPAskPos->Append(r.wtPAskPos);
        col_wtPBidVol->Append(r.wtPBidVol);
        col_wtPAskVol->Append(r.wtPAskVol);
        col_wtPBidNum->Append(r.wtPBidNum);
        col_wtPAskNum->Append(r.wtPAskNum);
        
        // 最大档位
        col_bidMaxPos->Append(r.bidMaxPos);
        col_askMaxPos->Append(r.askMaxPos);
        col_bidMaxPrice->Append(r.bidMaxPrice);
        col_askMaxPrice->Append(r.askMaxPrice);
        col_bidMaxVol->Append(r.bidMaxVol);
        col_askMaxVol->Append(r.askMaxVol);
        
        // 委托档位区间
        col_wtBid1Num->Append(r.wtBid1Num);
        col_wtBid2_4Num->Append(r.wtBid2_4Num);
        col_wtBid5_10Num->Append(r.wtBid5_10Num);
        col_wtBid11InfNum->Append(r.wtBid11InfNum);
        col_wtBid1Vol->Append(r.wtBid1Vol);
        col_wtBid2_4Vol->Append(r.wtBid2_4Vol);
        col_wtBid5_10Vol->Append(r.wtBid5_10Vol);
        col_wtBid11InfVol->Append(r.wtBid11InfVol);
        col_wtBid1Amt->Append(r.wtBid1Amt);
        col_wtBid2_4Amt->Append(r.wtBid2_4Amt);
        col_wtBid5_10Amt->Append(r.wtBid5_10Amt);
        col_wtBid11InfAmt->Append(r.wtBid11InfAmt);
        col_wtBid2_4AvgPrice->Append(r.wtBid2_4AvgPrice);
        col_wtBid5_10AvgPrice->Append(r.wtBid5_10AvgPrice);
        col_wtBid11InfAvgPrice->Append(r.wtBid11InfAvgPrice);
        
        col_wtAsk1Num->Append(r.wtAsk1Num);
        col_wtAsk2_4Num->Append(r.wtAsk2_4Num);
        col_wtAsk5_10Num->Append(r.wtAsk5_10Num);
        col_wtAsk11InfNum->Append(r.wtAsk11InfNum);
        col_wtAsk1Vol->Append(r.wtAsk1Vol);
        col_wtAsk2_4Vol->Append(r.wtAsk2_4Vol);
        col_wtAsk5_10Vol->Append(r.wtAsk5_10Vol);
        col_wtAsk11InfVol->Append(r.wtAsk11InfVol);
        col_wtAsk1Amt->Append(r.wtAsk1Amt);
        col_wtAsk2_4Amt->Append(r.wtAsk2_4Amt);
        col_wtAsk5_10Amt->Append(r.wtAsk5_10Amt);
        col_wtAsk11InfAmt->Append(r.wtAsk11InfAmt);
        col_wtAsk2_4AvgPrice->Append(r.wtAsk2_4AvgPrice);
        col_wtAsk5_10AvgPrice->Append(r.wtAsk5_10AvgPrice);
        col_wtAsk11InfAvgPrice->Append(r.wtAsk11InfAvgPrice);
        
        // 撤单档位
        col_clBid1Num->Append(r.clBid1Num);
        col_clBid2_4Num->Append(r.clBid2_4Num);
        col_clBid5_10Num->Append(r.clBid5_10Num);
        col_clBid1Vol->Append(r.clBid1Vol);
        col_clBid2_4Vol->Append(r.clBid2_4Vol);
        col_clBid5_10Vol->Append(r.clBid5_10Vol);
        col_clBid1Amt->Append(r.clBid1Amt);
        col_clBid2_4Amt->Append(r.clBid2_4Amt);
        col_clBid5_10Amt->Append(r.clBid5_10Amt);
        col_clBid2_4AvgPrice->Append(r.clBid2_4AvgPrice);
        col_clBid5_10AvgPrice->Append(r.clBid5_10AvgPrice);
        
        col_clAsk1Num->Append(r.clAsk1Num);
        col_clAsk2_4Num->Append(r.clAsk2_4Num);
        col_clAsk5_10Num->Append(r.clAsk5_10Num);
        col_clAsk1Vol->Append(r.clAsk1Vol);
        col_clAsk2_4Vol->Append(r.clAsk2_4Vol);
        col_clAsk5_10Vol->Append(r.clAsk5_10Vol);
        col_clAsk1Amt->Append(r.clAsk1Amt);
        col_clAsk2_4Amt->Append(r.clAsk2_4Amt);
        col_clAsk5_10Amt->Append(r.clAsk5_10Amt);
        col_clAsk2_4AvgPrice->Append(r.clAsk2_4AvgPrice);
        col_clAsk5_10AvgPrice->Append(r.clAsk5_10AvgPrice);
        
        // Type分类
        col_bidType1Vol->Append(r.bidType1Vol);
        col_bidType2Vol->Append(r.bidType2Vol);
        col_bidType3Vol->Append(r.bidType3Vol);
        col_bidType4Vol->Append(r.bidType4Vol);
        col_bidType5Vol->Append(r.bidType5Vol);
        col_bidType6Vol->Append(r.bidType6Vol);
        col_bidType1Num->Append(r.bidType1Num);
        col_bidType2Num->Append(r.bidType2Num);
        col_bidType3Num->Append(r.bidType3Num);
        col_bidType4Num->Append(r.bidType4Num);
        col_bidType5Num->Append(r.bidType5Num);
        col_bidType6Num->Append(r.bidType6Num);
        
        col_askType1Vol->Append(r.askType1Vol);
        col_askType2Vol->Append(r.askType2Vol);
        col_askType3Vol->Append(r.askType3Vol);
        col_askType4Vol->Append(r.askType4Vol);
        col_askType5Vol->Append(r.askType5Vol);
        col_askType6Vol->Append(r.askType6Vol);
        col_askType1Num->Append(r.askType1Num);
        col_askType2Num->Append(r.askType2Num);
        col_askType3Num->Append(r.askType3Num);
        col_askType4Num->Append(r.askType4Num);
        col_askType5Num->Append(r.askType5Num);
        col_askType6Num->Append(r.askType6Num);
        
        // Pending订单池
        col_pendingBid1Num->Append(r.pendingBid1Num);
        col_pendingBid2_4Num->Append(r.pendingBid2_4Num);
        col_pendingBid5_10Num->Append(r.pendingBid5_10Num);
        col_pendingBidTotalNum->Append(r.pendingBidTotalNum);
        col_pendingBid1Vol->Append(r.pendingBid1Vol);
        col_pendingBid2_4Vol->Append(r.pendingBid2_4Vol);
        col_pendingBid5_10Vol->Append(r.pendingBid5_10Vol);
        col_pendingBidTotalVol->Append(r.pendingBidTotalVol);
        col_pendingBid1Amt->Append(r.pendingBid1Amt);
        col_pendingBid2_4Amt->Append(r.pendingBid2_4Amt);
        col_pendingBid5_10Amt->Append(r.pendingBid5_10Amt);
        col_pendingBidTotalAmt->Append(r.pendingBidTotalAmt);
        
        col_pendingAsk1Num->Append(r.pendingAsk1Num);
        col_pendingAsk2_4Num->Append(r.pendingAsk2_4Num);
        col_pendingAsk5_10Num->Append(r.pendingAsk5_10Num);
        col_pendingAskTotalNum->Append(r.pendingAskTotalNum);
        col_pendingAsk1Vol->Append(r.pendingAsk1Vol);
        col_pendingAsk2_4Vol->Append(r.pendingAsk2_4Vol);
        col_pendingAsk5_10Vol->Append(r.pendingAsk5_10Vol);
        col_pendingAskTotalVol->Append(r.pendingAskTotalVol);
        col_pendingAsk1Amt->Append(r.pendingAsk1Amt);
        col_pendingAsk2_4Amt->Append(r.pendingAsk2_4Amt);
        col_pendingAsk5_10Amt->Append(r.pendingAsk5_10Amt);
        col_pendingAskTotalAmt->Append(r.pendingAskTotalAmt);
        
        // 10档盘口
        for (int i = 0; i < 10; ++i) {
            col_bidPrice[i]->Append(r.bidPrice[i]);
            col_bidVolume[i]->Append(r.bidVolume[i]);
            col_askPrice[i]->Append(r.askPrice[i]);
            col_askVolume[i]->Append(r.askVolume[i]);
        }
        
        // 行情汇总
        col_totalVolume->Append(r.totalVolume);
        col_totalAmount->Append(r.totalAmount);
        col_openPrice->Append(r.openPrice);
        col_highPrice->Append(r.highPrice);
        col_lowPrice->Append(r.lowPrice);
        col_lastPrice->Append(r.lastPrice);
        col_dayHigh->Append(r.dayHigh);
        col_dayLow->Append(r.dayLow);
        col_vwap->Append(r.vwap);
        
        // 元数据
        col_eventCount->Append(r.eventCount);
        col_dataFlag->Append(static_cast<uint8_t>(r.dataFlag));
    }
    
    // ========== 添加列到Block ==========
    block.AppendColumn("tradedate", col_tradedate);
    block.AppendColumn("symbol", col_symbol);
    block.AppendColumn("dateTime", col_dateTime);
    block.AppendColumn("instrumentID", col_instrumentID);
    block.AppendColumn("event_time", col_event_time);
    
    block.AppendColumn("wtBidNum", col_wtBidNum);
    block.AppendColumn("wtAskNum", col_wtAskNum);
    block.AppendColumn("zbBidNum", col_zbBidNum);
    block.AppendColumn("zbAskNum", col_zbAskNum);
    block.AppendColumn("clBidNum", col_clBidNum);
    block.AppendColumn("clAskNum", col_clAskNum);
    
    block.AppendColumn("wtBidVol", col_wtBidVol);
    block.AppendColumn("wtAskVol", col_wtAskVol);
    block.AppendColumn("zbBidVol", col_zbBidVol);
    block.AppendColumn("zbAskVol", col_zbAskVol);
    block.AppendColumn("clBidVol", col_clBidVol);
    block.AppendColumn("clAskVol", col_clAskVol);
    
    block.AppendColumn("wtBidAmt", col_wtBidAmt);
    block.AppendColumn("wtAskAmt", col_wtAskAmt);
    block.AppendColumn("zbBidAmt", col_zbBidAmt);
    block.AppendColumn("zbAskAmt", col_zbAskAmt);
    block.AppendColumn("clBidAmt", col_clBidAmt);
    block.AppendColumn("clAskAmt", col_clAskAmt);
    
    block.AppendColumn("wtBidp", col_wtBidp);
    block.AppendColumn("wtAskp", col_wtAskp);
    block.AppendColumn("zbBidp", col_zbBidp);
    block.AppendColumn("zbAskp", col_zbAskp);
    block.AppendColumn("clBidp", col_clBidp);
    block.AppendColumn("clAskp", col_clAskp);
    
    block.AppendColumn("wtBidp1", col_wtBidp1);
    block.AppendColumn("wtAskp1", col_wtAskp1);
    block.AppendColumn("zbBidp1", col_zbBidp1);
    block.AppendColumn("zbAskp1", col_zbAskp1);
    block.AppendColumn("wtBidv1", col_wtBidv1);
    block.AppendColumn("wtAskv1", col_wtAskv1);
    block.AppendColumn("zbBidv1", col_zbBidv1);
    block.AppendColumn("zbAskv1", col_zbAskv1);
    
    block.AppendColumn("wtBidWeightPrice", col_wtBidWeightPrice);
    block.AppendColumn("wtAskWeightPrice", col_wtAskWeightPrice);
    
    block.AppendColumn("wtVol", col_wtVol);
    block.AppendColumn("zbVol", col_zbVol);
    block.AppendColumn("clVol", col_clVol);
    block.AppendColumn("wtNum", col_wtNum);
    block.AppendColumn("zbNum", col_zbNum);
    block.AppendColumn("clNum", col_clNum);
    
    block.AppendColumn("wtBidNumRatio", col_wtBidNumRatio);
    block.AppendColumn("wtAskNumRatio", col_wtAskNumRatio);
    block.AppendColumn("wtBidVolRatio", col_wtBidVolRatio);
    block.AppendColumn("wtAskVolRatio", col_wtAskVolRatio);
    block.AppendColumn("wtBidAvgPos", col_wtBidAvgPos);
    block.AppendColumn("wtAskAvgPos", col_wtAskAvgPos);
    block.AppendColumn("wtABidVol", col_wtABidVol);
    block.AppendColumn("wtAAskVol", col_wtAAskVol);
    block.AppendColumn("wtABidNum", col_wtABidNum);
    block.AppendColumn("wtAAskNum", col_wtAAskNum);
    block.AppendColumn("wtPBidPos", col_wtPBidPos);
    block.AppendColumn("wtPAskPos", col_wtPAskPos);
    block.AppendColumn("wtPBidVol", col_wtPBidVol);
    block.AppendColumn("wtPAskVol", col_wtPAskVol);
    block.AppendColumn("wtPBidNum", col_wtPBidNum);
    block.AppendColumn("wtPAskNum", col_wtPAskNum);
    
    block.AppendColumn("bidMaxPos", col_bidMaxPos);
    block.AppendColumn("askMaxPos", col_askMaxPos);
    block.AppendColumn("bidMaxPrice", col_bidMaxPrice);
    block.AppendColumn("askMaxPrice", col_askMaxPrice);
    block.AppendColumn("bidMaxVol", col_bidMaxVol);
    block.AppendColumn("askMaxVol", col_askMaxVol);
    
    // 委托档位区间
    block.AppendColumn("wtBid1Num", col_wtBid1Num);
    block.AppendColumn("wtBid2_4Num", col_wtBid2_4Num);
    block.AppendColumn("wtBid5_10Num", col_wtBid5_10Num);
    block.AppendColumn("wtBid11InfNum", col_wtBid11InfNum);
    block.AppendColumn("wtBid1Vol", col_wtBid1Vol);
    block.AppendColumn("wtBid2_4Vol", col_wtBid2_4Vol);
    block.AppendColumn("wtBid5_10Vol", col_wtBid5_10Vol);
    block.AppendColumn("wtBid11InfVol", col_wtBid11InfVol);
    block.AppendColumn("wtBid1Amt", col_wtBid1Amt);
    block.AppendColumn("wtBid2_4Amt", col_wtBid2_4Amt);
    block.AppendColumn("wtBid5_10Amt", col_wtBid5_10Amt);
    block.AppendColumn("wtBid11InfAmt", col_wtBid11InfAmt);
    block.AppendColumn("wtBid2_4AvgPrice", col_wtBid2_4AvgPrice);
    block.AppendColumn("wtBid5_10AvgPrice", col_wtBid5_10AvgPrice);
    block.AppendColumn("wtBid11InfAvgPrice", col_wtBid11InfAvgPrice);
    
    block.AppendColumn("wtAsk1Num", col_wtAsk1Num);
    block.AppendColumn("wtAsk2_4Num", col_wtAsk2_4Num);
    block.AppendColumn("wtAsk5_10Num", col_wtAsk5_10Num);
    block.AppendColumn("wtAsk11InfNum", col_wtAsk11InfNum);
    block.AppendColumn("wtAsk1Vol", col_wtAsk1Vol);
    block.AppendColumn("wtAsk2_4Vol", col_wtAsk2_4Vol);
    block.AppendColumn("wtAsk5_10Vol", col_wtAsk5_10Vol);
    block.AppendColumn("wtAsk11InfVol", col_wtAsk11InfVol);
    block.AppendColumn("wtAsk1Amt", col_wtAsk1Amt);
    block.AppendColumn("wtAsk2_4Amt", col_wtAsk2_4Amt);
    block.AppendColumn("wtAsk5_10Amt", col_wtAsk5_10Amt);
    block.AppendColumn("wtAsk11InfAmt", col_wtAsk11InfAmt);
    block.AppendColumn("wtAsk2_4AvgPrice", col_wtAsk2_4AvgPrice);
    block.AppendColumn("wtAsk5_10AvgPrice", col_wtAsk5_10AvgPrice);
    block.AppendColumn("wtAsk11InfAvgPrice", col_wtAsk11InfAvgPrice);
    
    // 撤单档位
    block.AppendColumn("clBid1Num", col_clBid1Num);
    block.AppendColumn("clBid2_4Num", col_clBid2_4Num);
    block.AppendColumn("clBid5_10Num", col_clBid5_10Num);
    block.AppendColumn("clBid1Vol", col_clBid1Vol);
    block.AppendColumn("clBid2_4Vol", col_clBid2_4Vol);
    block.AppendColumn("clBid5_10Vol", col_clBid5_10Vol);
    block.AppendColumn("clBid1Amt", col_clBid1Amt);
    block.AppendColumn("clBid2_4Amt", col_clBid2_4Amt);
    block.AppendColumn("clBid5_10Amt", col_clBid5_10Amt);
    block.AppendColumn("clBid2_4AvgPrice", col_clBid2_4AvgPrice);
    block.AppendColumn("clBid5_10AvgPrice", col_clBid5_10AvgPrice);
    
    block.AppendColumn("clAsk1Num", col_clAsk1Num);
    block.AppendColumn("clAsk2_4Num", col_clAsk2_4Num);
    block.AppendColumn("clAsk5_10Num", col_clAsk5_10Num);
    block.AppendColumn("clAsk1Vol", col_clAsk1Vol);
    block.AppendColumn("clAsk2_4Vol", col_clAsk2_4Vol);
    block.AppendColumn("clAsk5_10Vol", col_clAsk5_10Vol);
    block.AppendColumn("clAsk1Amt", col_clAsk1Amt);
    block.AppendColumn("clAsk2_4Amt", col_clAsk2_4Amt);
    block.AppendColumn("clAsk5_10Amt", col_clAsk5_10Amt);
    block.AppendColumn("clAsk2_4AvgPrice", col_clAsk2_4AvgPrice);
    block.AppendColumn("clAsk5_10AvgPrice", col_clAsk5_10AvgPrice);
    
    // Type分类
    block.AppendColumn("bidType1Vol", col_bidType1Vol);
    block.AppendColumn("bidType2Vol", col_bidType2Vol);
    block.AppendColumn("bidType3Vol", col_bidType3Vol);
    block.AppendColumn("bidType4Vol", col_bidType4Vol);
    block.AppendColumn("bidType5Vol", col_bidType5Vol);
    block.AppendColumn("bidType6Vol", col_bidType6Vol);
    block.AppendColumn("bidType1Num", col_bidType1Num);
    block.AppendColumn("bidType2Num", col_bidType2Num);
    block.AppendColumn("bidType3Num", col_bidType3Num);
    block.AppendColumn("bidType4Num", col_bidType4Num);
    block.AppendColumn("bidType5Num", col_bidType5Num);
    block.AppendColumn("bidType6Num", col_bidType6Num);
    
    block.AppendColumn("askType1Vol", col_askType1Vol);
    block.AppendColumn("askType2Vol", col_askType2Vol);
    block.AppendColumn("askType3Vol", col_askType3Vol);
    block.AppendColumn("askType4Vol", col_askType4Vol);
    block.AppendColumn("askType5Vol", col_askType5Vol);
    block.AppendColumn("askType6Vol", col_askType6Vol);
    block.AppendColumn("askType1Num", col_askType1Num);
    block.AppendColumn("askType2Num", col_askType2Num);
    block.AppendColumn("askType3Num", col_askType3Num);
    block.AppendColumn("askType4Num", col_askType4Num);
    block.AppendColumn("askType5Num", col_askType5Num);
    block.AppendColumn("askType6Num", col_askType6Num);
    
    // Pending订单池
    block.AppendColumn("pendingBid1Num", col_pendingBid1Num);
    block.AppendColumn("pendingBid2_4Num", col_pendingBid2_4Num);
    block.AppendColumn("pendingBid5_10Num", col_pendingBid5_10Num);
    block.AppendColumn("pendingBidTotalNum", col_pendingBidTotalNum);
    block.AppendColumn("pendingBid1Vol", col_pendingBid1Vol);
    block.AppendColumn("pendingBid2_4Vol", col_pendingBid2_4Vol);
    block.AppendColumn("pendingBid5_10Vol", col_pendingBid5_10Vol);
    block.AppendColumn("pendingBidTotalVol", col_pendingBidTotalVol);
    block.AppendColumn("pendingBid1Amt", col_pendingBid1Amt);
    block.AppendColumn("pendingBid2_4Amt", col_pendingBid2_4Amt);
    block.AppendColumn("pendingBid5_10Amt", col_pendingBid5_10Amt);
    block.AppendColumn("pendingBidTotalAmt", col_pendingBidTotalAmt);
    
    block.AppendColumn("pendingAsk1Num", col_pendingAsk1Num);
    block.AppendColumn("pendingAsk2_4Num", col_pendingAsk2_4Num);
    block.AppendColumn("pendingAsk5_10Num", col_pendingAsk5_10Num);
    block.AppendColumn("pendingAskTotalNum", col_pendingAskTotalNum);
    block.AppendColumn("pendingAsk1Vol", col_pendingAsk1Vol);
    block.AppendColumn("pendingAsk2_4Vol", col_pendingAsk2_4Vol);
    block.AppendColumn("pendingAsk5_10Vol", col_pendingAsk5_10Vol);
    block.AppendColumn("pendingAskTotalVol", col_pendingAskTotalVol);
    block.AppendColumn("pendingAsk1Amt", col_pendingAsk1Amt);
    block.AppendColumn("pendingAsk2_4Amt", col_pendingAsk2_4Amt);
    block.AppendColumn("pendingAsk5_10Amt", col_pendingAsk5_10Amt);
    block.AppendColumn("pendingAskTotalAmt", col_pendingAskTotalAmt);
    
    // 10档盘口
    for (int i = 0; i < 10; ++i) {
        block.AppendColumn("bidPrice" + std::to_string(i + 1), col_bidPrice[i]);
        block.AppendColumn("bidVolume" + std::to_string(i + 1), col_bidVolume[i]);
        block.AppendColumn("askPrice" + std::to_string(i + 1), col_askPrice[i]);
        block.AppendColumn("askVolume" + std::to_string(i + 1), col_askVolume[i]);
    }
    
    // 行情汇总
    block.AppendColumn("totalVolume", col_totalVolume);
    block.AppendColumn("totalAmount", col_totalAmount);
    block.AppendColumn("openPrice", col_openPrice);
    block.AppendColumn("highPrice", col_highPrice);
    block.AppendColumn("lowPrice", col_lowPrice);
    block.AppendColumn("lastPrice", col_lastPrice);
    block.AppendColumn("dayHigh", col_dayHigh);
    block.AppendColumn("dayLow", col_dayLow);
    block.AppendColumn("vwap", col_vwap);
    
    // 元数据
    block.AppendColumn("eventCount", col_eventCount);
    block.AppendColumn("dataFlag", col_dataFlag);
    
    // 执行插入
    client_->Insert(table_name, block);
}

// ============================================================
// LobDataProcessor实现
// ============================================================

LobDataProcessor::LobDataProcessor(const ClickHouseConfig& config)
    : ch_config_(config) {}

std::vector<AggregatedData> LobDataProcessor::processH5File(
    const std::string& h5_path,
    const std::string& level2_path,
    const std::time_t& trade_date) {
    
    // HDF5 不是线程安全的，必须串行化所有 H5 读取操作
    // 锁只保护 H5 I/O 部分，聚合计算和 ClickHouse 写入仍然并行
    std::vector<OrderRecord> orders;
    std::vector<TradeRecord> trades;
    std::string stock_code;
    std::unordered_map<int64_t, std::string> order_type_map;
    
    {
        std::lock_guard<std::mutex> lock(g_hdf5_mutex);
        
        // 读取数据 (构造函数中会加载 Level2 数据)
        H5DataReader reader(h5_path, level2_path);
        orders = reader.readOrderData();
        
        if (!orders.empty()) {
            trades = reader.readTradeData();
        }
        
        stock_code = reader.getStockCode();
        order_type_map = reader.getOrderTypeMap();
    }
    // mutex 释放，后续聚合计算可以并行
    
    // 检查数据有效性: 停牌或无数据的股票直接跳过
    if (orders.empty()) {
        std::cerr << "[WARN] No order data (suspended?), skip: " << h5_path << std::endl;
        return {};
    }
    
    // 聚合处理 (纯 CPU 计算，无需持有锁)
    TickAggregator aggregator(
        orders, trades, stock_code, trade_date,
        order_type_map
    );
    auto results = aggregator.aggregateTo3s();
    
    return results;
}

void LobDataProcessor::processAndSave(
    const std::string& h5_path,
    const std::string& level2_path,
    const std::time_t& trade_date,
    bool save_to_ch,
    bool save_to_csv,
    const std::string& table_name,
    int time_start,
    int time_end,
    bool save_to_parquet_shard,
    const std::string& parquet_staging_dir) {

    auto results = processH5File(h5_path, level2_path, trade_date);

    // 无数据(停牌等)直接跳过保存
    if (results.empty()) {
        return;
    }

    // 按时间段过滤: 根据dateTime的HHMMSS部分过滤
    if (time_start >= 0 || time_end >= 0) {
        std::vector<AggregatedData> filtered;
        filtered.reserve(results.size());
        for (const auto& d : results) {
            // dateTime格式: YYYYMMDDHHMMSSMMM (17位)
            // 提取HHMMSS部分转换为秒数
            int64_t hms = (d.dateTime / 1000LL) % 1000000LL;  // HHMMSS
            int h = static_cast<int>(hms / 10000);
            int m = static_cast<int>((hms / 100) % 100);
            int s = static_cast<int>(hms % 100);
            int sec = h * 3600 + m * 60 + s;

            bool in_range = true;
            if (time_start >= 0 && sec < time_start) in_range = false;
            if (time_end >= 0 && sec > time_end) in_range = false;

            if (in_range) {
                filtered.push_back(d);
            }
        }
        results = std::move(filtered);

        if (results.empty()) {
            return;
        }
    }

    if (save_to_csv) {
        std::string csv_path = h5_path;
        size_t pos = csv_path.rfind(".h5");
        if (pos != std::string::npos) {
            csv_path.replace(pos, 3, "_3s.csv");
        } else {
            csv_path += "_3s.csv";
        }

        saveToCSV(results, csv_path);
        std::cout << "  Saved to CSV: " << csv_path << std::endl;
    }

    if (save_to_parquet_shard) {
        // {staging_root}/{YYYYMMDD}/{symbol}.bin
        // 用 results[0].tradedate 而不是入参 trade_date，避免时区导致日期错位
        std::string date_str = Utils::formatDate(results[0].tradedate, "%Y%m%d");
        std::string shard_dir = parquet_staging_dir + "/" + date_str;
        fs::create_directories(shard_dir);

        fs::path p(h5_path);
        std::string symbol = p.stem().string();
        std::string bin_path = shard_dir + "/" + symbol + ".bin";

        try {
            saveToBinaryShard(results, bin_path);
        } catch (const std::exception& e) {
            std::cerr << "  Warning: Failed to save parquet shard: " << e.what() << std::endl;
            throw;
        }
    }

    if (save_to_ch) {
        try {
            ClickHouseManager ch_manager(ch_config_);
            ch_manager.insertData(results, table_name);
        } catch (const std::exception& e) {
            std::cerr << "  Warning: Failed to save to ClickHouse: " << e.what() << std::endl;
        }
    }
}

void LobDataProcessor::saveToCSV(const std::vector<AggregatedData>& data, 
                                 const std::string& csv_path) {
    std::ofstream ofs(csv_path);
    if (!ofs.is_open()) {
        throw std::runtime_error("Failed to open CSV file: " + csv_path);
    }
    
    // 写入CSV头（简化版本）
    ofs << "symbol,dateTime,event_time,wtBidNum,wtAskNum,zbBidNum,zbAskNum,"
        << "wtBidVol,wtAskVol,zbBidVol,zbAskVol,lastPrice,totalVolume,totalAmount,vwap\n";
    
    // 写入数据行
    for (const auto& row : data) {
        ofs << row.symbol << ","
            << row.dateTime << ","
            << row.event_time << ","
            << row.wtBidNum << "," << row.wtAskNum << ","
            << row.zbBidNum << "," << row.zbAskNum << ","
            << row.wtBidVol << "," << row.wtAskVol << ","
            << row.zbBidVol << "," << row.zbAskVol << ","
            << row.lastPrice << ","
            << row.totalVolume << ","
            << row.totalAmount << ","
            << row.vwap << "\n";
    }
    
    ofs.close();
}

// ============================================================
// 二进制分片输出 (供 merge_3s_parquet.py 合并为单文件 parquet)
// ----------------------------------------------------------------
// 格式 (little-endian, 列主序):
//   Header:
//     [0..8)   magic "L3SBIN01"
//     [8..12)  uint32 num_cols
//     [12..16) uint32 num_rows
//     For each col:
//       uint16 name_len
//       name_len bytes  (UTF-8 列名)
//       uint8  type_code  (1=int32, 2=int64, 3=float64)
//   Body:
//     按声明顺序，逐列写 num_rows * sizeof(type) 字节
//
// 列集合与顺序严格对齐 nova/export_tick_3s.py 的 FIELDS 列表（209 列），
// 这样 Python 合并端按 FIELDS 顺序逐列 frombuffer 即可还原 Arrow Table。
// ============================================================
void LobDataProcessor::saveToBinaryShard(const std::vector<AggregatedData>& data,
                                          const std::string& bin_path) {
    if (data.empty()) return;

    fs::create_directories(fs::path(bin_path).parent_path());

    FILE* fp = std::fopen(bin_path.c_str(), "wb");
    if (!fp) {
        throw std::runtime_error("Failed to open shard file: " + bin_path);
    }

    using Writer = std::function<void(FILE*, const std::vector<AggregatedData>&)>;
    struct Column { const char* name; uint8_t type; Writer writer; };

    auto col_i32 = [](const char* n, std::function<int32_t(const AggregatedData&)> g) -> Column {
        return {n, 1, [g](FILE* f, const std::vector<AggregatedData>& d) {
            std::vector<int32_t> buf(d.size());
            for (size_t i = 0; i < d.size(); ++i) buf[i] = g(d[i]);
            std::fwrite(buf.data(), sizeof(int32_t), d.size(), f);
        }};
    };
    auto col_i64 = [](const char* n, std::function<int64_t(const AggregatedData&)> g) -> Column {
        return {n, 2, [g](FILE* f, const std::vector<AggregatedData>& d) {
            std::vector<int64_t> buf(d.size());
            for (size_t i = 0; i < d.size(); ++i) buf[i] = g(d[i]);
            std::fwrite(buf.data(), sizeof(int64_t), d.size(), f);
        }};
    };
    auto col_f64 = [](const char* n, std::function<double(const AggregatedData&)> g) -> Column {
        return {n, 3, [g](FILE* f, const std::vector<AggregatedData>& d) {
            std::vector<double> buf(d.size());
            for (size_t i = 0; i < d.size(); ++i) buf[i] = g(d[i]);
            std::fwrite(buf.data(), sizeof(double), d.size(), f);
        }};
    };

    std::vector<Column> cols = {
        col_f64("tradedate", [](const AggregatedData& r) -> double {
            struct tm tm_info{}; localtime_r(&r.tradedate, &tm_info);
            return (tm_info.tm_year + 1900) * 10000.0 +
                   (tm_info.tm_mon  + 1)   * 100.0   +
                    tm_info.tm_mday;
        }),
        col_i32("instrumentID", [](const AggregatedData& r){ return r.instrumentID; }),
        col_i64("dateTime",     [](const AggregatedData& r){ return r.dateTime; }),
        col_i32("dataFlag",     [](const AggregatedData& r){ return r.dataFlag; }),

        col_i32("wtBidNum", [](const AggregatedData& r){ return r.wtBidNum; }),
        col_i32("wtAskNum", [](const AggregatedData& r){ return r.wtAskNum; }),
        col_i32("zbBidNum", [](const AggregatedData& r){ return r.zbBidNum; }),
        col_i32("zbAskNum", [](const AggregatedData& r){ return r.zbAskNum; }),
        col_i32("clBidNum", [](const AggregatedData& r){ return r.clBidNum; }),
        col_i32("clAskNum", [](const AggregatedData& r){ return r.clAskNum; }),

        col_i64("wtBidVol", [](const AggregatedData& r){ return r.wtBidVol; }),
        col_i64("wtAskVol", [](const AggregatedData& r){ return r.wtAskVol; }),
        col_i64("zbBidVol", [](const AggregatedData& r){ return r.zbBidVol; }),
        col_i64("zbAskVol", [](const AggregatedData& r){ return r.zbAskVol; }),
        col_i64("clBidVol", [](const AggregatedData& r){ return r.clBidVol; }),
        col_i64("clAskVol", [](const AggregatedData& r){ return r.clAskVol; }),

        col_f64("wtBidAmt", [](const AggregatedData& r){ return r.wtBidAmt; }),
        col_f64("wtAskAmt", [](const AggregatedData& r){ return r.wtAskAmt; }),
        col_f64("zbBidAmt", [](const AggregatedData& r){ return r.zbBidAmt; }),
        col_f64("zbAskAmt", [](const AggregatedData& r){ return r.zbAskAmt; }),
        col_f64("clBidAmt", [](const AggregatedData& r){ return r.clBidAmt; }),
        col_f64("clAskAmt", [](const AggregatedData& r){ return r.clAskAmt; }),

        col_f64("wtBidp", [](const AggregatedData& r){ return r.wtBidp; }),
        col_f64("wtAskp", [](const AggregatedData& r){ return r.wtAskp; }),
        col_f64("zbBidp", [](const AggregatedData& r){ return r.zbBidp; }),
        col_f64("zbAskp", [](const AggregatedData& r){ return r.zbAskp; }),
        col_f64("clBidp", [](const AggregatedData& r){ return r.clBidp; }),
        col_f64("clAskp", [](const AggregatedData& r){ return r.clAskp; }),

        col_f64("wtBidp1", [](const AggregatedData& r){ return r.wtBidp1; }),
        col_i64("wtBidv1", [](const AggregatedData& r){ return r.wtBidv1; }),
        col_f64("wtAskp1", [](const AggregatedData& r){ return r.wtAskp1; }),
        col_i64("wtAskv1", [](const AggregatedData& r){ return r.wtAskv1; }),
        col_f64("zbBidp1", [](const AggregatedData& r){ return r.zbBidp1; }),
        col_i64("zbBidv1", [](const AggregatedData& r){ return r.zbBidv1; }),
        col_f64("zbAskp1", [](const AggregatedData& r){ return r.zbAskp1; }),
        col_i64("zbAskv1", [](const AggregatedData& r){ return r.zbAskv1; }),

        col_f64("wtBidWeightPrice", [](const AggregatedData& r){ return r.wtBidWeightPrice; }),
        col_f64("wtAskWeightPrice", [](const AggregatedData& r){ return r.wtAskWeightPrice; }),

        col_i64("wtVol", [](const AggregatedData& r){ return r.wtVol; }),
        col_i32("wtNum", [](const AggregatedData& r){ return r.wtNum; }),
        col_i64("zbVol", [](const AggregatedData& r){ return r.zbVol; }),
        col_i32("zbNum", [](const AggregatedData& r){ return r.zbNum; }),
        col_i64("clVol", [](const AggregatedData& r){ return r.clVol; }),
        col_i32("clNum", [](const AggregatedData& r){ return r.clNum; }),

        col_f64("wtBidNumRatio", [](const AggregatedData& r){ return r.wtBidNumRatio; }),
        col_f64("wtBidVolRatio", [](const AggregatedData& r){ return r.wtBidVolRatio; }),
        col_f64("wtAskNumRatio", [](const AggregatedData& r){ return r.wtAskNumRatio; }),
        col_f64("wtAskVolRatio", [](const AggregatedData& r){ return r.wtAskVolRatio; }),

        col_f64("wtBidAvgPos", [](const AggregatedData& r){ return r.wtBidAvgPos; }),
        col_f64("wtAskAvgPos", [](const AggregatedData& r){ return r.wtAskAvgPos; }),

        col_i64("wtABidVol", [](const AggregatedData& r){ return r.wtABidVol; }),
        col_i32("wtABidNum", [](const AggregatedData& r){ return r.wtABidNum; }),
        col_i64("wtAAskVol", [](const AggregatedData& r){ return r.wtAAskVol; }),
        col_i32("wtAAskNum", [](const AggregatedData& r){ return r.wtAAskNum; }),

        col_f64("wtPBidPos", [](const AggregatedData& r){ return r.wtPBidPos; }),
        col_i64("wtPBidVol", [](const AggregatedData& r){ return r.wtPBidVol; }),
        col_i32("wtPBidNum", [](const AggregatedData& r){ return r.wtPBidNum; }),
        col_f64("wtPAskPos", [](const AggregatedData& r){ return r.wtPAskPos; }),
        col_i64("wtPAskVol", [](const AggregatedData& r){ return r.wtPAskVol; }),
        col_i32("wtPAskNum", [](const AggregatedData& r){ return r.wtPAskNum; }),

        col_i32("bidMaxPos",   [](const AggregatedData& r){ return r.bidMaxPos; }),
        col_f64("bidMaxPrice", [](const AggregatedData& r){ return r.bidMaxPrice; }),
        col_i64("bidMaxVol",   [](const AggregatedData& r){ return r.bidMaxVol; }),
        col_i32("askMaxPos",   [](const AggregatedData& r){ return r.askMaxPos; }),
        col_f64("askMaxPrice", [](const AggregatedData& r){ return r.askMaxPrice; }),
        col_i64("askMaxVol",   [](const AggregatedData& r){ return r.askMaxVol; }),

        col_i32("wtBid1Num", [](const AggregatedData& r){ return r.wtBid1Num; }),
        col_i64("wtBid1Vol", [](const AggregatedData& r){ return r.wtBid1Vol; }),
        col_f64("wtBid1Amt", [](const AggregatedData& r){ return r.wtBid1Amt; }),
        col_i32("wtAsk1Num", [](const AggregatedData& r){ return r.wtAsk1Num; }),
        col_i64("wtAsk1Vol", [](const AggregatedData& r){ return r.wtAsk1Vol; }),
        col_f64("wtAsk1Amt", [](const AggregatedData& r){ return r.wtAsk1Amt; }),

        col_i32("wtBid2_4Num",      [](const AggregatedData& r){ return r.wtBid2_4Num; }),
        col_i64("wtBid2_4Vol",      [](const AggregatedData& r){ return r.wtBid2_4Vol; }),
        col_f64("wtBid2_4AvgPrice", [](const AggregatedData& r){ return r.wtBid2_4AvgPrice; }),
        col_f64("wtBid2_4Amt",      [](const AggregatedData& r){ return r.wtBid2_4Amt; }),
        col_i32("wtAsk2_4Num",      [](const AggregatedData& r){ return r.wtAsk2_4Num; }),
        col_i64("wtAsk2_4Vol",      [](const AggregatedData& r){ return r.wtAsk2_4Vol; }),
        col_f64("wtAsk2_4AvgPrice", [](const AggregatedData& r){ return r.wtAsk2_4AvgPrice; }),
        col_f64("wtAsk2_4Amt",      [](const AggregatedData& r){ return r.wtAsk2_4Amt; }),

        col_i32("wtBid5_10Num",      [](const AggregatedData& r){ return r.wtBid5_10Num; }),
        col_i64("wtBid5_10Vol",      [](const AggregatedData& r){ return r.wtBid5_10Vol; }),
        col_f64("wtBid5_10AvgPrice", [](const AggregatedData& r){ return r.wtBid5_10AvgPrice; }),
        col_f64("wtBid5_10Amt",      [](const AggregatedData& r){ return r.wtBid5_10Amt; }),
        col_i32("wtAsk5_10Num",      [](const AggregatedData& r){ return r.wtAsk5_10Num; }),
        col_i64("wtAsk5_10Vol",      [](const AggregatedData& r){ return r.wtAsk5_10Vol; }),
        col_f64("wtAsk5_10AvgPrice", [](const AggregatedData& r){ return r.wtAsk5_10AvgPrice; }),
        col_f64("wtAsk5_10Amt",      [](const AggregatedData& r){ return r.wtAsk5_10Amt; }),

        col_i32("wtBid11InfNum",      [](const AggregatedData& r){ return r.wtBid11InfNum; }),
        col_i64("wtBid11InfVol",      [](const AggregatedData& r){ return r.wtBid11InfVol; }),
        col_f64("wtBid11InfAvgPrice", [](const AggregatedData& r){ return r.wtBid11InfAvgPrice; }),
        col_f64("wtBid11InfAmt",      [](const AggregatedData& r){ return r.wtBid11InfAmt; }),
        col_i32("wtAsk11InfNum",      [](const AggregatedData& r){ return r.wtAsk11InfNum; }),
        col_i64("wtAsk11InfVol",      [](const AggregatedData& r){ return r.wtAsk11InfVol; }),
        col_f64("wtAsk11InfAvgPrice", [](const AggregatedData& r){ return r.wtAsk11InfAvgPrice; }),
        col_f64("wtAsk11InfAmt",      [](const AggregatedData& r){ return r.wtAsk11InfAmt; }),

        col_i32("pendingBid1Num", [](const AggregatedData& r){ return r.pendingBid1Num; }),
        col_i64("pendingBid1Vol", [](const AggregatedData& r){ return r.pendingBid1Vol; }),
        col_f64("pendingBid1Amt", [](const AggregatedData& r){ return r.pendingBid1Amt; }),
        col_i32("pendingAsk1Num", [](const AggregatedData& r){ return r.pendingAsk1Num; }),
        col_i64("pendingAsk1Vol", [](const AggregatedData& r){ return r.pendingAsk1Vol; }),
        col_f64("pendingAsk1Amt", [](const AggregatedData& r){ return r.pendingAsk1Amt; }),

        col_i32("pendingBid2_4Num", [](const AggregatedData& r){ return r.pendingBid2_4Num; }),
        col_i64("pendingBid2_4Vol", [](const AggregatedData& r){ return r.pendingBid2_4Vol; }),
        col_f64("pendingBid2_4Amt", [](const AggregatedData& r){ return r.pendingBid2_4Amt; }),
        col_i32("pendingAsk2_4Num", [](const AggregatedData& r){ return r.pendingAsk2_4Num; }),
        col_i64("pendingAsk2_4Vol", [](const AggregatedData& r){ return r.pendingAsk2_4Vol; }),
        col_f64("pendingAsk2_4Amt", [](const AggregatedData& r){ return r.pendingAsk2_4Amt; }),

        col_i32("pendingBid5_10Num", [](const AggregatedData& r){ return r.pendingBid5_10Num; }),
        col_i64("pendingBid5_10Vol", [](const AggregatedData& r){ return r.pendingBid5_10Vol; }),
        col_f64("pendingBid5_10Amt", [](const AggregatedData& r){ return r.pendingBid5_10Amt; }),
        col_i32("pendingAsk5_10Num", [](const AggregatedData& r){ return r.pendingAsk5_10Num; }),
        col_i64("pendingAsk5_10Vol", [](const AggregatedData& r){ return r.pendingAsk5_10Vol; }),
        col_f64("pendingAsk5_10Amt", [](const AggregatedData& r){ return r.pendingAsk5_10Amt; }),

        col_i32("pendingBidTotalNum", [](const AggregatedData& r){ return r.pendingBidTotalNum; }),
        col_i64("pendingBidTotalVol", [](const AggregatedData& r){ return r.pendingBidTotalVol; }),
        col_f64("pendingBidTotalAmt", [](const AggregatedData& r){ return r.pendingBidTotalAmt; }),
        col_i32("pendingAskTotalNum", [](const AggregatedData& r){ return r.pendingAskTotalNum; }),
        col_i64("pendingAskTotalVol", [](const AggregatedData& r){ return r.pendingAskTotalVol; }),
        col_f64("pendingAskTotalAmt", [](const AggregatedData& r){ return r.pendingAskTotalAmt; }),

        col_i32("clBid1Num", [](const AggregatedData& r){ return r.clBid1Num; }),
        col_i64("clBid1Vol", [](const AggregatedData& r){ return r.clBid1Vol; }),
        col_f64("clBid1Amt", [](const AggregatedData& r){ return r.clBid1Amt; }),
        col_i32("clAsk1Num", [](const AggregatedData& r){ return r.clAsk1Num; }),
        col_i64("clAsk1Vol", [](const AggregatedData& r){ return r.clAsk1Vol; }),
        col_f64("clAsk1Amt", [](const AggregatedData& r){ return r.clAsk1Amt; }),

        col_i32("clBid2_4Num",      [](const AggregatedData& r){ return r.clBid2_4Num; }),
        col_i64("clBid2_4Vol",      [](const AggregatedData& r){ return r.clBid2_4Vol; }),
        col_f64("clBid2_4AvgPrice", [](const AggregatedData& r){ return r.clBid2_4AvgPrice; }),
        col_f64("clBid2_4Amt",      [](const AggregatedData& r){ return r.clBid2_4Amt; }),
        col_i32("clAsk2_4Num",      [](const AggregatedData& r){ return r.clAsk2_4Num; }),
        col_i64("clAsk2_4Vol",      [](const AggregatedData& r){ return r.clAsk2_4Vol; }),
        col_f64("clAsk2_4AvgPrice", [](const AggregatedData& r){ return r.clAsk2_4AvgPrice; }),
        col_f64("clAsk2_4Amt",      [](const AggregatedData& r){ return r.clAsk2_4Amt; }),

        col_i32("clBid5_10Num",      [](const AggregatedData& r){ return r.clBid5_10Num; }),
        col_i64("clBid5_10Vol",      [](const AggregatedData& r){ return r.clBid5_10Vol; }),
        col_f64("clBid5_10AvgPrice", [](const AggregatedData& r){ return r.clBid5_10AvgPrice; }),
        col_f64("clBid5_10Amt",      [](const AggregatedData& r){ return r.clBid5_10Amt; }),
        col_i32("clAsk5_10Num",      [](const AggregatedData& r){ return r.clAsk5_10Num; }),
        col_i64("clAsk5_10Vol",      [](const AggregatedData& r){ return r.clAsk5_10Vol; }),
        col_f64("clAsk5_10AvgPrice", [](const AggregatedData& r){ return r.clAsk5_10AvgPrice; }),
        col_f64("clAsk5_10Amt",      [](const AggregatedData& r){ return r.clAsk5_10Amt; }),

        col_i64("bidType1Vol", [](const AggregatedData& r){ return r.bidType1Vol; }),
        col_i64("bidType2Vol", [](const AggregatedData& r){ return r.bidType2Vol; }),
        col_i64("bidType3Vol", [](const AggregatedData& r){ return r.bidType3Vol; }),
        col_i64("bidType4Vol", [](const AggregatedData& r){ return r.bidType4Vol; }),
        col_i64("bidType5Vol", [](const AggregatedData& r){ return r.bidType5Vol; }),
        col_i64("bidType6Vol", [](const AggregatedData& r){ return r.bidType6Vol; }),
        col_i64("askType1Vol", [](const AggregatedData& r){ return r.askType1Vol; }),
        col_i64("askType2Vol", [](const AggregatedData& r){ return r.askType2Vol; }),
        col_i64("askType3Vol", [](const AggregatedData& r){ return r.askType3Vol; }),
        col_i64("askType4Vol", [](const AggregatedData& r){ return r.askType4Vol; }),
        col_i64("askType5Vol", [](const AggregatedData& r){ return r.askType5Vol; }),
        col_i64("askType6Vol", [](const AggregatedData& r){ return r.askType6Vol; }),
        col_i32("bidType1Num", [](const AggregatedData& r){ return r.bidType1Num; }),
        col_i32("bidType2Num", [](const AggregatedData& r){ return r.bidType2Num; }),
        col_i32("bidType3Num", [](const AggregatedData& r){ return r.bidType3Num; }),
        col_i32("bidType4Num", [](const AggregatedData& r){ return r.bidType4Num; }),
        col_i32("bidType5Num", [](const AggregatedData& r){ return r.bidType5Num; }),
        col_i32("bidType6Num", [](const AggregatedData& r){ return r.bidType6Num; }),
        col_i32("askType1Num", [](const AggregatedData& r){ return r.askType1Num; }),
        col_i32("askType2Num", [](const AggregatedData& r){ return r.askType2Num; }),
        col_i32("askType3Num", [](const AggregatedData& r){ return r.askType3Num; }),
        col_i32("askType4Num", [](const AggregatedData& r){ return r.askType4Num; }),
        col_i32("askType5Num", [](const AggregatedData& r){ return r.askType5Num; }),
        col_i32("askType6Num", [](const AggregatedData& r){ return r.askType6Num; }),

        // bidPriceN/bidVolumeN 交错（与 FIELDS 一致）
        col_f64("bidPrice1",  [](const AggregatedData& r){ return r.bidPrice[0]; }),
        col_i64("bidVolume1", [](const AggregatedData& r){ return r.bidVolume[0]; }),
        col_f64("bidPrice2",  [](const AggregatedData& r){ return r.bidPrice[1]; }),
        col_i64("bidVolume2", [](const AggregatedData& r){ return r.bidVolume[1]; }),
        col_f64("bidPrice3",  [](const AggregatedData& r){ return r.bidPrice[2]; }),
        col_i64("bidVolume3", [](const AggregatedData& r){ return r.bidVolume[2]; }),
        col_f64("bidPrice4",  [](const AggregatedData& r){ return r.bidPrice[3]; }),
        col_i64("bidVolume4", [](const AggregatedData& r){ return r.bidVolume[3]; }),
        col_f64("bidPrice5",  [](const AggregatedData& r){ return r.bidPrice[4]; }),
        col_i64("bidVolume5", [](const AggregatedData& r){ return r.bidVolume[4]; }),
        col_f64("bidPrice6",  [](const AggregatedData& r){ return r.bidPrice[5]; }),
        col_i64("bidVolume6", [](const AggregatedData& r){ return r.bidVolume[5]; }),
        col_f64("bidPrice7",  [](const AggregatedData& r){ return r.bidPrice[6]; }),
        col_i64("bidVolume7", [](const AggregatedData& r){ return r.bidVolume[6]; }),
        col_f64("bidPrice8",  [](const AggregatedData& r){ return r.bidPrice[7]; }),
        col_i64("bidVolume8", [](const AggregatedData& r){ return r.bidVolume[7]; }),
        col_f64("bidPrice9",  [](const AggregatedData& r){ return r.bidPrice[8]; }),
        col_i64("bidVolume9", [](const AggregatedData& r){ return r.bidVolume[8]; }),
        col_f64("bidPrice10", [](const AggregatedData& r){ return r.bidPrice[9]; }),
        col_i64("bidVolume10",[](const AggregatedData& r){ return r.bidVolume[9]; }),

        col_f64("askPrice1",  [](const AggregatedData& r){ return r.askPrice[0]; }),
        col_i64("askVolume1", [](const AggregatedData& r){ return r.askVolume[0]; }),
        col_f64("askPrice2",  [](const AggregatedData& r){ return r.askPrice[1]; }),
        col_i64("askVolume2", [](const AggregatedData& r){ return r.askVolume[1]; }),
        col_f64("askPrice3",  [](const AggregatedData& r){ return r.askPrice[2]; }),
        col_i64("askVolume3", [](const AggregatedData& r){ return r.askVolume[2]; }),
        col_f64("askPrice4",  [](const AggregatedData& r){ return r.askPrice[3]; }),
        col_i64("askVolume4", [](const AggregatedData& r){ return r.askVolume[3]; }),
        col_f64("askPrice5",  [](const AggregatedData& r){ return r.askPrice[4]; }),
        col_i64("askVolume5", [](const AggregatedData& r){ return r.askVolume[4]; }),
        col_f64("askPrice6",  [](const AggregatedData& r){ return r.askPrice[5]; }),
        col_i64("askVolume6", [](const AggregatedData& r){ return r.askVolume[5]; }),
        col_f64("askPrice7",  [](const AggregatedData& r){ return r.askPrice[6]; }),
        col_i64("askVolume7", [](const AggregatedData& r){ return r.askVolume[6]; }),
        col_f64("askPrice8",  [](const AggregatedData& r){ return r.askPrice[7]; }),
        col_i64("askVolume8", [](const AggregatedData& r){ return r.askVolume[7]; }),
        col_f64("askPrice9",  [](const AggregatedData& r){ return r.askPrice[8]; }),
        col_i64("askVolume9", [](const AggregatedData& r){ return r.askVolume[8]; }),
        col_f64("askPrice10", [](const AggregatedData& r){ return r.askPrice[9]; }),
        col_i64("askVolume10",[](const AggregatedData& r){ return r.askVolume[9]; }),

        col_i64("totalVolume", [](const AggregatedData& r){ return r.totalVolume; }),
        col_f64("totalAmount", [](const AggregatedData& r){ return r.totalAmount; }),
        col_f64("highPrice",   [](const AggregatedData& r){ return r.highPrice; }),
        col_f64("lowPrice",    [](const AggregatedData& r){ return r.lowPrice; }),
        col_f64("dayHigh",     [](const AggregatedData& r){ return r.dayHigh; }),
        col_f64("dayLow",      [](const AggregatedData& r){ return r.dayLow; }),
        col_f64("openPrice",   [](const AggregatedData& r){ return r.openPrice; }),
        col_f64("lastPrice",   [](const AggregatedData& r){ return r.lastPrice; }),
        col_f64("vwap",        [](const AggregatedData& r){ return r.vwap; }),
        col_i32("eventCount",  [](const AggregatedData& r){ return r.eventCount; }),
    };

    // ---- Header ----
    std::fwrite("L3SBIN01", 1, 8, fp);
    uint32_t num_cols = static_cast<uint32_t>(cols.size());
    uint32_t num_rows = static_cast<uint32_t>(data.size());
    std::fwrite(&num_cols, sizeof(uint32_t), 1, fp);
    std::fwrite(&num_rows, sizeof(uint32_t), 1, fp);
    for (const auto& c : cols) {
        uint16_t nlen = static_cast<uint16_t>(std::strlen(c.name));
        std::fwrite(&nlen, sizeof(uint16_t), 1, fp);
        std::fwrite(c.name, 1, nlen, fp);
        std::fwrite(&c.type, sizeof(uint8_t), 1, fp);
    }

    // ---- Body ----
    for (const auto& c : cols) {
        c.writer(fp, data);
    }

    std::fclose(fp);
}

// 省略processDate、processDateRange等函数的完整实现
// 这些函数涉及文件遍历、多线程处理等逻辑，参考main.cpp中的实现

// ============================================================
// Utils命名空间实现
// ============================================================

namespace Utils {

std::time_t parseDate(const std::string& date_str) {
    std::string clean_str = date_str;
    clean_str.erase(std::remove(clean_str.begin(), clean_str.end(), '-'), clean_str.end());
    
    if (clean_str.length() != 8) {
        throw std::runtime_error("Invalid date format: " + date_str);
    }
    
    std::tm tm = {};
    tm.tm_year = std::stoi(clean_str.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(clean_str.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(clean_str.substr(6, 2));
    tm.tm_hour = 12;  // 设置为中午12点，避免时区边界问题
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;  // 让系统自动判断夏令时
    
    return std::mktime(&tm);
}

std::string formatDate(const std::time_t& t, const char* format) {
    std::tm* tm = std::localtime(&t);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), format, tm);
    return std::string(buffer);
}

bool isWeekend(const std::time_t& t) {
    std::tm* tm = std::localtime(&t);
    return (tm->tm_wday == 0 || tm->tm_wday == 6);
}

std::vector<std::string> getH5Files(const std::string& dir_path) {
    std::vector<std::string> h5_files;
    
    if (!fs::exists(dir_path)) {
        return h5_files;
    }
    
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".h5") {
            h5_files.push_back(entry.path().string());
        }
    }
    
    std::sort(h5_files.begin(), h5_files.end());
    return h5_files;
}

} // namespace Utils
