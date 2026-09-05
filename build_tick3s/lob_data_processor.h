/**
 * LOB数据3秒降频处理模块 - C++实现
 * 
 * 功能：将H5格式的逐笔数据降频为3秒切片数据，保存到ClickHouse
 * 
 * 对应Python版本: Lob_data_save_sh.py
 * 作者：MX Team
 * 日期：2026-02-05
 */

#ifndef LOB_DATA_PROCESSOR_H
#define LOB_DATA_PROCESSOR_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <ctime>
#include <optional>
#include <array>
#include <deque>

// 前向声明
namespace H5 {
    class H5File;
    class DataSet;
}

namespace clickhouse {
    class Client;
}

// ============================================================
// 常量定义
// ============================================================

namespace Constants {
    // 集合竞价时间段
    constexpr int CALL_AUCTION_MORNING_START = 33300;  // 09:15
    constexpr int CALL_AUCTION_MORNING_END = 33900;    // 09:25
    constexpr int CALL_AUCTION_CLOSE_START = 53820;    // 14:57
    constexpr int CALL_AUCTION_CLOSE_END = 54000;      // 15:00
    
    // 连续竞价时间段
    constexpr int CONTINUOUS_MORNING_START = 34200;    // 09:30
    constexpr int CONTINUOUS_MORNING_END = 41400;      // 11:30
    constexpr int CONTINUOUS_AFTERNOON_START = 46800;  // 13:00
    constexpr int CONTINUOUS_AFTERNOON_END = 53820;    // 14:57
    
    // 10档盘口
    constexpr int LOB_LEVELS = 10;
}

// ============================================================
// 数据结构定义
// ============================================================

// 订单记录结构
struct OrderRecord {
    int64_t sysid;                      // 订单号
    std::string ordertype;              // wt/zb/cl
    std::string direction;              // B/S
    std::string time;                   // 时间字符串
    double time_seconds;                // 秒数
    int time_slot;                      // 时间槽(秒)
    double price;                       // 价格
    int64_t volume;                     // 数量
    double amount;                      // 金额
    int position;                       // 档位位置
    int ask_position;                   // 卖方档位
    int bid_position;                   // 买方档位
    int type;                           // 类型
    
    // 盘口数据 (10档)
    std::array<double, Constants::LOB_LEVELS> bdp;    // 买入价格
    std::array<int64_t, Constants::LOB_LEVELS> bdv;   // 买入数量
    std::array<double, Constants::LOB_LEVELS> akp;    // 卖出价格
    std::array<int64_t, Constants::LOB_LEVELS> akv;   // 卖出数量
    std::array<int64_t, Constants::LOB_LEVELS> bid_num;   // 各档挂单笔数(精确, builder 导出)
    std::array<int64_t, Constants::LOB_LEVELS> ask_num;

    // 事件后盘口: LOB H5 行内盘口是 pre-event; 3s 快照需要 post-event。
    // 读取后由 TickAggregator 用下一条记录的 pre-event 盘口填充。
    std::array<double, Constants::LOB_LEVELS> post_bdp;
    std::array<int64_t, Constants::LOB_LEVELS> post_bdv;
    std::array<double, Constants::LOB_LEVELS> post_akp;
    std::array<int64_t, Constants::LOB_LEVELS> post_akv;
    std::array<int64_t, Constants::LOB_LEVELS> post_bid_num;
    std::array<int64_t, Constants::LOB_LEVELS> post_ask_num;

    int64_t tradevolume;                // 累计成交量

    OrderRecord() : sysid(0), time_seconds(0.0), time_slot(0), price(0.0),
                   volume(0), amount(0.0), position(0), ask_position(0),
                   bid_position(0), type(0), tradevolume(0) {
        bdp.fill(0.0);
        bdv.fill(0);
        akp.fill(0.0);
        akv.fill(0);
        bid_num.fill(0);
        ask_num.fill(0);
        post_bdp.fill(0.0);
        post_bdv.fill(0);
        post_akp.fill(0.0);
        post_akv.fill(0);
        post_bid_num.fill(0);
        post_ask_num.fill(0);
    }
};

