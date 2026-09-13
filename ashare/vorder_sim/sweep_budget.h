#pragma once
// 主动单预算 Q: 每笔委托的【原始委托数量】。
//
// 虚拟单的成交必须受"造成这笔成交的主动单还剩多少量"约束, 否则多笔虚拟单会各自
// 认领同一批流动性(凭空超发)。Q 就是这个共享预算的初值。
//
// 两个市场的委托记录完整度不同(实测 20260707):
//   SZ: 委托记录在成交之前, 且带【完整原始数量】       → Q = 委托量                (100% 覆盖)
//   SH: 立即全成的主动单【根本没有委托记录】(约41%)     → Q = 该单总成交量
//       有委托记录的, 委托量只记【挂住量】(先吃部分被净掉) → Q = 挂住量 + 委托记录之前已成交的量
// 统一成一条规则(用【原始 seqNo】判断"委托记录之前"):
//   Q(X) = 有委托记录 ? (委托量 + 该单在其委托记录 seqNo 之前的成交量) : (该单总成交量)

#include "lob_builder.h"
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace vsim {

// 输入: 某股票【未经重排的原始】UnifiedRecord 序列(seqNo 即交易所原始序)
// 输出: orderId -> 原始委托数量 Q
inline std::unordered_map<int64_t,int64_t>
computeOrderBudget(const std::vector<dc::UnifiedRecord>& recs) {
    std::unordered_map<int64_t,int64_t> ordVol, ordSeq, tradedBefore, tradedTotal;
    ordVol.reserve(recs.size()); tradedTotal.reserve(recs.size());

    for (const auto& r : recs) {
        if (r.actionType == 0 && r.sysid > 0) { ordVol[r.sysid] = r.volume; ordSeq[r.sysid] = r.seqNo; }
    }
    for (const auto& r : recs) {
        if (r.actionType != 2) continue;
        for (int64_t id : { r.buyId, r.sellId }) {
            if (id <= 0) continue;
            tradedTotal[id] += r.volume;
            auto it = ordSeq.find(id);
            // SH "先吃后挂": 委托记录排在其立即成交之后 → 这些成交的 seqNo 小于委托记录 seqNo,
            // 且已从委托量里净掉, 必须加回才是原始委托量。SZ 委托在前 → 恒为 0。
            if (it != ordSeq.end() && r.seqNo < it->second) tradedBefore[id] += r.volume;
        }
    }

    std::unordered_map<int64_t,int64_t> Q;
    Q.reserve(tradedTotal.size());
    for (const auto& kv : tradedTotal) {
        int64_t id = kv.first;
        auto io = ordVol.find(id);
        if (io != ordVol.end()) {
            int64_t before = 0;
            auto ib = tradedBefore.find(id); if (ib != tradedBefore.end()) before = ib->second;
            Q[id] = io->second + before;              // 挂住量 + 先吃量 = 原始量 (SZ: before=0)
        } else {
            Q[id] = kv.second;                        // SH 派生单(无委托记录): 立即全成 → 原始量=总成交量
        }
    }
    return Q;
}

} // namespace vsim
