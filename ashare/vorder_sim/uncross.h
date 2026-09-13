#pragma once
// 集合竞价虚拟撮合 (uncross) —— 移植自 auction 项目 computeAuctionSnapshot 的清算价算法。
// 对聚合盘口(价->量)求 A 股集合竞价均衡价 p*: 最大成交量 → 买卖失衡最小 → 价高者优先。
// 只读、纯函数, 不改任何簿。

#include <map>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <climits>

namespace vsim {

struct UncrossResult {
    bool    has = false;         // 是否存在可撮合价
    int64_t clearIntPrice = 0;   // 均衡价 p* (整数分)
    int64_t maxVol = 0;          // 可撮合最大成交量(两侧匹配量)
};

// bestBid: key = -intPrice -> vol (升序即价高在前); bestAsk: key = intPrice -> vol (升序即价低在前)
inline UncrossResult uncross(const std::map<int64_t,int64_t>& bestBid,
                             const std::map<int64_t,int64_t>& bestAsk) {
    UncrossResult r;
    if (bestBid.empty() || bestAsk.empty()) return r;

    // 候选价 = 买卖所有价位并集
    std::vector<int64_t> prices;
    prices.reserve(bestBid.size() + bestAsk.size());
    for (const auto& kv : bestBid) prices.push_back(-kv.first);
    for (const auto& kv : bestAsk) prices.push_back(kv.first);
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());

    int64_t bestExec = 0, bestImb = INT64_MAX, clearP = 0;
    for (int64_t p : prices) {
        int64_t demand = 0, supply = 0;          // demand: 买价>=p; supply: 卖价<=p
        for (const auto& kv : bestBid) { int64_t bp = -kv.first; if (bp >= p) demand += kv.second; }
        for (const auto& kv : bestAsk) { int64_t ap =  kv.first; if (ap <= p) supply += kv.second; }
        int64_t exec = std::min(demand, supply);
        if (exec <= 0) continue;
        int64_t imb = (demand > supply) ? (demand - supply) : (supply - demand);
        // 最大成交量 → 失衡最小 → 价高
        if (exec > bestExec ||
            (exec == bestExec && imb < bestImb) ||
            (exec == bestExec && imb == bestImb && p > clearP)) {
            bestExec = exec; bestImb = imb; clearP = p;
        }
    }
    if (bestExec <= 0) return r;
    r.has = true; r.clearIntPrice = clearP; r.maxVol = bestExec;
    return r;
}

// 给定撮合结果与聚合盘口, 求「我方(side)严格优于 p* 的挂单总量」(不含 p* 那一档)。
// 买单严格优 = 价>p*; 卖单严格优 = 价<p*。用于判定虚拟单在竞价里是全成还是边际部分成。
inline int64_t sideVolStrictlyBetter(char side, int64_t clearP,
                                     const std::map<int64_t,int64_t>& bestBid,
                                     const std::map<int64_t,int64_t>& bestAsk) {
    int64_t v = 0;
    if (side == 'B') {
        for (const auto& kv : bestBid) { int64_t bp = -kv.first; if (bp > clearP) v += kv.second; }
    } else {
        for (const auto& kv : bestAsk) { int64_t ap =  kv.first; if (ap < clearP) v += kv.second; }
    }
    return v;
}

// 我方(side)在 p* 能撮到的「对手可成交量」: 买单 → 卖价<=p* 的总供给; 卖单 → 买价>=p* 的总需求。
// 用于给虚拟单竞价成交量封顶, 防止委托量 > 实际对手深度时超额成交。
inline int64_t oppositeExecutableVol(char side, int64_t clearP,
                                     const std::map<int64_t,int64_t>& bestBid,
                                     const std::map<int64_t,int64_t>& bestAsk) {
    int64_t v = 0;
    if (side == 'B') {
        for (const auto& kv : bestAsk) { int64_t ap =  kv.first; if (ap <= clearP) v += kv.second; }
    } else {
        for (const auto& kv : bestBid) { int64_t bp = -kv.first; if (bp >= clearP) v += kv.second; }
    }
    return v;
}

} // namespace vsim