// 成交记录结构
struct TradeRecord {
    int64_t sysid;
    double trans_ss;        // 成交时间秒数
    int time_slot;
    std::string direction;  // B/S
    
    TradeRecord() : sysid(0), trans_ss(0.0), time_slot(0) {}
};

// Pending订单结构
struct PendingOrder {
    std::string direction;  // B/S
    int64_t volume;
    double price;
    double amount;
    int position;
    
    PendingOrder() : volume(0), price(0.0), amount(0.0), position(0) {}
    PendingOrder(const std::string& dir, int64_t vol, double p, double amt, int pos)
        : direction(dir), volume(vol), price(p), amount(amt), position(pos) {}
};

// 3秒聚合结果结构
struct AggregatedData {
    // 基础信息
    std::time_t tradedate;
    int instrumentID;
    std::string symbol;
    std::string event_time;     // YYYY-MM-DD HH:MM:SS
    int64_t dateTime;           // 17位: YYYYMMDDHHMMSSMMM
    
    // 基础统计: wt/zb/cl × Bid/Ask
    int wtBidNum, wtAskNum, zbBidNum, zbAskNum, clBidNum, clAskNum;
    int64_t wtBidVol, wtAskVol, zbBidVol, zbAskVol, clBidVol, clAskVol;
    double wtBidAmt, wtAskAmt, zbBidAmt, zbAskAmt, clBidAmt, clAskAmt;
    double wtBidp, wtAskp, zbBidp, zbAskp, clBidp, clAskp;
    
    // 最新价量
    double wtBidp1, wtAskp1, zbBidp1, zbAskp1;
    int64_t wtBidv1, wtAskv1, zbBidv1, zbAskv1;
    
    // 量加权价
    double wtBidWeightPrice, wtAskWeightPrice;
    
    // 总计
    int64_t wtVol, zbVol, clVol;
    int wtNum, zbNum, clNum;
    
    // 位置统计
    double wtBidNumRatio, wtAskNumRatio;
    double wtBidVolRatio, wtAskVolRatio;
    double wtBidAvgPos, wtAskAvgPos;
    int64_t wtABidVol, wtAAskVol;
    int wtABidNum, wtAAskNum;
    double wtPBidPos, wtPAskPos;
    int64_t wtPBidVol, wtPAskVol;
    int wtPBidNum, wtPAskNum;
    
    // 最大档位
    int bidMaxPos, askMaxPos;
    double bidMaxPrice, askMaxPrice;
    int64_t bidMaxVol, askMaxVol;
    
    // 委托档位区间 (1, 2-4, 5-10, 11+)
    int wtBid1Num, wtBid2_4Num, wtBid5_10Num, wtBid11InfNum;
    int64_t wtBid1Vol, wtBid2_4Vol, wtBid5_10Vol, wtBid11InfVol;
    double wtBid1Amt, wtBid2_4Amt, wtBid5_10Amt, wtBid11InfAmt;
    double wtBid2_4AvgPrice, wtBid5_10AvgPrice, wtBid11InfAvgPrice;
    
    int wtAsk1Num, wtAsk2_4Num, wtAsk5_10Num, wtAsk11InfNum;
    int64_t wtAsk1Vol, wtAsk2_4Vol, wtAsk5_10Vol, wtAsk11InfVol;
    double wtAsk1Amt, wtAsk2_4Amt, wtAsk5_10Amt, wtAsk11InfAmt;
    double wtAsk2_4AvgPrice, wtAsk5_10AvgPrice, wtAsk11InfAvgPrice;
    
