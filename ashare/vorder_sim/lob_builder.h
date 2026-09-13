#pragma once

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <climits>
#include <fstream>
#include <algorithm>
#include <functional>

namespace dc {

// ── 常量 ──────────────────────────────────────────────────────────────────────

constexpr double CONTINUOUS_AM_START = 34200.0;  // 09:30:00
constexpr double CONTINUOUS_AM_END   = 41400.0;  // 11:30:00
constexpr double CONTINUOUS_PM_START = 46800.0;  // 13:00:00
constexpr double CLOSE_AUCTION_START = 53820.0;  // 14:57:00 (收盘集合竞价开始)
constexpr double CLOSE_AUCTION_END   = 54000.0;  // 15:00:00
constexpr int    LOB_LEVELS          = 10;

// ── 统一中间记录（由 combined parser 直接产出） ───────────────────────────────

struct UnifiedRecord {
    int64_t seqNo      = 0;    // SH: BizIndex/seqNo; Flow: appSeq
    std::string time;          // HH:MM:SS.mmm
    double  timeSeconds = 0;
    int     actionType  = 0;   // 0=委托 1=撤单 2=成交
    char    direction   = ' '; // 'B' or 'S'
    int64_t sysid       = 0;   // 委托/撤单订单号
    int64_t buyId       = 0;   // 成交买方订单号
    int64_t sellId      = 0;   // 成交卖方订单号
    double  price       = 0;
    int64_t intPrice    = 0;   // price × 100 (整数分)
    int64_t volume      = 0;
    double  turnover    = 0;
    char    priceType   = '2'; // '2'=限价 '1'=市价 'U'=本方最优
    int     tradeDirection = 0;// 1=买方主动 2=卖方主动
};

// ── 订单对象 ──────────────────────────────────────────────────────────────────

struct Order {
    int64_t sysid;
    std::string time;
    double  timeSeconds;
    double  price;
    int64_t volume;
    int64_t originalVolume;
    char    direction;
    int64_t intPrice;

    Order() : sysid(0), timeSeconds(0), price(0), volume(0),
              originalVolume(0), direction(' '), intPrice(0) {}
    Order(int64_t id, const std::string& t, double p, int64_t v, char dir, double sec)
        : sysid(id), time(t), timeSeconds(sec), price(p), volume(v),
          originalVolume(v), direction(dir) {
        intPrice = static_cast<int64_t>((p + 1e-6) * 100);
    }
    std::pair<int64_t,int64_t> getBidKey() const { return {-intPrice, sysid}; }
    std::pair<int64_t,int64_t> getAskKey() const { return { intPrice, sysid}; }
};

// ── 虚拟订单信息（SH 市价单） ─────────────────────────────────────────────────

struct DerivedOrderInfo {
    int64_t sysid = 0;
    char    direction = ' ';
    double  price = 0;
    int64_t intPrice = 0;
    int64_t totalVolume = 0;
    std::string timeStr;
    double  timeSeconds = 0;
};

// ── LOB 输出记录 ──────────────────────────────────────────────────────────────

struct LobRecord {
    std::string time;
    double  timeSeconds = 0;
    int64_t sysid = 0;
    std::string ordertype;  // "wt" / "zb" / "cl"
    char    direction = ' ';
    double  price = 0;
    int64_t volume = 0;
    int64_t tradeVolume = 0;
    int     bidPosition = 0;
    int     askPosition = 0;
    double  bidDistance = 0;
    double  askDistance = 0;
    double  bidRatio = 0;
    double  askRatio = 0;
    double  timeDiff = 0;
    int     type = -1;
    int64_t bidLevels = 0;
    int64_t askLevels = 0;
    double  bdp[LOB_LEVELS] = {};
    int64_t bdv[LOB_LEVELS] = {};
    double  akp[LOB_LEVELS] = {};
    int64_t akv[LOB_LEVELS] = {};
    int64_t bdn[LOB_LEVELS] = {};  // 买盘每档订单笔数 (bid_num*)
    int64_t akn[LOB_LEVELS] = {};  // 卖盘每档订单笔数 (ask_num*)
};

// ── Trade 输出记录 ────────────────────────────────────────────────────────────

struct TradeRecord {
    int64_t sysid = 0;
    int64_t wtVolume = 0;
    int64_t wtTime = 0;
    double  wtSec = 0;
    double  transSt = 0, transSs = 0;
    int     act = 0;
    double  transEt = 0, transEs = 0;
    double  clTime = 0, clSec = 0;
    int64_t clVlm = 0;
    int64_t transVlm = 0;
    double  transAmt = 0, vwap = 0;
    int64_t actVlm = 0;
    double  actAmt = 0;
    int64_t actNum = 0, actPnm = 0;
    int64_t pasVlm = 0;
    double  pasAmt = 0;
    int64_t pasNum = 0, pasPnm = 0;
    double  wtTs = 0, tsTe = 0, wtCl = 0;
};

// ── LobBuilder ────────────────────────────────────────────────────────────────

class LobBuilder {
public:
    LobBuilder(const std::string& symbol, const std::string& date, bool isSH);

    // 传入已排好序的 UnifiedRecord（由 combined parser 产出）
    void load(std::vector<UnifiedRecord> records);

    void build();

    void writeOrderTableCSV(const std::string& path) const;
    void writeTradeTableCSV(const std::string& path) const;
    void writeOrderTableH5(const std::string& path) const;
    void writeTradeTableH5(const std::string& path) const;

