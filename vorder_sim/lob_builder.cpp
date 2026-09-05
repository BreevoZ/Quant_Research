#include "lob_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <H5Cpp.h>
#include <iomanip>
#include <iostream>
#include <unordered_set>

namespace dc {

LobBuilder::LobBuilder(const std::string& symbol, const std::string& date, bool isSH)
    : symbol_(symbol), date_(date), isSH_(isSH) {}

void LobBuilder::load(std::vector<UnifiedRecord> records) {
    records_ = std::move(records);
}

// ─────────────────────────────────────────────────────────────────────────────

bool LobBuilder::isContinuousTradingTime(double sec) const {
    if (sec >= CONTINUOUS_AM_START && sec < CONTINUOUS_AM_END) return true;
    // 下午连续竞价沪深一致 13:00-14:57；14:57-15:00 是收盘集合竞价（非连续，
    // 交叉单直接累积进簿、得到竞价簿分类，收盘簿由 15:00 真实成交收口）。
    if (sec >= CONTINUOUS_PM_START && sec < CLOSE_AUCTION_START) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────

void LobBuilder::handleOrder(int64_t sysid, const std::string& time, double price,
                              int64_t volume, char dir, double sec, char priceType) {
    if (volume <= 0 || sysid <= 0) return;

    auto order = std::make_unique<Order>(sysid, time, price, volume, dir, sec);
    Order* ptr = order.get();
    int64_t ip = ptr->intPrice;
    bool continuous = isContinuousTradingTime(sec);

    // 市价单（priceType=='1'）：上游已将其展示价解析为「到达时对手盘最优价」(真实可用价格,
    // 非涨跌停保护价，见 build() 里 SZ 市价单价格解析)，只有对手盘为空、price 仍是原始占位值
    // (无法解析出真实价格) 时才不挂盘口，只记录用于成交扣减。
    // 否则必须按普通激进单走「先吃后挂」(waitEnqueue): 市价单常见部分成交后残量继续挂在
    // 队列里、被后续到达的对手单陆续吃掉(实测 300750 当天 12 笔市价单成交横跨 0.5~10.4 秒)。
    // 若像旧逻辑一样完全跳过 best*List_，残量的后续成交仍会走 updateOrder 从 best*List_ 里
    // 倒扣一笔从未记入的量，导致该价位被错误掏空(20260611 sz300750@383.19 实测偏差 -100)。
    if (priceType == '1') {
        bool hasOpposite = (dir == 'B') ? !bestAskList_.empty() : !bestBidList_.empty();
        if (!hasOpposite) {
            allOrders_[sysid] = std::move(order);
            return;
        }
    }

    if (dir == 'B') {
        if (continuous && !bestAskList_.empty()) {     
            if (ip >= bestAskList_.begin()->first) {
                waitEnqueue(ptr);
                allOrders_[sysid] = std::move(order);
                return;
            }
        }
        bidList_[ptr->getBidKey()] = ptr;
        bestBidList_[-ip] += volume;
    } else {
        if (continuous && !bestBidList_.empty()) {
            int64_t bestBidIP = -bestBidList_.begin()->first;
            if (ip <= bestBidIP) {
                waitEnqueue(ptr);
                allOrders_[sysid] = std::move(order);
                return;
            }
        }
        askList_[ptr->getAskKey()] = ptr;
        bestAskList_[ip] += volume;
    }

    allOrders_[sysid] = std::move(order);
    checkWaitQueueReturn();
}

bool LobBuilder::handleCancel(int64_t sysid, const std::string& /*time*/, int64_t volume,
                               double& outPrice, int64_t& outVol, char& outDir) {
    auto it = allOrders_.find(sysid);
    if (it == allOrders_.end()) return false;

    Order* o = it->second.get();
    if (!o || o->volume <= 0) { outVol = 0; return false; }

    outPrice = o->price;
    outDir   = o->direction;
    int64_t cancel = (volume > 0) ? std::min(volume, o->volume) : o->volume;
    outVol   = cancel;
    o->volume -= cancel;

    if (o->direction == 'B') {
        if (bidWaitQueue_.count(sysid)) {
            if (o->volume <= 0) waitDequeue(o);
        } else {
            if (o->volume <= 0) bidList_.erase(o->getBidKey());
            bestBidList_[-o->intPrice] -= cancel;
            if (bestBidList_[-o->intPrice] <= 0) bestBidList_.erase(-o->intPrice);
        }
    } else {
        if (askWaitQueue_.count(sysid)) {
            if (o->volume <= 0) waitDequeue(o);
        } else {
            if (o->volume <= 0) askList_.erase(o->getAskKey());
            bestAskList_[o->intPrice] -= cancel;
            if (bestAskList_[o->intPrice] <= 0) bestAskList_.erase(o->intPrice);
        }
    }

    checkWaitQueueReturn();
    return true;
}

void LobBuilder::handleTrade(int64_t buyId, int64_t sellId, double /*price*/, int64_t volume,
                              const std::string& /*time*/, int tradeDir,
                              char& activeDir, int64_t& activeSysid, int64_t& passiveSysid,
                              int64_t tradeSeqNo) {
    cumulativeVolume_ += volume;

    Order* buyOrder  = nullptr;
    Order* sellOrder = nullptr;
    auto ib = allOrders_.find(buyId);  if (ib != allOrders_.end()) buyOrder  = ib->second.get();
    auto is = allOrders_.find(sellId); if (is != allOrders_.end()) sellOrder = is->second.get();

    if (tradeDir == 1 || tradeDir == 2) {
        activeDir    = (tradeDir == 1) ? 'B' : 'S';
        activeSysid  = (tradeDir == 1) ? buyId  : sellId;
        passiveSysid = (tradeDir == 1) ? sellId : buyId;
    } else {
        activeDir    = (buyId > sellId) ? 'B' : 'S';
        activeSysid  = std::max(buyId, sellId);
        passiveSysid = std::min(buyId, sellId);
    }

    auto updateOrder = [this](Order* o, int64_t vol) {
        if (!o) return;
        // 跟 handleCancel 一样夹到剩余量为止——不能在 vol 超出剩余量时整段跳过扣减，
        // 否则 bestBidList_/bestAskList_ 里的残留量永远清不掉，盘口价位会被卡死在
        // 一个早就不存在的价格上（实测：涨停价撤回后盘口长期卡在涨停价的根因）。
        int64_t applied = std::min(vol, o->volume);
        if (applied <= 0) return;
        o->volume -= applied;
        if (o->direction == 'B') {
            if (bidWaitQueue_.count(o->sysid)) {
                if (o->volume <= 0) waitDequeue(o);
            } else {
                if (o->volume <= 0) bidList_.erase(o->getBidKey());
                bestBidList_[-o->intPrice] -= applied;
                if (bestBidList_[-o->intPrice] <= 0) bestBidList_.erase(-o->intPrice);
            }
        } else {
            if (askWaitQueue_.count(o->sysid)) {
                if (o->volume <= 0) waitDequeue(o);
            } else {
                if (o->volume <= 0) askList_.erase(o->getAskKey());
                bestAskList_[o->intPrice] -= applied;
                if (bestAskList_[o->intPrice] <= 0) bestAskList_.erase(o->intPrice);
            }
        }
    };

    // "先吃后挂"单(A 被 resequence 提前): A_qty 已净掉原始 A 序号之前的成交, 该部分不再扣减,
    // 只扣对手方; 否则挂住残量被双扣(先吃部分扣两次)。
    auto isPreAFill = [this](int64_t sysid, int64_t seq) {
        auto it = shOrderOrigSeq_.find(sysid);
        return it != shOrderOrigSeq_.end() && seq > 0 && seq < it->second;
    };
    if (!isPreAFill(buyId,  tradeSeqNo)) updateOrder(buyOrder,  volume);
    if (!isPreAFill(sellId, tradeSeqNo)) updateOrder(sellOrder, volume);

    // 残量显示由 checkWaitQueueReturn(严格 价<卖一) 负责(卖盘清到上方才回簿, bid1<ask1, 对齐官方 tick);
    // 不在触价(价==对手一档)surface, 避免 bdp1==akp1 锁价(多笔同毫秒成交间的瞬时残量, 官方不显示)。
    checkWaitQueueReturn();
}

void LobBuilder::waitEnqueue(Order* o) {
    if (o->direction == 'B') {
        bidWaitQueue_[o->sysid] = o;
        bidWaitByPrice_[o->intPrice].push_back(o);
    } else {
        askWaitQueue_[o->sysid] = o;
        askWaitByPrice_[o->intPrice].push_back(o);
    }
}

void LobBuilder::waitDequeue(Order* o) {
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

void LobBuilder::checkWaitQueueReturn() {
    if (!isContinuousPhase_) return;

    // 出队严格 < 卖一(与入队 >= 卖一 互补): 锁价(==)单留在队列。用按价索引只遍历"该回簿"的价位:
    // 买单 intPrice < 卖一 -> 索引中 [begin, lower_bound(卖一)); 无卖盘则全回(卖一=+∞)。O(回簿)。
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

// ─────────────────────────────────────────────────────────────────────────────

void LobBuilder::resequenceSHOrderBeforeTrade() {
    // 1. 找每个订单号（买/卖）首笔成交的最小 seqNo
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
    // 2. 若委托的 seqNo 晚于其首笔成交，把排序键提前到首笔成交的 seqNo。
    //    同时记录原始序号: 这类"先吃后挂"单的 A_qty 是挂住量(交易所已净掉 A 前的立即成交),
    //    handleTrade 对 seq < 原始A序号 的成交必须跳过主动方扣减, 否则双重扣减。
    for (auto& r : records_) {
        if (r.actionType != 0) continue;
        auto it = firstTradeBiz.find(r.sysid);
        if (it != firstTradeBiz.end() && it->second < r.seqNo) {
            shOrderOrigSeq_[r.sysid] = r.seqNo;
            r.seqNo = it->second;
        }
    }
}

void LobBuilder::buildDerivedOrders() {
    std::unordered_set<int64_t> existing;
    existing.reserve(records_.size() / 2);
    for (const auto& r : records_)
        if (r.actionType == 0) existing.insert(r.sysid);

    for (const auto& r : records_) {
        if (r.actionType != 2) continue;
        auto process = [&](int64_t id, char dir) {
            if (id <= 0 || existing.count(id)) return;
            auto it = derivedOrders_.find(id);
            if (it == derivedOrders_.end()) {
                DerivedOrderInfo info;
                info.sysid = id; info.direction = dir;
                info.price = r.price; info.intPrice = r.intPrice;
                info.totalVolume = r.volume;
                info.timeStr = r.time; info.timeSeconds = r.timeSeconds;
                derivedOrders_[id] = info;
            } else {
                it->second.totalVolume += r.volume;
                if (r.timeSeconds < it->second.timeSeconds) {
                    it->second.timeStr = r.time;
                    it->second.timeSeconds = r.timeSeconds;
                }
            }
        };
        process(r.buyId,  'B');
        process(r.sellId, 'S');
    }

    // 预计算每张委托的「成交量+撤单量」之和，供 build() 补全 SH 立即成交主动单的 wt 量。
    // SH 交易所对立即成交的主动单只记录已挂住部分的委托量（A_qty），可能小于实际下单量；
    // 补全为 max(A_qty, orderFilled_) 让 wt 量守恒，handleOrder 仍用原始 A_qty 不影响订单簿。
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
            if (r.sysid  > 0) orderFilled_[r.sysid]  += r.volume;
        }
    }
}

bool LobBuilder::createDerivedOrder(int64_t sysid, const std::string& /*time*/, LobRecord& rec) {
    auto it = derivedOrders_.find(sysid);
    if (it == derivedOrders_.end()) return false;
    if (allOrders_.count(sysid)) return false;

    const DerivedOrderInfo& info = it->second;

    // 价补全(仅派生单): 派生单无 A 委托记录, info.price 是首笔成交价非限价 ->
    // 买单还原最高成交价、卖单还原最低成交价(=限价); 否则残量挂错顶档。
    double dprice = info.price;
    if (info.direction == 'B') {
        auto pit = orderBuyMaxPx_.find(sysid);
        if (pit != orderBuyMaxPx_.end() && pit->second > dprice) dprice = pit->second;
    } else {
        auto pit = orderSellMinPx_.find(sysid);
        if (pit != orderSellMinPx_.end() && pit->second < dprice) dprice = pit->second;
    }

    auto [bidPos, askPos] = calcPosition(dprice, info.direction);
    int type = calcType(dprice, info.totalVolume, info.direction);

    rec.time        = info.timeStr;
    rec.timeSeconds = info.timeSeconds;
    rec.sysid       = sysid;
    rec.ordertype   = "wt";
    rec.direction   = info.direction;
    rec.price       = dprice;
    rec.volume      = info.totalVolume;
    rec.tradeVolume = cumulativeVolume_;
    rec.bidPosition = bidPos;
    rec.askPosition = askPos;
    rec.type        = type;
    rec.timeDiff    = 0;
    if (!lite_) {
        getCurrentLob(rec);
        if (rec.bdp[0] > 0) {
            rec.bidDistance = rec.bdp[0] - dprice;
            if (rec.bdv[0] > 0)
                rec.bidRatio = static_cast<double>(info.totalVolume) / rec.bdv[0];
        }
        if (rec.akp[0] > 0) {
            rec.askDistance = dprice - rec.akp[0];
            if (rec.akv[0] > 0)
                rec.askRatio = static_cast<double>(info.totalVolume) / rec.akv[0];
        }
    }

    handleOrder(sysid, info.timeStr, dprice, info.totalVolume, info.direction, info.timeSeconds);
    return true;
}

void LobBuilder::getCurrentLob(LobRecord& rec) const {
    int level = 0;
    for (auto& [key, vol] : bestBidList_) {
        if (level >= LOB_LEVELS) break;
        rec.bdp[level] = (-key) / 100.0;
        rec.bdv[level] = vol;
        auto lo = bidList_.lower_bound({key, INT64_MIN});
        auto hi = bidList_.upper_bound({key, INT64_MAX});
        rec.bdn[level] = static_cast<int64_t>(std::distance(lo, hi));
        ++level;
    }
    rec.bidLevels = static_cast<int64_t>(bestBidList_.size());

    level = 0;
    for (auto& [key, vol] : bestAskList_) {
        if (level >= LOB_LEVELS) break;
        rec.akp[level] = key / 100.0;
        rec.akv[level] = vol;
        auto lo = askList_.lower_bound({key, INT64_MIN});
        auto hi = askList_.upper_bound({key, INT64_MAX});
        rec.akn[level] = static_cast<int64_t>(std::distance(lo, hi));
        ++level;
    }
    rec.askLevels = static_cast<int64_t>(bestAskList_.size());
}

std::pair<int,int> LobBuilder::calcPosition(double price, char dir) const {
    int bidPos = 0, askPos = 0;
    int64_t ip = static_cast<int64_t>((price + 1e-6) * 100);

    if (dir == 'B') {
        if (!bestAskList_.empty() && ip >= bestAskList_.begin()->first) {
            askPos = 0;
        } else {
            auto lb = bestBidList_.lower_bound(-ip);
            bidPos = static_cast<int>(std::distance(bestBidList_.begin(), lb)) + 1;
        }
    } else {
        if (!bestBidList_.empty() && ip <= -bestBidList_.begin()->first) {
            bidPos = 0;
        } else {
            auto lb = bestAskList_.lower_bound(ip);
            askPos = static_cast<int>(std::distance(bestAskList_.begin(), lb)) + 1;
        }
    }
    return {bidPos, askPos};
}

int LobBuilder::calcType(double price, int64_t volume, char dir) const {
    bool hasBid = !bestBidList_.empty();
    bool hasAsk = !bestAskList_.empty();
    if (!hasBid && !hasAsk) return 0;

    int64_t ip       = static_cast<int64_t>((price + 1e-6) * 100);
    int64_t bdp1     = hasBid ? -bestBidList_.begin()->first : 0;
    int64_t bdv1     = hasBid ?  bestBidList_.begin()->second : 0;
    int64_t akp1     = hasAsk ?  bestAskList_.begin()->first  : INT64_MAX;
    int64_t akv1     = hasAsk ?  bestAskList_.begin()->second : 0;

    if (dir == 'B') {
        if (hasAsk) {
            if      (ip > akp1 && volume >= akv1) return 1;
            else if (ip == akp1 && volume >= akv1) return 2;
            else if (ip >= akp1 && volume < akv1)  return 3;
        }
        if (hasBid) {
            if (hasAsk && ip < akp1 && ip > bdp1) return 4;
            else if (ip == bdp1) return 5;
            else if (ip <  bdp1) return 6;
        } else if (!hasAsk) return 5;
    } else {
        if (hasBid) {
            if      (ip < bdp1 && volume >= bdv1) return 1;
            else if (ip == bdp1 && volume >= bdv1) return 2;
            else if (ip <= bdp1 && volume < bdv1)  return 3;
        }
        if (hasAsk) {
            if (hasBid && ip > bdp1 && ip < akp1) return 4;
            else if (ip == akp1) return 5;
            else if (ip >  akp1) return 6;
        } else if (!hasBid) return 5;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────

void LobBuilder::build() {
    if (records_.empty()) return;

    // SH：先把立即成交主动单的委托 seqNo 提前到其首笔成交之前，再排序，
    // 保证 wt 始终在对应 zb 之前出现，盘口能按正确顺序扣减。
    if (isSH_) resequenceSHOrderBeforeTrade();

    // 上海按 BizIndex(seqNo)、Flow 按 appSeq(seqNo) 统一排序。
    // 同 seqNo 时按 actionType 排序：委托(0) < 撤单(1) < 成交(2)。
    // resequenceSHOrderBeforeTrade 会把立即成交主动单的委托 seqNo 提前到其首笔成交，
    // 造成 A 与 T 同 seqNo——必须保证 A 在 T 前，否则成交先于委托被处理、盘口被污染。
    std::sort(records_.begin(), records_.end(),
              [](const UnifiedRecord& a, const UnifiedRecord& b){
                  if (a.seqNo != b.seqNo) return a.seqNo < b.seqNo;
                  return a.actionType < b.actionType;
              });

    if (isSH_) buildDerivedOrders();

    bool contStarted = false;
    double lastWtSec = 0.0;   // 上一条委托(wt)时间, 供 wt 的 time_diff(生命周期语义)

    for (const auto& rec : records_) {
        if (!contStarted && isContinuousTradingTime(rec.timeSeconds)) {
            contStarted = true;
            isContinuousPhase_ = true;
            checkWaitQueueReturn();
        }

        LobRecord lr;
        lr.time        = rec.time;
        lr.timeSeconds = rec.timeSeconds;
        // 盘口快照取「本笔事件发生前」的状态(与 tl/flow 一致, 下游 build_tick3s 按 pre-event
        // 基线消费、自行用下一行推 post-event): 在 handle* 改簿前先快照。
        // time_diff 改生命周期语义(wt=距上委托, cl=撤单−原委托, zb=成交−被动单), 各分支内算。
        if (!lite_) getCurrentLob(lr);

        if (rec.actionType == 0) {
            // SZ 特殊单价格按盘口解析(源 price 是涨停/地板占位价, 非真实价):
            //   'U' 本方最优 -> best-self(买→买一, 卖→卖一); '1' 市价 -> 对手盘最优(买→卖一, 卖→买一)。
            double wtPrice = rec.price;
            if (!isSH_ && rec.priceType == 'U') {
                if (rec.direction == 'B' && !bestBidList_.empty())      wtPrice = -bestBidList_.begin()->first / 100.0;
                else if (rec.direction == 'S' && !bestAskList_.empty()) wtPrice = bestAskList_.begin()->first / 100.0;
            } else if (!isSH_ && rec.priceType == '1') {
                // SZ 市价单: wt 行表示订单到达时的事件特征, 展示价取到达前对手盘首档
                // (买→卖一, 卖→买一)。价格笼子/最终扫档价属于撮合约束和后续 zb 结果,
                // 不反写到 wt.price/type, 避免使用 ex-post 成交极值。方案B不挂簿。
                if (rec.direction == 'B' && !bestAskList_.empty()) wtPrice = bestAskList_.begin()->first / 100.0;
                else if (rec.direction == 'S' && !bestBidList_.empty()) wtPrice = -bestBidList_.begin()->first / 100.0;
            }
            // SH 显式 A 委托: rec.price 就是真实限价(mdl A Price=限价, 非入场价), 直接用, 不用成交极值
            // 覆盖。成交极值补全只用于无 A 记录的派生单(见 createDerivedOrder)。

            // 点1: 分类/显示用补全量(=max(A_qty,成交+撤单)), 与 ref 语义一致(按原始全量判激进)。
            // SH 激进单 A_qty 只记挂住量; SZ orderFilled_ 空->不变。
            int64_t stateVol = rec.volume;
            { auto fIt = orderFilled_.find(rec.sysid);
              if (fIt != orderFilled_.end()) stateVol = std::max(rec.volume, fIt->second); }
            // 入簿量: resequence 提前的"先吃后挂"单, A_qty 已是挂住量且 handleTrade 会跳过
            // 其 A 前成交 -> 必须用 A_qty 入簿(用补全量会把先吃部分留成幻影挂单);
            // 非 resequence 单 orderFilled_ ≤ 原始量, stateVol == A_qty, 两者等价。
            int64_t bookVol = shOrderOrigSeq_.count(rec.sysid) ? rec.volume : stateVol;
            // 分类/位置用这笔新订单插入前的盘口状态算(用解析后的 wtPrice)
            auto [bp, ap]  = calcPosition(wtPrice, rec.direction);
            int  ord_type  = calcType(wtPrice, stateVol, rec.direction);
            handleOrder(rec.sysid, rec.time, wtPrice, bookVol, rec.direction, rec.timeSeconds,
                        rec.priceType);
            lr.ordertype   = "wt";
            lr.sysid       = rec.sysid;
            lr.direction   = rec.direction;
            lr.price       = wtPrice;
            // SH 立即成交主动单：A_qty 可能只记录了挂住部分，补全为 max(A_qty, 成交+撤单总量)
            {
                auto fIt = orderFilled_.find(rec.sysid);
                lr.volume = (fIt != orderFilled_.end())
                            ? std::max(rec.volume, fIt->second) : rec.volume;
            }
            lr.tradeVolume = cumulativeVolume_;
            lr.bidPosition = bp; lr.askPosition = ap;
            lr.type        = ord_type;
            // time_diff = 距上一条委托(生命周期语义): 首条=0, 跨午休减 5400s
            if (lastWtSec > 0) {
                lr.timeDiff = rec.timeSeconds - lastWtSec;
                if (lastWtSec < 43200 && rec.timeSeconds > 43200) lr.timeDiff -= 5400;
            }
            lastWtSec = rec.timeSeconds;

        } else if (rec.actionType == 1) {
            // 撤单 time_diff 需原委托时间
            double clOrigSec = -1.0;
            { auto it = allOrders_.find(rec.sysid);
              if (it != allOrders_.end() && it->second) clOrigSec = it->second->timeSeconds; }
            double cp; int64_t cv; char cd;
            if (handleCancel(rec.sysid, rec.time, rec.volume, cp, cv, cd)) {
                lr.ordertype   = "cl";
                lr.sysid       = rec.sysid;
                lr.direction   = cd;
                lr.price       = cp;
                lr.volume      = cv;
                lr.tradeVolume = cumulativeVolume_;
                auto [bp, ap]  = calcPosition(cp, cd);
                lr.bidPosition = bp; lr.askPosition = ap;
                if (clOrigSec >= 0) {
                    lr.timeDiff = rec.timeSeconds - clOrigSec;
                    if (clOrigSec < 43200 && rec.timeSeconds > 43200) lr.timeDiff -= 5400;
                }
            } else {
                // 孤儿撤单: 保留输出(与 tl/flow 一致), 用 rec 自带的价/量/向; type 默认 -1, time_diff=0
                lr.ordertype   = "cl";
                lr.sysid       = rec.sysid;
                lr.direction   = rec.direction;
                lr.price       = rec.price;
                lr.volume      = rec.volume;
                lr.tradeVolume = cumulativeVolume_;
            }

        } else {
            // 成交 time_diff = 成交时间 − 被动方原委托时间; 跨午休减 5400s。被动方按主动方向判定
            // (与 handleTrade 一致): SH 有 tradeDirection 时主动方不一定是大订单号, 不能写死 min。
            {
                int64_t passiveId;
                if (rec.tradeDirection == 1 || rec.tradeDirection == 2)
                    passiveId = (rec.tradeDirection == 1) ? rec.sellId : rec.buyId;
                else
                    passiveId = std::min(rec.buyId, rec.sellId);
                auto pit = allOrders_.find(passiveId);
                if (pit != allOrders_.end() && pit->second) {
                    double pts = pit->second->timeSeconds;
                    lr.timeDiff = rec.timeSeconds - pts;
                    if (pts < 43200 && rec.timeSeconds > 43200) lr.timeDiff -= 5400;
                }
            }
            // 成交 position 用「成交前」盘口 + 主动方向(与 tl/flow 一致), 在 handleTrade 改簿前算
            char preAd;
            if (rec.tradeDirection == 1 || rec.tradeDirection == 2)
                preAd = (rec.tradeDirection == 1) ? 'B' : 'S';
            else
                preAd = (rec.buyId > rec.sellId) ? 'B' : 'S';
            { auto [bp, ap] = calcPosition(rec.price, preAd);
              lr.bidPosition = bp; lr.askPosition = ap; }

            if (isSH_) {
                // createDerivedOrder 必须调用(其中 handleOrder 把派生单入簿); 仅 lite 时跳过记录 push
                if (!allOrders_.count(rec.buyId)) {
                    LobRecord dr; bool m = createDerivedOrder(rec.buyId,  rec.time, dr); if (m && !lite_) orderTable_.push_back(dr);
                }
                if (!allOrders_.count(rec.sellId)) {
                    LobRecord dr; bool m = createDerivedOrder(rec.sellId, rec.time, dr); if (m && !lite_) orderTable_.push_back(dr);
                }
            }

            char ad; int64_t asid, psid;
            handleTrade(rec.buyId, rec.sellId, rec.price, rec.volume,
                        rec.time, rec.tradeDirection, ad, asid, psid, rec.seqNo);

            lr.ordertype   = "zb";
            lr.sysid       = asid;
            lr.direction   = ad;
            lr.price       = rec.price;
            lr.volume      = rec.volume;
            lr.tradeVolume = cumulativeVolume_;
            lr.type        = -1;

            if (!lite_) {
                TradeRecord tr;
                tr.sysid    = asid;
                tr.wtVolume = rec.volume;
                tr.wtSec    = rec.timeSeconds;
                tr.transSt  = rec.timeSeconds; tr.transSs = rec.timeSeconds;
                tr.act      = (ad == 'B') ? 1 : 0;
                tr.transEt  = rec.timeSeconds; tr.transEs = rec.timeSeconds;
                tr.transVlm = rec.volume;
                tr.transAmt = rec.price * rec.volume; tr.vwap = rec.price;
                tr.actVlm   = (ad == 'B') ? rec.volume : 0;
                tr.actAmt   = (ad == 'B') ? rec.price * rec.volume : 0;
                tr.actNum   = (ad == 'B') ? 1 : 0;
                tr.pasVlm   = (ad == 'B') ? 0 : rec.volume;
                tr.pasAmt   = (ad == 'B') ? 0 : rec.price * rec.volume;
                tr.pasNum   = (ad == 'B') ? 0 : 1;
                tradeTable_.push_back(tr);
            }
        }

        if (!lite_) {
            // 距离/比例用「插入前」的盘口(已在循环顶部 getCurrentLob 取好), 对 wt/cl/zb 全算
            if (lr.bdp[0] > 0) {
                lr.bidDistance = lr.bdp[0] - lr.price;
                if (lr.bdv[0] > 0 && lr.volume > 0)
                    lr.bidRatio = static_cast<double>(lr.volume) / lr.bdv[0];
            }
            if (lr.akp[0] > 0) {
                lr.askDistance = lr.price - lr.akp[0];
                if (lr.akv[0] > 0 && lr.volume > 0)
                    lr.askRatio = static_cast<double>(lr.volume) / lr.akv[0];
            }
            orderTable_.push_back(lr);
        }

        // vorder_sim: 每条记录处理完(改簿后)旁挂虚拟订单追踪器。只读, 不影响真实簿。
        if (onEvent_) onEvent_(rec, *this);
    }

    // 不再追加 15:00:01 合成"最终记录"(会被下游当真委托统计、多出一行); 收盘簿由真实收盘成交
    // 收口。与 tl_lob_builder / flow_lob_builder 一致。
}

// ─────────────────────────────────────────────────────────────────────────────

void LobBuilder::writeOrderTableCSV(const std::string& path) const {
    std::ofstream f(path);
    if (!f) { std::cerr << "Cannot write: " << path << "\n"; return; }

    f << "time,sysid,ordertype,direction,price,volume,tradeVolume,"
         "bidPosition,askPosition,bidDistance,askDistance,bidRatio,askRatio,"
         "timeDiff,type,bidLevels,askLevels";
    for (int i = 0; i < LOB_LEVELS; ++i) f << ",bdp" << (i+1) << ",bdv" << (i+1);
    for (int i = 0; i < LOB_LEVELS; ++i) f << ",akp" << (i+1) << ",akv" << (i+1);
    for (int i = 0; i < LOB_LEVELS; ++i) f << ",bid_num" << (i+1);
    for (int i = 0; i < LOB_LEVELS; ++i) f << ",ask_num" << (i+1);
    f << "\n";

    for (const auto& r : orderTable_) {
        f << r.time << "," << r.sysid << "," << r.ordertype << ","
          << r.direction << "," << r.price << "," << r.volume << ","
          << r.tradeVolume << "," << r.bidPosition << "," << r.askPosition << ","
          << r.bidDistance << "," << r.askDistance << ","
          << r.bidRatio << "," << r.askRatio << ","
          << r.timeDiff << "," << r.type << ","
          << r.bidLevels << "," << r.askLevels;
        for (int i = 0; i < LOB_LEVELS; ++i) f << "," << r.bdp[i] << "," << r.bdv[i];
        for (int i = 0; i < LOB_LEVELS; ++i) f << "," << r.akp[i] << "," << r.akv[i];
        for (int i = 0; i < LOB_LEVELS; ++i) f << "," << r.bdn[i];
        for (int i = 0; i < LOB_LEVELS; ++i) f << "," << r.akn[i];
        f << "\n";
    }
}

void LobBuilder::writeTradeTableCSV(const std::string& path) const {
    std::ofstream f(path);
    if (!f) { std::cerr << "Cannot write: " << path << "\n"; return; }

    f << "sysid,wt_volume,wt_time,wt_sec,trans_st,trans_ss,act,"
         "trans_et,trans_es,cl_time,cl_sec,cl_vlm,trans_vlm,trans_amt,vwap,"
         "act_vlm,act_amt,act_num,act_pnm,pas_vlm,pas_amt,pas_num,pas_pnm,"
         "wt_ts,ts_te,wt_cl\n";

    f << std::fixed << std::setprecision(3);
    for (const auto& r : tradeTable_) {
        f << r.sysid << "," << r.wtVolume << "," << r.wtTime << ","
          << r.wtSec << "," << r.transSt << "," << r.transSs << ","
          << r.act << "," << r.transEt << "," << r.transEs << ","
          << r.clTime << "," << r.clSec << "," << r.clVlm << ","
          << r.transVlm << "," << r.transAmt << "," << r.vwap << ","
          << r.actVlm << "," << r.actAmt << "," << r.actNum << ","
          << r.actPnm << "," << r.pasVlm << "," << r.pasAmt << ","
          << r.pasNum << "," << r.pasPnm << ","
          << r.wtTs << "," << r.tsTe << "," << r.wtCl << "\n";
    }
}

// ── HDF5 输出 (compound type, 与 TLLobBuilder 格式兼容) ───────────────────────

namespace {

struct H5OrderRow {
    char    time[16];
    int64_t sysid;
    char    ordertype[4];
    char    direction[2];
    double  price;
    int64_t volume;
    int64_t tradevolume;
    int64_t bid_position;
    int64_t ask_position;
    double  bid_distance;
    double  ask_distance;
    double  bid_ratio;
    double  ask_ratio;
    double  time_diff;
    int64_t type;
    int64_t bid_levels;
    int64_t ask_levels;
    double  bdp1,  bdp2,  bdp3,  bdp4,  bdp5,  bdp6,  bdp7,  bdp8,  bdp9,  bdp10;
    double  bdv1,  bdv2,  bdv3,  bdv4,  bdv5,  bdv6,  bdv7,  bdv8,  bdv9,  bdv10;
    double  akp1,  akp2,  akp3,  akp4,  akp5,  akp6,  akp7,  akp8,  akp9,  akp10;
    double  akv1,  akv2,  akv3,  akv4,  akv5,  akv6,  akv7,  akv8,  akv9,  akv10;
    int64_t bid_num1, bid_num2, bid_num3, bid_num4, bid_num5, bid_num6, bid_num7, bid_num8, bid_num9, bid_num10;
    int64_t ask_num1, ask_num2, ask_num3, ask_num4, ask_num5, ask_num6, ask_num7, ask_num8, ask_num9, ask_num10;
};

struct H5TradeRow {
    int64_t sysid;
    int64_t wt_volume;
    int64_t wt_time;
    double  wt_sec;
    double  trans_st;
    double  trans_ss;
    int64_t act;
    double  trans_et;
    double  trans_es;
    double  cl_time;
    double  cl_sec;
    int64_t cl_vlm;
    int64_t trans_vlm;
    double  trans_amt;
    double  vwap;
    int64_t act_vlm;
    double  act_amt;
    int64_t act_num;
    int64_t act_pnm;
    int64_t pas_vlm;
    double  pas_amt;
    int64_t pas_num;
    int64_t pas_pnm;
    double  wt_ts;
    double  ts_te;
    double  wt_cl;
};

} // anon namespace

void LobBuilder::writeOrderTableH5(const std::string& path) const {
    try {
        H5::Exception::dontPrint();
        H5::H5File file(path, H5F_ACC_TRUNC);

        H5::CompType ct(sizeof(H5OrderRow));
        H5::StrType  st16(H5::PredType::C_S1, 16);
        H5::StrType  st4 (H5::PredType::C_S1, 4);
        H5::StrType  st2 (H5::PredType::C_S1, 2);

        ct.insertMember("time",         HOFFSET(H5OrderRow, time),         st16);
        ct.insertMember("sysid",        HOFFSET(H5OrderRow, sysid),        H5::PredType::NATIVE_INT64);
        ct.insertMember("ordertype",    HOFFSET(H5OrderRow, ordertype),    st4);
        ct.insertMember("direction",    HOFFSET(H5OrderRow, direction),    st2);
        ct.insertMember("price",        HOFFSET(H5OrderRow, price),        H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("volume",       HOFFSET(H5OrderRow, volume),       H5::PredType::NATIVE_INT64);
        ct.insertMember("tradevolume",  HOFFSET(H5OrderRow, tradevolume),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_position", HOFFSET(H5OrderRow, bid_position), H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_position", HOFFSET(H5OrderRow, ask_position), H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_distance", HOFFSET(H5OrderRow, bid_distance), H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("ask_distance", HOFFSET(H5OrderRow, ask_distance), H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bid_ratio",    HOFFSET(H5OrderRow, bid_ratio),    H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("ask_ratio",    HOFFSET(H5OrderRow, ask_ratio),    H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("time_diff",    HOFFSET(H5OrderRow, time_diff),    H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("type",         HOFFSET(H5OrderRow, type),         H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_levels",   HOFFSET(H5OrderRow, bid_levels),   H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_levels",   HOFFSET(H5OrderRow, ask_levels),   H5::PredType::NATIVE_INT64);
        ct.insertMember("bdp1",  HOFFSET(H5OrderRow, bdp1),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp2",  HOFFSET(H5OrderRow, bdp2),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp3",  HOFFSET(H5OrderRow, bdp3),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp4",  HOFFSET(H5OrderRow, bdp4),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp5",  HOFFSET(H5OrderRow, bdp5),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp6",  HOFFSET(H5OrderRow, bdp6),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp7",  HOFFSET(H5OrderRow, bdp7),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp8",  HOFFSET(H5OrderRow, bdp8),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp9",  HOFFSET(H5OrderRow, bdp9),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdp10", HOFFSET(H5OrderRow, bdp10), H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv1",  HOFFSET(H5OrderRow, bdv1),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv2",  HOFFSET(H5OrderRow, bdv2),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv3",  HOFFSET(H5OrderRow, bdv3),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv4",  HOFFSET(H5OrderRow, bdv4),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv5",  HOFFSET(H5OrderRow, bdv5),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv6",  HOFFSET(H5OrderRow, bdv6),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv7",  HOFFSET(H5OrderRow, bdv7),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv8",  HOFFSET(H5OrderRow, bdv8),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv9",  HOFFSET(H5OrderRow, bdv9),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bdv10", HOFFSET(H5OrderRow, bdv10), H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp1",  HOFFSET(H5OrderRow, akp1),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp2",  HOFFSET(H5OrderRow, akp2),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp3",  HOFFSET(H5OrderRow, akp3),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp4",  HOFFSET(H5OrderRow, akp4),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp5",  HOFFSET(H5OrderRow, akp5),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp6",  HOFFSET(H5OrderRow, akp6),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp7",  HOFFSET(H5OrderRow, akp7),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp8",  HOFFSET(H5OrderRow, akp8),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp9",  HOFFSET(H5OrderRow, akp9),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akp10", HOFFSET(H5OrderRow, akp10), H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv1",  HOFFSET(H5OrderRow, akv1),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv2",  HOFFSET(H5OrderRow, akv2),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv3",  HOFFSET(H5OrderRow, akv3),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv4",  HOFFSET(H5OrderRow, akv4),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv5",  HOFFSET(H5OrderRow, akv5),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv6",  HOFFSET(H5OrderRow, akv6),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv7",  HOFFSET(H5OrderRow, akv7),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv8",  HOFFSET(H5OrderRow, akv8),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv9",  HOFFSET(H5OrderRow, akv9),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("akv10", HOFFSET(H5OrderRow, akv10), H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("bid_num1",  HOFFSET(H5OrderRow, bid_num1),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num2",  HOFFSET(H5OrderRow, bid_num2),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num3",  HOFFSET(H5OrderRow, bid_num3),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num4",  HOFFSET(H5OrderRow, bid_num4),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num5",  HOFFSET(H5OrderRow, bid_num5),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num6",  HOFFSET(H5OrderRow, bid_num6),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num7",  HOFFSET(H5OrderRow, bid_num7),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num8",  HOFFSET(H5OrderRow, bid_num8),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num9",  HOFFSET(H5OrderRow, bid_num9),  H5::PredType::NATIVE_INT64);
        ct.insertMember("bid_num10", HOFFSET(H5OrderRow, bid_num10), H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num1",  HOFFSET(H5OrderRow, ask_num1),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num2",  HOFFSET(H5OrderRow, ask_num2),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num3",  HOFFSET(H5OrderRow, ask_num3),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num4",  HOFFSET(H5OrderRow, ask_num4),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num5",  HOFFSET(H5OrderRow, ask_num5),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num6",  HOFFSET(H5OrderRow, ask_num6),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num7",  HOFFSET(H5OrderRow, ask_num7),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num8",  HOFFSET(H5OrderRow, ask_num8),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num9",  HOFFSET(H5OrderRow, ask_num9),  H5::PredType::NATIVE_INT64);
        ct.insertMember("ask_num10", HOFFSET(H5OrderRow, ask_num10), H5::PredType::NATIVE_INT64);

        std::vector<H5OrderRow> rows(orderTable_.size());
        for (size_t i = 0; i < orderTable_.size(); ++i) {
            const auto& r = orderTable_[i];
            H5OrderRow& row = rows[i];
            strncpy(row.time,      r.time.c_str(),      15); row.time[15]      = '\0';
            strncpy(row.ordertype, r.ordertype.c_str(),  3); row.ordertype[3]  = '\0';
            row.direction[0] = r.direction; row.direction[1] = '\0';
            row.sysid        = r.sysid;
            row.price        = r.price;
            row.volume       = r.volume;
            row.tradevolume  = r.tradeVolume;
            row.bid_position = r.bidPosition;
            row.ask_position = r.askPosition;
            row.bid_distance = r.bidDistance;
            row.ask_distance = r.askDistance;
            row.bid_ratio    = r.bidRatio;
            row.ask_ratio    = r.askRatio;
            row.time_diff    = r.timeDiff;
            row.type         = r.type;
            row.bid_levels   = r.bidLevels;
            row.ask_levels   = r.askLevels;
            row.bdp1  = r.bdp[0]; row.bdp2  = r.bdp[1]; row.bdp3  = r.bdp[2];
            row.bdp4  = r.bdp[3]; row.bdp5  = r.bdp[4]; row.bdp6  = r.bdp[5];
            row.bdp7  = r.bdp[6]; row.bdp8  = r.bdp[7]; row.bdp9  = r.bdp[8];
            row.bdp10 = r.bdp[9];
            row.bdv1  = static_cast<double>(r.bdv[0]); row.bdv2  = static_cast<double>(r.bdv[1]);
            row.bdv3  = static_cast<double>(r.bdv[2]); row.bdv4  = static_cast<double>(r.bdv[3]);
            row.bdv5  = static_cast<double>(r.bdv[4]); row.bdv6  = static_cast<double>(r.bdv[5]);
            row.bdv7  = static_cast<double>(r.bdv[6]); row.bdv8  = static_cast<double>(r.bdv[7]);
            row.bdv9  = static_cast<double>(r.bdv[8]); row.bdv10 = static_cast<double>(r.bdv[9]);
            row.akp1  = r.akp[0]; row.akp2  = r.akp[1]; row.akp3  = r.akp[2];
            row.akp4  = r.akp[3]; row.akp5  = r.akp[4]; row.akp6  = r.akp[5];
            row.akp7  = r.akp[6]; row.akp8  = r.akp[7]; row.akp9  = r.akp[8];
            row.akp10 = r.akp[9];
            row.akv1  = static_cast<double>(r.akv[0]); row.akv2  = static_cast<double>(r.akv[1]);
            row.akv3  = static_cast<double>(r.akv[2]); row.akv4  = static_cast<double>(r.akv[3]);
            row.akv5  = static_cast<double>(r.akv[4]); row.akv6  = static_cast<double>(r.akv[5]);
            row.akv7  = static_cast<double>(r.akv[6]); row.akv8  = static_cast<double>(r.akv[7]);
            row.akv9  = static_cast<double>(r.akv[8]); row.akv10 = static_cast<double>(r.akv[9]);
            row.bid_num1 = r.bdn[0]; row.bid_num2 = r.bdn[1]; row.bid_num3 = r.bdn[2];
            row.bid_num4 = r.bdn[3]; row.bid_num5 = r.bdn[4]; row.bid_num6 = r.bdn[5];
            row.bid_num7 = r.bdn[6]; row.bid_num8 = r.bdn[7]; row.bid_num9 = r.bdn[8];
            row.bid_num10 = r.bdn[9];
            row.ask_num1 = r.akn[0]; row.ask_num2 = r.akn[1]; row.ask_num3 = r.akn[2];
            row.ask_num4 = r.akn[3]; row.ask_num5 = r.akn[4]; row.ask_num6 = r.akn[5];
            row.ask_num7 = r.akn[6]; row.ask_num8 = r.akn[7]; row.ask_num9 = r.akn[8];
            row.ask_num10 = r.akn[9];
        }

        hsize_t dims[1] = {rows.size()};
        H5::DataSpace space(1, dims);
        H5::DSetCreatPropList plist;
        hsize_t chunk[1] = {std::min<hsize_t>(rows.size(), 10000)};
        if (chunk[0] > 0) { plist.setChunk(1, chunk); plist.setDeflate(1); }
        H5::DataSet ds = file.createDataSet("order", ct, space, plist);
        if (!rows.empty()) ds.write(rows.data(), ct);
        file.close();
    } catch (const H5::Exception& e) {
        std::cerr << "H5 error (order): " << e.getCDetailMsg() << "\n";
    }
}

void LobBuilder::writeTradeTableH5(const std::string& path) const {
    try {
        H5::Exception::dontPrint();
        H5::H5File file(path, H5F_ACC_RDWR);   // order was already written

        H5::CompType ct(sizeof(H5TradeRow));
        ct.insertMember("sysid",     HOFFSET(H5TradeRow, sysid),     H5::PredType::NATIVE_INT64);
        ct.insertMember("wt_volume", HOFFSET(H5TradeRow, wt_volume), H5::PredType::NATIVE_INT64);
        ct.insertMember("wt_time",   HOFFSET(H5TradeRow, wt_time),   H5::PredType::NATIVE_INT64);
        ct.insertMember("wt_sec",    HOFFSET(H5TradeRow, wt_sec),    H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("trans_st",  HOFFSET(H5TradeRow, trans_st),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("trans_ss",  HOFFSET(H5TradeRow, trans_ss),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("act",       HOFFSET(H5TradeRow, act),       H5::PredType::NATIVE_INT64);
        ct.insertMember("trans_et",  HOFFSET(H5TradeRow, trans_et),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("trans_es",  HOFFSET(H5TradeRow, trans_es),  H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("cl_time",   HOFFSET(H5TradeRow, cl_time),   H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("cl_sec",    HOFFSET(H5TradeRow, cl_sec),    H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("cl_vlm",    HOFFSET(H5TradeRow, cl_vlm),    H5::PredType::NATIVE_INT64);
        ct.insertMember("trans_vlm", HOFFSET(H5TradeRow, trans_vlm), H5::PredType::NATIVE_INT64);
        ct.insertMember("trans_amt", HOFFSET(H5TradeRow, trans_amt), H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("vwap",      HOFFSET(H5TradeRow, vwap),      H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("act_vlm",   HOFFSET(H5TradeRow, act_vlm),   H5::PredType::NATIVE_INT64);
        ct.insertMember("act_amt",   HOFFSET(H5TradeRow, act_amt),   H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("act_num",   HOFFSET(H5TradeRow, act_num),   H5::PredType::NATIVE_INT64);
        ct.insertMember("act_pnm",   HOFFSET(H5TradeRow, act_pnm),   H5::PredType::NATIVE_INT64);
        ct.insertMember("pas_vlm",   HOFFSET(H5TradeRow, pas_vlm),   H5::PredType::NATIVE_INT64);
        ct.insertMember("pas_amt",   HOFFSET(H5TradeRow, pas_amt),   H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("pas_num",   HOFFSET(H5TradeRow, pas_num),   H5::PredType::NATIVE_INT64);
        ct.insertMember("pas_pnm",   HOFFSET(H5TradeRow, pas_pnm),   H5::PredType::NATIVE_INT64);
        ct.insertMember("wt_ts",     HOFFSET(H5TradeRow, wt_ts),     H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("ts_te",     HOFFSET(H5TradeRow, ts_te),     H5::PredType::NATIVE_DOUBLE);
        ct.insertMember("wt_cl",     HOFFSET(H5TradeRow, wt_cl),     H5::PredType::NATIVE_DOUBLE);

        std::vector<H5TradeRow> rows(tradeTable_.size());
        for (size_t i = 0; i < tradeTable_.size(); ++i) {
            const auto& r = tradeTable_[i];
            H5TradeRow& row = rows[i];
            row.sysid     = r.sysid;
            row.wt_volume = r.wtVolume;
            row.wt_time   = r.wtTime;
            row.wt_sec    = r.wtSec;
            row.trans_st  = r.transSt;
            row.trans_ss  = r.transSs;
            row.act       = r.act;
            row.trans_et  = r.transEt;
            row.trans_es  = r.transEs;
            row.cl_time   = r.clTime;
            row.cl_sec    = r.clSec;
            row.cl_vlm    = r.clVlm;
            row.trans_vlm = r.transVlm;
            row.trans_amt = r.transAmt;
            row.vwap      = r.vwap;
            row.act_vlm   = r.actVlm;
            row.act_amt   = r.actAmt;
            row.act_num   = r.actNum;
            row.act_pnm   = r.actPnm;
            row.pas_vlm   = r.pasVlm;
            row.pas_amt   = r.pasAmt;
            row.pas_num   = r.pasNum;
            row.pas_pnm   = r.pasPnm;
            row.wt_ts     = r.wtTs;
            row.ts_te     = r.tsTe;
            row.wt_cl     = r.wtCl;
        }

        hsize_t dims[1] = {rows.size()};
        H5::DataSpace space(1, dims);
        H5::DSetCreatPropList plist;
        hsize_t chunk[1] = {std::min<hsize_t>(rows.size(), 10000)};
        if (chunk[0] > 0) { plist.setChunk(1, chunk); plist.setDeflate(1); }
        H5::DataSet ds = file.createDataSet("trade_1500", ct, space, plist);
        if (!rows.empty()) ds.write(rows.data(), ct);
        file.close();
    } catch (const H5::Exception& e) {
        std::cerr << "H5 error (trade): " << e.getCDetailMsg() << "\n";
    }
}

} // namespace dc