    // 撤单档位 (1, 2-4, 5-10)
    int clBid1Num, clBid2_4Num, clBid5_10Num;
    int64_t clBid1Vol, clBid2_4Vol, clBid5_10Vol;
    double clBid1Amt, clBid2_4Amt, clBid5_10Amt;
    double clBid2_4AvgPrice, clBid5_10AvgPrice;
    
    int clAsk1Num, clAsk2_4Num, clAsk5_10Num;
    int64_t clAsk1Vol, clAsk2_4Vol, clAsk5_10Vol;
    double clAsk1Amt, clAsk2_4Amt, clAsk5_10Amt;
    double clAsk2_4AvgPrice, clAsk5_10AvgPrice;
    
    // Type分类 (1-6)
    int64_t bidType1Vol, bidType2Vol, bidType3Vol, bidType4Vol, bidType5Vol, bidType6Vol;
    int bidType1Num, bidType2Num, bidType3Num, bidType4Num, bidType5Num, bidType6Num;
    int64_t askType1Vol, askType2Vol, askType3Vol, askType4Vol, askType5Vol, askType6Vol;
    int askType1Num, askType2Num, askType3Num, askType4Num, askType5Num, askType6Num;
    
    // Pending订单池统计
    int pendingBid1Num, pendingBid2_4Num, pendingBid5_10Num, pendingBidTotalNum;
    int64_t pendingBid1Vol, pendingBid2_4Vol, pendingBid5_10Vol, pendingBidTotalVol;
    double pendingBid1Amt, pendingBid2_4Amt, pendingBid5_10Amt, pendingBidTotalAmt;
    
    int pendingAsk1Num, pendingAsk2_4Num, pendingAsk5_10Num, pendingAskTotalNum;
    int64_t pendingAsk1Vol, pendingAsk2_4Vol, pendingAsk5_10Vol, pendingAskTotalVol;
    double pendingAsk1Amt, pendingAsk2_4Amt, pendingAsk5_10Amt, pendingAskTotalAmt;
    
    // 10档盘口
    std::array<double, Constants::LOB_LEVELS> bidPrice;
    std::array<int64_t, Constants::LOB_LEVELS> bidVolume;
    std::array<double, Constants::LOB_LEVELS> askPrice;
    std::array<int64_t, Constants::LOB_LEVELS> askVolume;
    
    // 行情汇总
    int64_t totalVolume;
    double totalAmount;
    double openPrice, highPrice, lowPrice, lastPrice;
    double dayHigh, dayLow;
    double vwap;
    
    // 元数据
    int eventCount;
    int dataFlag;  // 0=正常, 1=集合竞价, 2=无数据变动
    
    // 构造函数初始化所有字段
    AggregatedData();
};

// ============================================================
// H5数据读取器类
// ============================================================

class H5DataReader {
public:
    H5DataReader(const std::string& h5_path, const std::string& level2_path = "");
    ~H5DataReader();
    
    std::vector<OrderRecord> readOrderData();
    std::vector<TradeRecord> readTradeData();
    
    std::string getStockCode() const { return stock_code_; }
    const std::unordered_map<int64_t, std::string>& getOrderTypeMap() const { 
        return level2_order_type_map_; 
    }
    
private:
    std::string h5_path_;
    std::string level2_path_;
    std::string stock_code_;
    
    // Level2数据缓存
    std::unordered_map<int64_t, double> level2_order_price_map_;
    std::unordered_map<int64_t, std::string> level2_order_type_map_;
    std::unordered_map<int64_t, int64_t> level2_trade_buy_map_;
    std::unordered_map<int64_t, int64_t> level2_trade_sell_map_;
    std::unordered_map<int64_t, double> level2_trade_price_map_;
    
    void loadLevel2Data();
    void fixZeroPrice(std::vector<OrderRecord>& orders);
    bool isMarketOrder(int64_t order_no) const;
    double timeStringToSeconds(const std::string& time_str) const;
    