    const std::vector<LobRecord>&  getOrderTable() const { return orderTable_; }
    const std::vector<TradeRecord>& getTradeTable() const { return tradeTable_; }

    // ── vorder_sim 扩展 ─────────────────────────────────────────────────────────
    // 每条记录处理完(handle* 改簿后)触发的观察者回调, 供虚拟订单追踪器旁挂。
    // 回调参数是「已 resequence 并排序后」的当前记录, 触发顺序 = 真实簿处理顺序。
    // 真实簿本身完全不受影响(回调只读)。
    using EventHook = std::function<void(const UnifiedRecord&, const LobBuilder&)>;
    void setEventHook(EventHook hook) { onEvent_ = std::move(hook); }
    // lite 模式: 只维护真实簿 + 触发回调, 跳过 orderTable_/tradeTable_ 的构造
    // (getCurrentLob/calcPosition/派生记录/距离比例等只为输出记录服务的重活)。
    // vorder_sim 不用这两张表, 开 lite 可显著减少建簿开销。
    void setLiteMode(bool v) { lite_ = v; }
    // 聚合盘口(价->量)只读访问, 供 uncross 与最优价查询。
    const std::map<int64_t,int64_t>& bestBid() const { return bestBidList_; }  // key = -intPrice
    const std::map<int64_t,int64_t>& bestAsk() const { return bestAskList_; }  // key =  intPrice

private:
    void buildDerivedOrders();
    bool createDerivedOrder(int64_t sysid, const std::string& time, LobRecord& rec);
    // SH 立即成交主动单的委托 seqNo 可能晚于其首笔成交 seqNo（交易所先撮合后回报）。
    // 把委托的排序键提前到其首笔成交之前，使 wt 必在 zb 前，盘口按正确顺序扣减。
    void resequenceSHOrderBeforeTrade();

    // priceType: '2'=限价(默认) '1'=市价/对手方最优(方案B：只进 allOrders_，不挂盘口)
    void handleOrder(int64_t sysid, const std::string& time, double price,
                     int64_t volume, char dir, double sec, char priceType = '2');
    bool handleCancel(int64_t sysid, const std::string& time, int64_t volume,
                      double& outPrice, int64_t& outVol, char& outDir);
    void handleTrade(int64_t buyId, int64_t sellId, double price, int64_t volume,
                     const std::string& time, int tradeDir,
                     char& activeDir, int64_t& activeSysid, int64_t& passiveSysid,
                     int64_t tradeSeqNo = 0);

    void checkWaitQueueReturn();
    void waitEnqueue(Order* o);   // 入等待队列(同步 unordered_map + 按价索引)
    void waitDequeue(Order* o);   // 出等待队列(成交完/撤单, 不回簿)
    void getCurrentLob(LobRecord& rec) const;
    std::pair<int,int> calcPosition(double price, char dir) const;
    int  calcType(double price, int64_t volume, char dir) const;
    bool isContinuousTradingTime(double sec) const;

    std::string symbol_, date_;
    bool isSH_;
    bool isContinuousPhase_ = false;

    std::vector<UnifiedRecord> records_;

    std::map<std::pair<int64_t,int64_t>, Order*> bidList_;
    std::map<std::pair<int64_t,int64_t>, Order*> askList_;
    std::unordered_map<int64_t, std::unique_ptr<Order>> allOrders_;

    std::map<int64_t, int64_t> bestBidList_;  // -intPrice → volume
    std::map<int64_t, int64_t> bestAskList_;  // +intPrice → volume

    std::unordered_map<int64_t, Order*> bidWaitQueue_;
    std::unordered_map<int64_t, Order*> askWaitQueue_;
    // 等待队列的按价索引(intPrice 升序): 让 checkWaitQueueReturn 只取"该回簿"的几笔(O(回簿)而非
    // O(队列)), 避免 SH active-skip 下队列变大导致 O(n²)。与上面两个 unordered_map 同步维护。
    std::map<int64_t, std::vector<Order*>> bidWaitByPrice_;
    std::map<int64_t, std::vector<Order*>> askWaitByPrice_;

    std::unordered_map<int64_t, DerivedOrderInfo> derivedOrders_;
    // SH resequence 提前的委托: sysid → A 记录原始 seqNo。上交所对"先吃后挂"单的 A_qty
    // 记的是挂住量(已净掉 A 之前的立即成交), 因此 seqNo < 原始A序号 的成交不得再扣该单,
    // 否则双重扣减(实测 688981@127.18 少 1612 股, 官方 tick=3056 vs 错误簿=1444)。
    std::unordered_map<int64_t, int64_t> shOrderOrigSeq_;
    // SH 每张委托的「成交量+撤单量」之和，用于补全立即成交主动单的 wt 量（A_qty 可能偏小）
    // 注: 不含 A 之前的成交(那部分已被交易所从 A_qty 净掉)。
    std::unordered_map<int64_t, int64_t> orderFilled_;
    // SH 价补全: 激进单 price 记的是入场价(首笔成交价)非限价 -> 买单=最高成交价/卖单=最低成交价还原限价
    std::unordered_map<int64_t, double> orderBuyMaxPx_;
    std::unordered_map<int64_t, double> orderSellMinPx_;

    int64_t cumulativeVolume_ = 0;

    std::vector<LobRecord>   orderTable_;
    std::vector<TradeRecord> tradeTable_;

    EventHook onEvent_;   // vorder_sim: 每条记录处理后触发(可空)
    bool lite_ = false;   // vorder_sim: 跳过 order/trade 记录构造
};

} // namespace dc
