/**
 * 通联历史数据 LOB Builder - 头文件
 * 
 * 直接处理通联格式的逐笔数据，生成LOB
 * 
 * 数据格式:
 * =========
 * 上海 mdl_4_24_0.csv: 逐笔全息 (委托+撤单+成交)
 *   字段: BizIndex,Channel,SecurityID,TickTime,Type,BuyOrderNO,SellOrderNO,Price,Qty,TradeMoney,TickBSFlag,LocalTime,SeqNo
 *   Type: A=委托, D=撤单, T=成交, S=状态
 *   TickBSFlag: B=买, S=卖
 * 
 * 深圳 mdl_6_33_0.csv: 逐笔委托
 *   字段: ChannelNo,ApplSeqNum,MDStreamID,SecurityID,SecurityIDSource,Price,OrderQty,Side,TransactTime,OrdType,LocalTime,SeqNo
 *   Side: 49='1'=买, 50='2'=卖
 * 
 * 深圳 mdl_6_36_0.csv: 逐笔成交
 *   字段: ChannelNo,ApplSeqNum,MDStreamID,BidApplSeqNum,OfferApplSeqNum,SecurityID,SecurityIDSource,LastPx,LastQty,ExecType,TransactTime,LocalTime,SeqNo
 *   ExecType: 52='4'=撤单, 70='F'=成交
 */