    // PyTables格式支持
    std::unordered_map<std::string, int> readPyTablesColumns(H5::DataSet& dataset);
    std::vector<OrderRecord> readPyTablesOrderData(H5::H5File& file,
                                                   H5::DataSet& dataset, 
                                                   const std::unordered_map<std::string, int>& column_map);
};

// ============================================================
// 3秒数据聚合器类
// ============================================================

class TickAggregator {
public:
    TickAggregator(const std::vector<OrderRecord>& orders,
                   const std::vector<TradeRecord>& trades,
                   const std::string& stock_code,
                   const std::time_t& trade_date,
                   const std::unordered_map<int64_t, std::string>& order_type_map);
    
    std::vector<AggregatedData> aggregateTo3s();
    
private:
    std::vector<OrderRecord> order_df_;
    std::vector<TradeRecord> trade_df_;
    std::string stock_code_;
    std::time_t trade_date_;
    std::unordered_map<int64_t, std::string> order_type_map_;
    
    // 订单池：跟踪未成交订单 (竞价段使用; 连续段收盘竞价入口处由守恒簿重建)
    std::unordered_map<int64_t, PendingOrder> pending_orders_;

    // ---- 连续段 价位+时间(FIFO) 守恒挂单簿 (路径A) ----
    // 旧法按 zb.sysid 扣减永远落空(zb.sysid=成交序号, 非对手挂单号) -> 挂单池虚高,
    // 故连续段改为价位守恒: 成交一定发生在被动方价位, 按"成交价 + 时间(FIFO)"扣减,
    // 完全不依赖 sysid。两簿均以 intPrice(分) 升序存储:
    //   买簿最优 = 最大键(rbegin); 卖簿最优 = 最小键(begin)。
    // 同一价位内按到达顺序(时间优先)排队, 成交从队首(最旧)扣。
    struct RestingOrder { int64_t sysid; int64_t vol; double price; double amount; };
    std::map<int64_t, std::deque<RestingOrder>> cont_bid_book_;
    std::map<int64_t, std::deque<RestingOrder>> cont_ask_book_;
    // 连续段 pos=0 主动单入簿追踪: sysid -> price_int, 供 contReduceByTrade 按 sysid 精确扣减。
    // type=2 (最优五档剩余转限价) 初始 bp=ap=0 → position=0, 部分成交后余量变被动,
    // 须计入全深度挂单总量; type=3 则余量自动撤销, 配对 cl 后净为零, 同样正确。
    std::unordered_map<int64_t, int64_t> bid_sysid_price_;
    std::unordered_map<int64_t, int64_t> ask_sysid_price_;

    // 连续段 pendingTotalNum/Vol/Amt 均从全深度 FIFO 守恒簿当前剩余订单汇总,
    // 与十档分档 pending 区分: Total 是全深度, 1/2-4/5-10 是快照十档。

    // 集合竞价 uncross 结果缓存 (fillAuctionPending 每槽更新, calcTypeStats 复用以
    // 按清算价重分类竞价段 type)
    bool auction_has_clearing_ = false;
    int64_t auction_clearing_intp_ = 0;
    int64_t auction_res_best_bid_intp_ = 0;
    int64_t auction_res_best_ask_intp_ = 0;

    // 老建造器幽灵: 开盘竞价撮合时, 老版本以各单自身限价计入 running total 但以统一清算价扣减,
    // 导致 pendingBidTotalAmt 偏高 / pendingAskTotalAmt 偏低。
    // phantom_bid_  = Σ(bid_limit - clearing_price) × matched_vol  (正值, 加到 Total Bid Amt)
    // phantom_ask_  = Σ(clearing_price - ask_limit) × matched_vol  (正值, 从 Total Ask Amt 扣)
    double auction_phantom_bid_amt_ = 0.0;
    double auction_phantom_ask_amt_ = 0.0;

