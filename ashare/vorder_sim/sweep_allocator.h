#pragma once
// SweepAllocator: 每(股票, 口径)一个。把该股票的所有虚拟腿挂在一起, 用
// 【造成这笔成交的主动单的剩余预算 Q】统一分配成交。真实盘口全程只读, 一个字节不改。
//
// 为什么需要它:
//   独立评估时每条腿都假设"只有我一个", 于是同一批流动性被多条腿各自认领 —— 凭空超发:
//     · 同价多腿      → 同一份溢出量被算两遍
//     · 异价多腿(买一/买二) → 同一笔扫单的量被两档各自认领
//   本分配器让所有腿从【同一个 Q】里按 (价格优先 → 插入先后) 依次取量, 取完为止。
//   同价串行 / 异价耦合 由这一个机制统一解决。
//
// 分配顺序(一笔成交价 P、成交量 V、主动单剩余预算 b):
//   ① 主动单已【越过】的腿(价格严格优于 P、且已到队首) → 先成交(它们排在 P 档真实单之前)
//   ② 本档 P: 交替消耗 [我前方的真实单] → [我] → (下一条腿) …
//   ③ 本档队尾真实单吃掉剩余
//
// 位移计 displaced(): 预算被吃到【负】的那部分 = 你从真实单嘴里挤走的量。
//   =0 → 零冲击(你吃的是主动单没消化掉的残量);
//   越大 → 无冲击假设被违反得越厉害, 结果越不可信。

#include "virtual_order_tracker.h"
#include "sweep_budget.h"
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>

namespace vsim {

class SweepAllocator {
public:
    SweepAllocator(Mode mode, const std::unordered_map<int64_t,int64_t>* orderQ)
        : mode_(mode), orderQ_(orderQ) {}

    void addLeg(VirtualOrderTracker* t) {
        all_.push_back(t);
        // marketable 跨价: 同一条记录上插入的多条腿共享可见对手深度; 吃掉的真实挂单全额计入位移
        t->setCrossOverlay(&crossUsed_, &displaced_);
        (t->sideChar() == 'B' ? buyLegs_ : sellLegs_).push_back(t);
        // 价格优先(买: 高价先; 卖: 低价先) → 插入先后
        auto by = [](VirtualOrderTracker* a, VirtualOrderTracker* b, bool buy){
            if (a->levelPrice() != b->levelPrice())
                return buy ? (a->levelPrice() > b->levelPrice())
                           : (a->levelPrice() < b->levelPrice());
            return a->insertSec() < b->insertSec();
        };
        std::stable_sort(buyLegs_.begin(),  buyLegs_.end(),
                         [&](auto* a, auto* c){ return by(a,c,true ); });
        std::stable_sort(sellLegs_.begin(), sellLegs_.end(),
                         [&](auto* a, auto* c){ return by(a,c,false); });
    }

    void onEvent(const dc::UnifiedRecord& rec, const dc::LobBuilder& book) {
        // OPT/PESS: 同毫秒内「撤单 vs 成交」先后不确定 → 缓冲到毫秒末按口径重排后统一 replay。
        // 缓冲放在【分配器】而非每条腿 → 全篮子共用同一个重排, 篮子级区间才自洽(可相加)。
        if (mode_ != Mode::NEUTRAL) {
            if (bufActive_ && rec.time != bufTime_) flush(book);
            // 只在【连续竞价段】缓冲: 集合竞价撮合是同时性的, 不存在"同毫秒撤单/成交先后"
            // 歧义; 若把 09:25/15:00 的竞价成交也缓冲, 会推迟到簿已解叉之后才 replay,
            // uncross 无叉可撮 → 上下界口径的竞价成交被整段清零(实测 case8 pess/opt=0)。
            bool contSession = rec.timeSeconds >= CONT_AM_START
                            && rec.timeSeconds <  CLOSE_AUC_START;
            if (contSession && (rec.actionType == 1 || rec.actionType == 2)) {
                buf_.push_back(rec); bufTime_ = rec.time; bufActive_ = true; return;
            }
        }
        apply(rec, book);
    }

    void finalize(const dc::LobBuilder& book) {
        if (bufActive_) flush(book);
        for (auto* t : all_) t->finalize(book);
    }

    int64_t displaced() const { return displaced_; }   // 从真实单挤走的量(位移计)

private:
    void flush(const dc::LobBuilder& book) {
        std::stable_sort(buf_.begin(), buf_.end(),
            [this](const dc::UnifiedRecord& a, const dc::UnifiedRecord& b){
                auto key = [this](const dc::UnifiedRecord& r){
                    if (mode_ == Mode::OPTIMISTIC) return r.actionType == 1 ? 0 : 1;  // 先撤: 前方让路(上界)
                    else                           return r.actionType == 2 ? 0 : 1;  // 先成交: 前方先被吃(下界)
                };
                return key(a) < key(b);
            });
        for (const auto& r : buf_) apply(r, book);
        buf_.clear(); bufActive_ = false;
    }