#pragma once

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <climits>
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <charconv>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace TLLob {

// ============================================================
// 常量定义
// ============================================================

// 上海 Type 定义
constexpr char SH_TYPE_ORDER = 'A';   // 委托
constexpr char SH_TYPE_CANCEL = 'D';  // 撤单
constexpr char SH_TYPE_TRADE = 'T';   // 成交
constexpr char SH_TYPE_STATUS = 'S';  // 状态

// 深圳 Side 定义 (ASCII)
constexpr int SZ_SIDE_BUY = 49;   // '1' = 买
constexpr int SZ_SIDE_SELL = 50;  // '2' = 卖

// 深圳 ExecType 定义 (ASCII)
constexpr int SZ_EXEC_CANCEL = 52;  // '4' = 撤单
constexpr int SZ_EXEC_TRADE = 70;   // 'F' = 成交

// 时间常量 (秒)
constexpr double OPEN_AUCTION_START = 33300.0;   // 09:15:00
constexpr double OPEN_AUCTION_END = 33900.0;     // 09:25:00
constexpr double CONTINUOUS_AM_START = 34200.0;  // 09:30:00
constexpr double CONTINUOUS_AM_END = 41400.0;    // 11:30:00
constexpr double CONTINUOUS_PM_START = 46800.0;  // 13:00:00
constexpr double CLOSE_AUCTION_START = 53820.0;  // 14:57:00
constexpr double CLOSE_AUCTION_END = 54000.0;    // 15:00:00

constexpr int LOB_LEVELS = 10;

// 大单阈值: 单笔委托金额 (price*volume, 单位: 元) >= 该值计入 bigBidVol/bigAskVol
// 可按研究口径调整 (常见: 50万=大单, 100万=特大单)
constexpr double BIG_ORDER_AMOUNT = 500000.0;

enum class Market { SH, SZ };

// ============================================================
// 数据结构
// ============================================================

/**
 * 上海逐笔记录 (mdl_4_24)
 */
struct SHTickRecord {
    int64_t bizIndex;
    int channel;
    std::string securityID;
    std::string tickTime;
    char type;           // A/D/T/S
    int64_t buyOrderNO;
    int64_t sellOrderNO;
    double price;
    int64_t qty;
    double tradeMoney;
    char tickBSFlag;     // B/S
    std::string localTime;
    int64_t seqNo;
    
    // 预处理字段
    double timeSeconds;
    int64_t intPrice;
};

/**
 * 深圳委托记录 (mdl_6_33)
 */
struct SZOrderRecord {
    int channelNo;
    int64_t applSeqNum;
    int mdStreamID;
    std::string securityID;
    double price;
    int64_t orderQty;
    int side;            // 49=买, 50=卖
    std::string transactTime;
    int ordType;
    std::string localTime;
    int64_t seqNo;
    
    // 预处理字段
    double timeSeconds;
    int64_t intPrice;
    char direction;      // 'B' or 'S'
};

/**
 * 深圳成交记录 (mdl_6_36)
 */
struct SZTradeRecord {
    int channelNo;
    int64_t applSeqNum;
    int mdStreamID;
    int64_t bidApplSeqNum;
    int64_t offerApplSeqNum;
    std::string securityID;
    double lastPx;
    int64_t lastQty;
    int execType;        // 52=撤单, 70=成交
    std::string transactTime;
    std::string localTime;
    int64_t seqNo;
    
    // 预处理字段
    double timeSeconds;
    int64_t intPrice;
};

/**
 * 统一的逐笔记录 (内部处理用)
 */
struct UnifiedRecord {
    int64_t seqNo;           // 排序用序号
    std::string time;
    double timeSeconds;
    int actionType;          // 0=委托, 1=撤单, 2=成交
    char direction;          // 'B' or 'S'
    int64_t sysid;           // 订单号 (委托/撤单用)
    int64_t buyId;           // 成交买方订单号
    int64_t sellId;          // 成交卖方订单号
    double price;
    int64_t intPrice;
    int64_t volume;
    double turnover;
    char priceType;          // '2'=限价
    int tradeDirection;      // 成交主动方向: 1=买方主动, 2=卖方主动, 0=未知
    
    UnifiedRecord() : seqNo(0), timeSeconds(0), actionType(0), direction(' '),
                      sysid(0), buyId(0), sellId(0), price(0), intPrice(0),
                      volume(0), turnover(0), priceType('2'), tradeDirection(0) {}
};

/**
 * 订单对象
 */
struct Order {
    int64_t sysid;
    std::string time;
    double timeSeconds;
    double price;
    int64_t volume;
    int64_t originalVolume;
    char direction;
    int64_t intPrice;
    
    Order() : sysid(0), timeSeconds(0), price(0), volume(0), 
              originalVolume(0), direction(' '), intPrice(0) {}
    
    Order(int64_t id, const std::string& t, double p, int64_t v, char dir, double timeSec = 0.0)
        : sysid(id), time(t), timeSeconds(timeSec), price(p), volume(v), 
          originalVolume(v), direction(dir) {
        intPrice = static_cast<int64_t>((p + 1e-6) * 100);
    }
    
    std::pair<int64_t, int64_t> getBidKey() const {
        return {-intPrice, sysid};
    }
    
    std::pair<int64_t, int64_t> getAskKey() const {
        return {intPrice, sysid};
    }
};

/**
 * LOB输出记录
 */
struct LobRecord {
    std::string time;
    int64_t sysid;
    std::string ordertype;  // wt/zb/cl
    char direction;
    double price;
    int64_t volume;
    int64_t tradeVolume;
    int bidPosition;
    int askPosition;
    double bidDistance;
    double askDistance;
    double bidRatio;
    double askRatio;
    double timeDiff;
    int type;
    int64_t bidLevels;
    int64_t askLevels;
    
    double bdp[LOB_LEVELS];
    int64_t bdv[LOB_LEVELS];
    double akp[LOB_LEVELS];
    int64_t akv[LOB_LEVELS];
    
    LobRecord() : sysid(0), direction(' '), price(0), volume(0),
                  tradeVolume(0), bidPosition(0), askPosition(0),
                  bidDistance(0), askDistance(0), bidRatio(0), askRatio(0),
                  timeDiff(0), type(-1), bidLevels(0), askLevels(0) {
        std::fill(bdp, bdp + LOB_LEVELS, 0.0);
        std::fill(bdv, bdv + LOB_LEVELS, 0);
        std::fill(akp, akp + LOB_LEVELS, 0.0);
        std::fill(akv, akv + LOB_LEVELS, 0);
    }
};

/**
 * Trade输出记录
 */
struct TradeRecord {
    int64_t sysid = 0;
    int64_t wtVolume = 0;
    int64_t wtTime = 0;
    double wtSec = 0;
    double transSt = 0;
    double transSs = 0;
    int act = 0;
    double transEt = 0;
    double transEs = 0;
    double clTime = 0;
    double clSec = 0;
    int64_t clVlm = 0;
    int64_t transVlm = 0;
    double transAmt = 0;
    double vwap = 0;
    int64_t actVlm = 0;
    double actAmt = 0;
    int64_t actNum = 0;
    int64_t actPnm = 0;
    int64_t pasVlm = 0;
    double pasAmt = 0;
    int64_t pasNum = 0;
    int64_t pasPnm = 0;
    double wtTs = 0;
    double tsTe = 0;
    double wtCl = 0;
};

/**
 * 虚拟订单信息 (用于上海市场成交引用但无委托记录的订单)
 */
struct DerivedOrderInfo {
    int64_t sysid;
    char direction;        // 'B' or 'S'
    double price;
    int64_t totalVolume;   // 累计成交量（推断的委托量）
    std::string timeStr;   // 首次出现时间
    double timeSeconds;
    int64_t intPrice;
    
    DerivedOrderInfo() : sysid(0), direction(' '), price(0), totalVolume(0),
                         timeSeconds(0), intPrice(0) {}
};

/**
 * 集合竞价 Action 输出记录 (开盘/收盘集合竞价逐事件快照)
 *
 * 字段顺序与 action_data_save 样例完全一致:
 *   time, close, totalVolume, totalAmt,
 *   bdp1..bdp10/bdv1..bdv10, akp1..akp10/akv1..akv10, totalNum
 *
 * 其中 close/totalVolume/totalAmt/totalNum 为对全深度订单簿做"虚拟集合竞价
 * 撮合(uncross)"的结果; bdp/bdv/akp/akv 为撮合后剩余盘口的前 10 档。
 */
struct ActionRecord {
    std::string time;          // HH:MM:SS.mmm (调试用, 不写入 bin)
    int64_t dateTime;          // 整秒 17 位: YYYYMMDDHHMMSS000 (float64 精确)
    int64_t eventSeq;          // 当日该股票逐笔处理顺序，用于同秒多事件稳定排序
    double close;              // 虚拟集合竞价参考价 (撮合均衡价); 无可撮合时为 NaN
    int64_t totalVolume;      // 虚拟匹配量
    double totalAmt;           // 虚拟匹配额 = close * totalVolume
    int64_t totalNum;         // 虚拟撮合成交笔数 (订单级 FIFO 计数)

    // 撮合后剩余盘口前 10 档
    double bdp[LOB_LEVELS];
    int64_t bdv[LOB_LEVELS];
    double akp[LOB_LEVELS];
    int64_t akv[LOB_LEVELS];

    // ===== 扩充因子列 =====
    // Tier 1: auction 失衡 + 全深度聚合 (撮合价处 / 全簿; 仅 C++ 全簿可得)
    double imbalanceVol;       // 撮合价处 demand - supply (带符号; +买方剩余); 无撮合 NaN
    double imbalanceRatio;     // (demand-supply)/(demand+supply); 无撮合 NaN
    int64_t totalBidVol;       // 全深度买盘总挂单量 (撮合前真实挂单)
    int64_t totalAskVol;       // 全深度卖盘总挂单量
    int64_t totalBidNum;       // 全深度买盘挂单笔数
    int64_t totalAskNum;       // 全深度卖盘挂单笔数
    // Tier 2: 事件流 / 订单级
    int64_t cancelVolCum;      // 当前集合竞价段内累计撤单量 (开盘/尾盘分别累计)
    int64_t cancelNumCum;      // 当前集合竞价段内累计撤单笔数
    double bidVwapFull;        // 全深度买盘量加权价 Σ(p*v)/Σv; 空簿 NaN
    double askVwapFull;        // 全深度卖盘量加权价; 空簿 NaN
    int64_t bigBidVol;         // 大单(委托额 >= BIG_ORDER_AMOUNT)买盘总量
    int64_t bigAskVol;         // 大单卖盘总量

    ActionRecord() : dateTime(0), eventSeq(0), close(std::nan("")), totalVolume(0),
                     totalAmt(std::nan("")), totalNum(0),
                     imbalanceVol(std::nan("")), imbalanceRatio(std::nan("")),
                     totalBidVol(0), totalAskVol(0), totalBidNum(0), totalAskNum(0),
                     cancelVolCum(0), cancelNumCum(0),
                     bidVwapFull(std::nan("")), askVwapFull(std::nan("")),
                     bigBidVol(0), bigAskVol(0) {
        std::fill(bdp, bdp + LOB_LEVELS, 0.0);
        std::fill(bdv, bdv + LOB_LEVELS, 0);
        std::fill(akp, akp + LOB_LEVELS, 0.0);
        std::fill(akv, akv + LOB_LEVELS, 0);
    }
};

// ============================================================
// LOB Builder 类
// ============================================================

class TLLobBuilder {
public:
    TLLobBuilder(const std::string& symbol, const std::string& tradeDate, Market market);
    
    // 上海数据处理
    void loadSHData(const std::string& filePath);
    void loadSHDataFromMemory(const std::vector<SHTickRecord>& records);
    
    // 深圳数据处理
    void loadSZOrderData(const std::string& filePath);
    void loadSZOrderDataFromMemory(const std::vector<SZOrderRecord>& records);
    void loadSZTradeData(const std::string& filePath);
    void loadSZTradeDataFromMemory(const std::vector<SZTradeRecord>& records);
    
    // 构建LOB
    void build();
    
    // 输出
    void writeOrderTableCSV(const std::string& filename) const;
    void writeTradeTableCSV(const std::string& filename) const;
    bool writeH5(const std::string& filename) const;

    // 集合竞价 action 输出 (本项目新增): 紧凑列主序二进制 shard, 供 Python merge
    void writeActionBin(const std::string& filename) const;

    // 获取结果
    const std::vector<LobRecord>& getOrderTable() const { return orderTable_; }
    const std::vector<TradeRecord>& getTradeTable() const { return tradeTable_; }
    const std::vector<ActionRecord>& getActionTable() const { return actionTable_; }
    
    void setVerbose(bool v) { verbose_ = v; }
    
    // 数据解析 (public for standalone CSV readers)
    static double parseTimeToSeconds(const std::string& timeStr);
    static std::string formatTimeStr(const std::string& rawTime);

private:
    
    // 订单簿操作
    // priceType: '2'=限价(默认) '1'=市价。市价单在对手盘为空、解析不出真实价时只登记
    // allOrders_、不挂簿(否则占位价 0.0 会被当真挂进 bestBidList_[0] 形成幽灵档),
    // 与 datacheck_final 一致。
    void handleOrder(int64_t sysid, const std::string& timeStr, double price,
                     int64_t volume, char direction, double timeSeconds, char priceType = '2');
    bool handleCancel(int64_t sysid, const std::string& timeStr, int64_t volume,
                      double& outPrice, int64_t& outVolume, char& outDirection);
    void handleTrade(int64_t buyId, int64_t sellId, double price, int64_t volume,
                     const std::string& timeStr, int tradeDirection,
                     char& activeDir, int64_t& activeSysid, int64_t& passiveSysid,
                     int64_t tradeSeqNo = 0);

    // SH 立即成交主动单的委托(A)BizIndex 可能晚于其成交(T)回报。
    // 把这类委托排序键提前到首笔成交 BizIndex，同 seqNo 下委托先于成交处理。
    void resequenceSHOrderBeforeTrade();
    
    // 虚拟订单 (上海市场)
    void buildDerivedOrders();
    bool createDerivedOrder(int64_t sysid, const std::string& timeStr, LobRecord& record);
    
    // Wait Queue
    void checkWaitQueueReturn();
    void waitEnqueue(Order* o);   // 入等待队列(同步 unordered_map + 按价索引)
    void waitDequeue(Order* o);   // 出等待队列(成交完/撤单清零, 不回簿)
    
    // 盘口计算
    void getCurrentLob(LobRecord& record) const;
    std::pair<int, int> calcPosition(double price, char direction) const;
    int calcType(double price, int64_t volume, char direction) const;

    // 集合竞价虚拟撮合: 对当前全深度订单簿做一次 uncross, 产出
    // close/totalVolume/totalAmt/totalNum + 撮合后剩余前 10 档, 不改动真实簿
    void computeAuctionSnapshot(const std::string& timeStr, ActionRecord& record) const;
    
    // 时间判断
    bool isContinuousTradingTime(double timeSeconds) const;
    bool isAuctionTradingTime(double timeSeconds) const;
    
private:
    std::string symbol_;
    std::string tradeDate_;
    Market market_;
    bool verbose_ = true;
    
    // 统一记录
    std::vector<UnifiedRecord> records_;
    
    // 订单簿
    std::map<std::pair<int64_t, int64_t>, Order*> bidList_;
    std::map<std::pair<int64_t, int64_t>, Order*> askList_;
    std::unordered_map<int64_t, std::unique_ptr<Order>> allOrders_;
    
    // Wait Queue
    std::unordered_map<int64_t, Order*> bidWaitQueue_;
    std::unordered_map<int64_t, Order*> askWaitQueue_;
    // 等待队列的按价索引(intPrice 升序): 让 checkWaitQueueReturn 只遍历「该回簿」的几个
    // 价位(O(回簿) 而非 O(队列)), 避免激进单堆积时每事件全量扫描退化为 O(n²)。
    // 与上面两个 unordered_map 同步维护(移植自 datacheck_final)。
    std::map<int64_t, std::vector<Order*>> bidWaitByPrice_;
    std::map<int64_t, std::vector<Order*>> askWaitByPrice_;
    
    // 虚拟订单 (上海市场：成交引用但无委托记录的订单)
    std::unordered_map<int64_t, DerivedOrderInfo> derivedOrders_;

    // SH resequence 提前的委托: sysid -> A 记录原始 seqNo。上交所对"先吃后挂"单的 A_qty
    // 记的是挂住量(已净掉 A 之前的立即成交)，因此 seqNo < 原始A序号 的成交不得再扣该单，
    // 否则双重扣减(移植自 datacheck_final Fix5)。
    std::unordered_map<int64_t, int64_t> shOrderOrigSeq_;
    // SH 每张委托的「成交量+撤单量」之和，用于补全立即成交主动单的展示委托量（A_qty 可能偏小）。
    // 不含 A 之前的成交(那部分已被交易所从 A_qty 净掉)，移植自 datacheck_final。
    std::unordered_map<int64_t, int64_t> orderFilled_;
    // SH 派生单(无 A 委托记录)价补全: info.price 是首笔成交价(入场价)非限价 ->
    // 买单还原最高成交价 / 卖单还原最低成交价(= 该单真实限价), 否则残量挂错档位、
    // type 分类被带翻(移植自 datacheck_final)。
    std::unordered_map<int64_t, double> orderBuyMaxPx_;
    std::unordered_map<int64_t, double> orderSellMinPx_;
    
    // Best price list
    std::map<int64_t, int64_t> bestBidList_;  // -intPrice -> volume
    std::map<int64_t, int64_t> bestAskList_;  // intPrice -> volume
    
    // 累计成交量
    int64_t cumulativeVolume_ = 0;

    // 集合竞价时段累计撤单 (供 cancelVolCum/cancelNumCum)
    int64_t auctionCancelVol_ = 0;
    int64_t auctionCancelNum_ = 0;
    
    // 输出
    std::vector<LobRecord> orderTable_;
    std::vector<TradeRecord> tradeTable_;
    std::vector<ActionRecord> actionTable_;  // 集合竞价 action 输出
};

// ============================================================
// 数据读取函数
// ============================================================

// nWorkers: 按字节区间把文件切成 N 块, 各块用独立线程(std::async)并行扫描+解析+过滤,
// 大幅加速大文件读取(参考 datacheck_final 的 computeFileChunks/parseTLChunk 思路)。
// nWorkers<=1 时退化为单线程(等价于旧行为)。
std::vector<SHTickRecord> readSHTickCSV(const std::string& filename, const std::string& symbol = "", int nWorkers = 1);
std::vector<SZOrderRecord> readSZOrderCSV(const std::string& filename, const std::string& symbol = "", int nWorkers = 1);
std::vector<SZTradeRecord> readSZTradeCSV(const std::string& filename, const std::string& symbol = "", int nWorkers = 1);

} // namespace TLLob