    // 上一秒的盘口数据
    std::array<double, Constants::LOB_LEVELS> last_lob_bid_price_;
    std::array<int64_t, Constants::LOB_LEVELS> last_lob_bid_volume_;
    std::array<double, Constants::LOB_LEVELS> last_lob_ask_price_;
    std::array<int64_t, Constants::LOB_LEVELS> last_lob_ask_volume_;
    // 上一秒各档挂单笔数(精确, 取自 builder 导出的 bid_num/ask_num; 随快照一同进位)
    std::array<int64_t, Constants::LOB_LEVELS> last_lob_bid_num_{};
    std::array<int64_t, Constants::LOB_LEVELS> last_lob_ask_num_{};
    
    // 上一秒的市场数据
    double last_price_;
    double last_vwap_;
    int64_t last_total_volume_;
    double last_total_amount_;
    double day_high_;
    double day_low_;
    double cumulative_amount_;
    
    // 预分组数据类型
    using OrderKey = std::tuple<int, std::string, std::string>;
    using OrderPregroup = std::map<OrderKey, std::vector<OrderRecord>>;
    using TradeGroups = std::map<int, std::vector<TradeRecord>>;
    
    // 核心聚合函数
    AggregatedData aggregateOneSecond(int time_sec,
                                      const OrderPregroup& order_pregroup,
                                      const std::vector<TradeRecord>& sec_trade);
    