    void apply(const dc::UnifiedRecord& rec, const dc::LobBuilder& book) {
        // marketable 共享层按【记录】重置: 只有在同一条记录上插入(=同一 t0)的腿才共享那一刻的可见深度。
        // 跨记录不累计 —— 那属于"影子层读的是真实簿"的漂移问题, 与被动路径同一近似。
        crossUsed_.clear();
        for (auto* t : all_) t->onEvent(rec, book);    // 插入(含 marketable 跨价)/竞价/撤单/队列维护
        if (rec.actionType == 2) allocate(rec);        // 成交 → 按主动单预算跨腿分配
    }

    // 主动方: SH 用 tradeDirection(1买/2卖); SZ 无该字段 → 大 ApplSeqNum(后到)为主动
    static char activeSideOf(const dc::UnifiedRecord& r) {
        if (r.tradeDirection == 1) return 'B';
        if (r.tradeDirection == 2) return 'S';
        return (r.buyId > r.sellId) ? 'B' : 'S';
    }

    // 真实单吃掉 qty: 扣预算; 预算被吃到负 → 那部分就是从真实单挤走的量
    void chargeReal(int64_t& b, int64_t qty) {
        if (qty <= 0) return;
        b -= qty;
        if (b < 0) displaced_ += std::min(qty, -b);
    }

    void allocate(const dc::UnifiedRecord& rec) {
        const char act = activeSideOf(rec);
        const int64_t aggr = (act == 'B') ? rec.buyId : rec.sellId;

        // 主动单可识别 → 该单一个共享预算(跨档跨腿); 识别不到(如合成数据无订单号)
        // → 退化为"每笔成交自成一个预算", 等价于无跨笔约束的旧行为。
        int64_t local = rec.volume;
        int64_t* bp = &local;
        if (aggr > 0) {
            auto ib = budget_.find(aggr);
            if (ib == budget_.end()) {
                int64_t q = rec.volume;                   // 兜底(查不到委托 → 至少这一笔)
                if (orderQ_) { auto it = orderQ_->find(aggr); if (it != orderQ_->end()) q = it->second; }
                ib = budget_.emplace(aggr, q).first;
            }
            bp = &ib->second;
        }
        int64_t& b = *bp;

        const int64_t P = rec.intPrice;
        int64_t remV = rec.volume;                        // 本笔成交里"真实单"吃掉的量
        // 只有【对手方主动】能填我: 我买单←卖方主动, 我卖单←买方主动
        auto& side = (act == 'B') ? sellLegs_ : buyLegs_;

        for (auto* t : side) {
            if (t->preInsert()) {                         // t0 前: 只维护队列, 不成交、不占预算
                if (t->levelPrice() == P) t->consumeFrontPreInsert(rec.volume);
                continue;
            }
            if (t->terminal()) continue;
            // 竞价段的腿: uncross 已一次性结清其竞价份额(成交 + 前方队列消耗 marginalCleared)。
            // 09:25/15:00 的竞价成交【打印】只是同一批量的磁带记录, 不得再消耗其 aheadQty
            // —— 否则前方被双扣(实测 000566: 18.88M 被扣成 0.25M), 收盘腿的 ahead_remaining 报错值。
            if (t->inAuctionPhase()) continue;

            if (t->strictlyBetterThan(P)) {               // ① 主动单已越过我 → 排在本档真实单之前
                if (t->aheadQty() == 0 && b > 0 && t->remaining() > 0)
                    b -= t->fillFromBudget(b, rec);
                continue;
            }
            if (t->levelPrice() != P) continue;           // 更差的档: 还没扫到

            // ② 本档: 交替消耗 [我前方的真实单] / [我]
            if (t->aheadQty() > 0 && remV > 0) {
                int64_t take = std::min(t->aheadQty(), remV);
                // 这些真实单排在本腿之前 → 也排在本档所有腿之前, 各腿的前方量同减。
                // 必须跳过已终止(FILLED/CANCELLED)的腿: 它们已经不在队列里了, 再扣就把
                // "撤单那一刻我前方还剩多少"这个结果给改坏了(不影响成交, 但 ahead_remaining 会报错值)。
                for (auto* u : side)
                    if (!u->preInsert() && !u->terminal() && !u->inAuctionPhase() &&
                        u->levelPrice() == P) u->consumeAhead(take);
                remV -= take;
                chargeReal(b, take);
            }
            if (t->aheadQty() == 0 && b > 0 && t->remaining() > 0)
                b -= t->fillFromBudget(b, rec);
        }
        chargeReal(b, remV);                              // ③ 本档队尾真实单(或本档无腿时的全部)
    }

    Mode mode_;
    const std::unordered_map<int64_t,int64_t>* orderQ_;   // orderId -> 原始委托量 Q
    std::vector<VirtualOrderTracker*> all_, buyLegs_, sellLegs_;
    std::unordered_map<int64_t,int64_t> budget_;          // 主动单 -> 剩余预算
    int64_t displaced_ = 0;

    std::vector<dc::UnifiedRecord> buf_;                  // 同毫秒缓冲(仅 OPT/PESS)
    std::string bufTime_;
    bool bufActive_ = false;
    std::map<int64_t,int64_t> crossUsed_;                 // marketable 共享可见深度(按记录重置)
};

} // namespace vsim