    // 各项统计计算函数
    void calcBasicStats(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    void calcPositionStats(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    void calcLevelRangeStats(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    void calcCancelLevelStats(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    void calcTypeStats(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    void getLobSnapshot(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    void calcPendingStats(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    // 集合竞价 pending: 对全深度挂单簿做虚拟 uncross, 用残余簿填 pending 统计。
    // finalize=true 时(竞价末槽)把残余写回 pending_orders_, 并把残余簿播种到
    // 连续段守恒簿 cont_bid_book_/cont_ask_book_, 供连续段从开盘残余簿继续。
    void fillAuctionPending(AggregatedData& data, bool finalize);

    // ---- 连续段守恒簿操作 (路径A) ----
    void contAddPassiveWt(int time_sec, const OrderPregroup& order_pregroup);  // wt(position<0)入簿
    void contReduceByTrade(int time_sec, const OrderPregroup& order_pregroup); // zb 按被动价位 FIFO 扣
    void contCancelOrders(int time_sec, const OrderPregroup& order_pregroup);  // cl 按价位定位扣
    void contUncross();                                                       // 撮掉交叉量(buy1>=ask1), 防 pending 偏大
    void contFillPending(AggregatedData& data);                               // 从守恒簿填 pending(全深度+Num)
    void rebuildPendingFromContBook();  // 进入收盘竞价时, 用连续守恒簿重建 pending_orders_
    static void contFifoReduce(std::map<int64_t, std::deque<RestingOrder>>& book,
                               int64_t intp, int64_t qty);  // 同价位按 FIFO 扣 qty
    // 按 sysid 精确扣: 用于 zb 扣减 pos=0 主动单在己方簿中的已成交量
    static void contSysidReduce(std::map<int64_t, std::deque<RestingOrder>>& book,
                                std::unordered_map<int64_t, int64_t>& sysid_price,
                                int64_t sysid, int64_t qty);
    void calcMarketSummary(AggregatedData& data, int time_sec, const OrderPregroup& order_pregroup);
    
    // 辅助函数
    bool isContinuousTrading(int seconds) const;
    bool isCallAuction(int seconds) const;
    int getInstrumentId() const;
    std::string secondsToDateTime(int seconds) const;
    double safeMean(const std::vector<double>& prices) const;
    double safeWeightedPrice(const std::vector<double>& prices, 
                            const std::vector<int64_t>& volumes) const;
};

// ============================================================
// ClickHouse管理器类
// ============================================================

struct ClickHouseConfig {
    std::string host = "192.168.30.109";
    int port = 9000;  // clickhouse-cpp使用TCP端口(9000)，不是HTTP端口(8123)
    std::string username = "default";
    std::string password = "mxzichan@";
    std::string database = "Tick_TL";
};

class ClickHouseManager {
public:
    ClickHouseManager(const ClickHouseConfig& config);
    ~ClickHouseManager();
    
    void insertData(const std::vector<AggregatedData>& data,
                   const std::string& table_name = "Tick_3sec_data");
    
private:
    std::unique_ptr<clickhouse::Client> client_;
    std::string database_;
    ClickHouseConfig config_;
    
    void cleanDataForClickHouse(AggregatedData& data);
    static double cleanDouble(double val);
};

// ============================================================
// LOB数据处理器主类
// ============================================================

class LobDataProcessor {
public:
    explicit LobDataProcessor(const ClickHouseConfig& config = ClickHouseConfig());
    
    // 处理单个H5文件
    std::vector<AggregatedData> processH5File(const std::string& h5_path,
                                              const std::string& level2_path,
                                              const std::time_t& trade_date);
    
    // 处理并保存
    // table_name: ClickHouse表名，默认"Tick_3sec_data"
    // time_start/time_end: 交易时间段过滤(秒数，如34200=09:30:00)，-1表示不过滤
    // save_to_parquet_shard: 写每股二进制分片，供 merge_3s_parquet.py 合并为单文件 parquet
    // parquet_staging_dir: 分片输出根目录（实际写到 {root}/{YYYYMMDD}/{symbol}.bin）
    void processAndSave(const std::string& h5_path,
                       const std::string& level2_path,
                       const std::time_t& trade_date,
                       bool save_to_ch = true,
                       bool save_to_csv = false,
                       const std::string& table_name = "Tick_3sec_data",
                       int time_start = -1,
                       int time_end = -1,
                       bool save_to_parquet_shard = false,
                       const std::string& parquet_staging_dir = "");
    
    // 处理指定日期的所有文件
    struct ProcessResult {
        std::string date;
        int total;
        int success;
        int failed;
        double elapsed_seconds;
    };
    
    ProcessResult processDate(const std::time_t& trade_date,
                             const std::string& h5_base_path = "/home/sharedriver1/public/Lob_local",
                             const std::string& level2_base_path = "/home/sharedriver1/public/Level2",
                             const std::string& csv_base_path = "/home/sharedriver1/public/Lob_local_3s",
                             bool save_to_ch = true,
                             bool save_to_csv = false,
                             int max_workers = 20);
    
    // 处理日期范围
    std::vector<ProcessResult> processDateRange(const std::time_t& start_date,
                                                const std::time_t& end_date,
                                                const std::string& h5_base_path = "/home/sharedriver1/public/Lob_local",
                                                const std::string& level2_base_path = "/home/sharedriver1/public/Level2",
                                                const std::string& csv_base_path = "/home/sharedriver1/public/Lob_local_3s",
                                                bool save_to_ch = true,
                                                bool save_to_csv = false,
                                                int max_workers = 20,
                                                bool skip_weekends = true);
    
private:
    ClickHouseConfig ch_config_;

    // 保存CSV文件
    void saveToCSV(const std::vector<AggregatedData>& data, const std::string& csv_path);

    // 保存二进制分片（供 merge_3s_parquet.py 合并）
    // 列顺序严格对齐 export_tick_3s.py 的 FIELDS 列表，方便 Python 端零映射读入
    void saveToBinaryShard(const std::vector<AggregatedData>& data, const std::string& bin_path);
};

// ============================================================
// 工具函数
// ============================================================

namespace Utils {
    // 日期解析
    std::time_t parseDate(const std::string& date_str);
    std::string formatDate(const std::time_t& t, const char* format = "%Y%m%d");
    
    // 判断是否为周末
    bool isWeekend(const std::time_t& t);
    
    // 获取目录下所有H5文件
    std::vector<std::string> getH5Files(const std::string& dir_path);
}

#endif // LOB_DATA_PROCESSOR_H
