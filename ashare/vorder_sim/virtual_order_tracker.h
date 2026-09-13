#pragma once
// 虚拟订单追踪器 (B档, L3精确排队)。旁挂在 LobBuilder 事件流上, 只读、绝不改真实簿。
//
// 模型(见设计规格):
//   - 只为虚拟单所在的单一价位 P 维护"我侧"的 L3 FIFO 队列 rest_(按 seqNo 序)。
//   - aheadQty_ = 我前方剩余真实量。t0 插入时 = P档我侧现存全部量; 之后:
//       * 我侧 wt 到 P (t0后): 排我后面, 忽略。
//       * cl 到 P: 被撤单在我前(seqNo<insertSeq_)则 aheadQty_ -= 撤量, 否则忽略。
//       * zb 到 P: aheadQty_>0 先扣前方(consumeFront); 扣光后(=队首)后续成交量按"无冲击"归我。
//   - 集合竞价: 累积期只维护队列; 竞价终点 uncross, 按清算价 p* 撮合; 撮不掉的残量转连续继续挂。
//
// 支持场景:
//   1. t0 在连续段 → 全程 B 档精确(主用例)。
//   2. 挂到收盘集合竞价(14:57–15:00) → 收盘 uncross 收口。
//   3. t0 在开盘集合竞价 → 开盘 uncross 后转连续(排队位置保留: 插入时簿深 − uncross 边际
//      结清 − 前方撤单, 以转连续时簿深封顶; 竞价打印不重复消耗队列)。

#include "lob_builder.h"
#include "uncross.h"
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

namespace vsim {

// 时间窗(秒)
static constexpr double OPEN_AUC_START  = 33300.0;  // 09:15
static constexpr double OPEN_AUC_END    = 33900.0;  // 09:25
static constexpr double CONT_AM_START   = 34200.0;  // 09:30
static constexpr double CLOSE_AUC_START = 53820.0;  // 14:57
static constexpr double CLOSE_AUC_END   = 54000.0;  // 15:00

inline bool inOpenAuction (double s){ return s >= OPEN_AUC_START  && s < OPEN_AUC_END; }
inline bool inCloseAuction(double s){ return s >= CLOSE_AUC_START && s < CLOSE_AUC_END; }

enum class VState { PENDING_AUCTION, RESTING, PARTIAL, FILLED, CANCELLED };

// 同毫秒内"撤单 vs 成交"先后不确定 → 三种口径:
//   NEUTRAL     : 按数据自然(seqNo)顺序(单点估计)
//   OPTIMISTIC  : 同毫秒内先撤(前方让路)后成交 → 我成交最多最早(上界)
//   PESSIMISTIC : 同毫秒内先成交(前方先被吃)后撤 → 我成交最少最晚(下界)
enum class Mode { NEUTRAL, OPTIMISTIC, PESSIMISTIC };

struct Fill {
    double      timeSec;
    std::string timeStr;
    int64_t     qty;
    double      price;
    std::string venue;   // "auction" / "cont"
};

class VirtualOrderTracker {
public:
    // side: 'B'|'S'; priceYuan: 限价(元); size: 委托量; insertSec: t0(秒)
    // cancelSec: 策略主动撤单时刻(秒), 默认极大=永不撤。到达该时刻则终止:
    //            此前成交保留, 未成交部分撤销, 状态置 CANCELLED。
    VirtualOrderTracker(char side, double priceYuan, int64_t size, double insertSec,
                        Mode mode = Mode::NEUTRAL, double cancelSec = 1e18)
        : side_(side), P_(static_cast<int64_t>((priceYuan + 1e-6) * 100)),
          priceYuan_(priceYuan), size_(size), insertSec_(insertSec),
          mode_(mode), cancelSec_(cancelSec) {}

    // 由 SweepAllocator 逐条驱动(改簿后, 只读 book)。
    // 注意: actionType==2(成交) 在这里【只做队列维护, 不成交】——成交由 SweepAllocator
    // 从"造成该成交的主动单的剩余预算"里统一分配(见 sweep_allocator.h), 否则多腿会各自
    // 认领同一批流动性、凭空超发。
    void onEvent(const dc::UnifiedRecord& rec, const dc::LobBuilder& book) {
        double sec = rec.timeSeconds;

        // ── 策略主动撤单: 到达撤单时刻即终止(此前成交保留, 余量撤销) ──
        if (!isTerminal() && sec >= cancelSec_) {
            state_ = VState::CANCELLED;
            prevSec_ = sec; return;
        }

        // ── 尚未到 t0: 维护 P 档我侧现存队列(用于 t0 冻结 aheadQty_) ──
        if (!inserted_) {
            preInsertObserve(rec);
            if (sec >= insertSec_) doInsert(rec, book);
            prevSec_ = sec;
            return;
        }
        if (isTerminal()) { prevSec_ = sec; return; }

        // ── 开盘竞价终点: 跨过 09:25:00 触发 uncross(仅当我在开盘竞价里挂着) ──
        if (state_ == VState::PENDING_AUCTION && fromOpenAuction_ &&
            prevSec_ < OPEN_AUC_END && sec >= OPEN_AUC_END && !openUncrossed_) {
            auctionUncross(book, OPEN_AUC_END, "09:25:00.000");
            openUncrossed_ = true;
        }
        // ── 连续段挂着、进入收盘竞价: 切累积态 ──
        if ((state_==VState::RESTING||state_==VState::PARTIAL) && inCloseAuction(sec) && !enteredClose_) {
            enteredClose_ = true; fromCloseAuction_ = true;
            state_ = VState::PENDING_AUCTION;
        }
        // ── 收盘竞价终点 ──
        if (state_==VState::PENDING_AUCTION && fromCloseAuction_ &&
            prevSec_ < CLOSE_AUC_END && sec >= CLOSE_AUC_END && !closeUncrossed_) {
            auctionUncross(book, CLOSE_AUC_END, "15:00:00.000");
            closeUncrossed_ = true;
        }

        // ── 开盘 uncross 后, 进入连续段首个事件: 从真实簿重置 aheadQty_(退化为聚合) ──
        if (openUncrossed_ && !contResumed_ && sec >= CONT_AM_START &&
            (state_==VState::RESTING||state_==VState::PARTIAL)) {
            resetAheadFromBook(book);
            contResumed_ = true;
        }

        if (rec.actionType == 0) {            // 委托: 只在我这一档影响队列
            if (rec.intPrice == P_ && rec.direction == side_ && state_ == VState::PENDING_AUCTION)
                addRest(rec);   // 竞价期我也排队(后面的量); 连续期 t0 后到达排我后面 → 忽略
        } else if (rec.actionType == 1) {     // 撤单: 只在我这一档
            if (rec.intPrice == P_ && rec.direction == side_) onCancel(rec);
        }
        // actionType==2(成交): 不在此处理。队列消耗与成交分配由 SweepAllocator 统一做。
        prevSec_ = sec;
    }

    // ── 供 SweepAllocator 调用的接口(成交分配需要按主动单预算跨腿协调) ──
    bool    inAuctionPhase() const {          // 竞价累积期 / 开盘settle gap[09:25,09:30) / 收盘uncross后
        // closeUncrossed_: 收盘 uncross 已把 15:00 竞价里属于我的量分配完, 之后的竞价成交
        // 【打印】只是同一批流动性的磁带记录, 不得再走预算路径二次填充(双重计入)。
        // 开盘侧由 (openUncrossed_ && !contResumed_) 挡住 09:25 打印, 收盘侧靠这一项。
        return (state_ == VState::PENDING_AUCTION)
            || (openUncrossed_ && !contResumed_)
            || closeUncrossed_;
    }
    bool    preInsert()   const { return !inserted_; }
    bool    terminal()    const { return isTerminal(); }
    char    sideChar()    const { return side_; }
    int64_t levelPrice()  const { return P_; }
    double  insertSec()   const { return insertSec_; }
    int64_t insertSeqNo() const { return insertSeq_; }
    int64_t aheadQty()    const { return aheadQty_; }
    int64_t remaining()   const { return size_ - filled_; }
    bool    reachedBy(int64_t tradeIntPrice) const { return executableAt(tradeIntPrice); }
    bool    strictlyBetterThan(int64_t p) const { return strictlyBetter(p); }
    bool    oppositeActiveTrade(const dc::UnifiedRecord& r) const { return oppositeActive(r); }

    // marketable 跨价时: 与同一时刻的兄弟腿共享可见对手深度(避免各吃一遍同一个 D),
    // 并把吃掉的真实挂单量全额记入位移计。由 SweepAllocator 注入。
    void setCrossOverlay(std::map<int64_t,int64_t>* used, int64_t* displaced) {
        crossUsed_ = used; crossDisplaced_ = displaced;
    }

    // 前方真实量被主动单消耗 q(同时维护 rest_ 队列)
    void consumeAhead(int64_t q) {
        if (q <= 0) return;
        int64_t take = std::min(q, aheadQty_);
        aheadQty_ -= take;
        consumeFront(q);                     // rest_ 内部按队首消耗(自带 clamp)
    }
    // t0 前: 队列前方被消耗(此时还没 aheadQty_, 只维护 rest_)
    void consumeFrontPreInsert(int64_t q) { consumeFront(q); }

    // 从主动单剩余预算里成交。前置条件由 SweepAllocator 保证(已到队首 + 已被扫到 + 预算>0)。
    int64_t fillFromBudget(int64_t budget, const dc::UnifiedRecord& rec) {
        if (budget <= 0 || !inserted_ || isTerminal() || inAuctionPhase()) return 0;
        if (aheadQty_ > 0) return 0;
        bool through = (rec.intPrice != P_);
        int64_t f = fillMe(budget, rec, through ? "through" : "cont");
        if (through) throughFilled_ += f;
        return f;
    }

    // 收盘后收尾: 若还挂在收盘竞价里(数据未到15:00:00), 用当前簿做一次 uncross
    void finalize(const dc::LobBuilder& book) {
        if (isTerminal()) return;
        if (state_ == VState::PENDING_AUCTION && fromCloseAuction_ && !closeUncrossed_) {
            auctionUncross(book, CLOSE_AUC_END, "15:00:00.000");
            closeUncrossed_ = true;
        }
    }

    // ── 结果 ──
    VState state() const { return state_; }
    int64_t filled() const { return filled_; }
    int64_t size()   const { return size_; }
    int64_t aheadRemaining() const { return aheadQty_; }
    int64_t throughFilled() const { return throughFilled_; }  // 其中由扫价穿透贡献的成交量
    double arrivalBid() const { return arrivalBid_; }
    double arrivalAsk() const { return arrivalAsk_; }
    double arrivalMid() const {
        if (arrivalBid_ > 0 && arrivalAsk_ > 0) return (arrivalBid_ + arrivalAsk_) / 2.0;
        return (arrivalBid_ > 0) ? arrivalBid_ : arrivalAsk_;
    }
    const std::vector<Fill>& fills() const { return fills_; }
    double avgFillPrice() const {
        if (filled_ == 0) return 0.0;
        double amt = 0; for (auto& f : fills_) amt += f.price * f.qty;
        return amt / filled_;
    }
    static const char* stateName(VState s) {
        switch (s) {
            case VState::PENDING_AUCTION: return "PENDING_AUCTION";
            case VState::RESTING: return "RESTING";
            case VState::PARTIAL: return "PARTIAL";
            case VState::FILLED:  return "FILLED";
            case VState::CANCELLED: return "CANCELLED";
        }
        return "?";
    }

private:
    bool isTerminal() const { return state_==VState::FILLED || state_==VState::CANCELLED; }
    bool executableAt(int64_t p) const { return side_=='B' ? P_ >= p : P_ <= p; }
    bool strictlyBetter(int64_t p) const { return side_=='B' ? P_ > p : P_ < p; }
    // 成交主动方: SH 用 tradeDirection(1买/2卖); SZ 无该字段 → 大 ApplSeqNum(后到)为主动。
    static char activeSide(const dc::UnifiedRecord& rec) {
        if (rec.tradeDirection == 1) return 'B';
        if (rec.tradeDirection == 2) return 'S';
        return (rec.buyId > rec.sellId) ? 'B' : 'S';
    }
    // 该成交是否由"对手方主动"造成(能填充我): 我买单←卖方主动; 我卖单←买方主动。
    bool oppositeActive(const dc::UnifiedRecord& rec) const {
        char a = activeSide(rec);
        return side_ == 'B' ? (a == 'S') : (a == 'B');
    }

    // t0 前: 维护 P 档我侧全部现存挂单(成交造成的前方消耗由 SweepAllocator 驱动)
    void preInsertObserve(const dc::UnifiedRecord& rec) {
        if (rec.intPrice != P_) return;
        if (rec.actionType == 0) { if (rec.direction == side_) addRest(rec); }
        else if (rec.actionType == 1) { if (rec.direction == side_) removeRest(rec.sysid, rec.volume); }
    }

    // 我这一档在【真实簿】上的实际挂单量(权威)。
    int64_t bookDepthAtP(const dc::LobBuilder& book) const {
        if (side_ == 'B') { auto it = book.bestBid().find(-P_); return it!=book.bestBid().end() ? it->second : 0; }
        else              { auto it = book.bestAsk().find( P_); return it!=book.bestAsk().end() ? it->second : 0; }
    }

    void doInsert(const dc::UnifiedRecord& rec, const dc::LobBuilder& book) {
        inserted_ = true;
        insertSeq_ = rec.seqNo;   // 处理顺序里的当前序号: 比所有现存挂单晚 → 排队尾
        // 前方真实量以【真实簿】为准, 不能用 rest_(按原始委托记录堆的)求和:
        //   限价穿越对手盘的【主动单】, 其委托记录也挂在我这一档, 但它实际是立刻在对手价成交掉的,
        //   从未在本档挂住过(LobBuilder 把它放进 waitQueue, 不计入盘口档位)。
        //   若把它算作我前方 → 幻影队列; 而该档往往从无成交(如买价高于当日最高价) → 幻影【永远消不掉】,
        //   把这条腿的连续段成交整个封死(实测: 10.60 档 100 股幻影, 封死 160 万股残量一整天)。
        aheadQty_ = bookDepthAtP(book);
        // 记录到达时最优价(供滑点/MTM)
        if (!book.bestBid().empty()) arrivalBid_ = -book.bestBid().begin()->first / 100.0;
        if (!book.bestAsk().empty()) arrivalAsk_ =  book.bestAsk().begin()->first / 100.0;
        if (getenv("VSIM_DEBUG_AUC"))
            fprintf(stderr, "[INS] side=%c P=%lld insertSec=%.3f recT=%s recSec=%.3f openAuc=%d ahead=%lld\n",
                    side_, (long long)P_, insertSec_, rec.time.c_str(), rec.timeSeconds,
                    (int)inOpenAuction(insertSec_), (long long)aheadQty_);
        if (inOpenAuction(insertSec_))  { state_=VState::PENDING_AUCTION; fromOpenAuction_=true; }
        else if (inCloseAuction(insertSec_)){ state_=VState::PENDING_AUCTION; fromCloseAuction_=true; enteredClose_=true; }
        else {
            state_ = VState::RESTING;
            aggressiveCrossAtArrival(book, rec);   // 到达即可成交(marketable)则先按对手价扫单
        }
    }

    // 到达即可成交(marketable): 限价穿越对手最优 → 立即按对手价位从触及价往外扫单成交
    // (成交价=对手价, 非我限价), 残量在 P 挂着继续被动追踪。只读盘口, 不改真实簿。
    //
    // 此处我【自己就是主动方】, 吃的是对手盘上真实挂着的量 —— 这些量在现实里是被别人吃掉的,
    // 所以 marketable 成交【全额计入位移】(不能报"零冲击")。
    // 同一时刻多笔虚拟单同时跨价时, 必须【共享同一批可见深度】(crossUsed_ 记录已被兄弟腿吃掉的量),
    // 否则每条腿各自吃一遍同一个 D = 对同一个量的重复计数。
    void aggressiveCrossAtArrival(const dc::LobBuilder& book, const dc::UnifiedRecord& rec) {
        auto avail = [&](int64_t ip, int64_t depth) -> int64_t {
            if (!crossUsed_) return depth;
            auto it = crossUsed_->find(ip);
            return (it == crossUsed_->end()) ? depth : std::max<int64_t>(0, depth - it->second);
        };
        auto take = [&](int64_t ip, int64_t q) {
            if (crossUsed_) (*crossUsed_)[ip] += q;
            if (crossDisplaced_) *crossDisplaced_ += q;   // 吃的是真实挂单 → 全额位移
        };
        if (side_ == 'B') {
            for (const auto& kv : book.bestAsk()) {          // 升序: 卖低价在前
                if (filled_ >= size_) break;
                int64_t ap = kv.first;
                if (ap > P_) break;                          // 超过我买价, 停
                int64_t fq = std::min<int64_t>(avail(ap, kv.second), size_ - filled_);
                if (fq <= 0) continue;                       // 该档已被兄弟腿吃光
                filled_ += fq; take(ap, fq);
                fills_.push_back({rec.timeSeconds, rec.time, fq, ap/100.0, "aggress"});
            }
        } else {
            for (const auto& kv : book.bestBid()) {          // key=-intPrice 升序: 买高价在前
                if (filled_ >= size_) break;
                int64_t bp = -kv.first;
                if (bp < P_) break;                          // 低于我卖价, 停
                int64_t fq = std::min<int64_t>(avail(-kv.first, kv.second), size_ - filled_);
                if (fq <= 0) continue;
                filled_ += fq; take(-kv.first, fq);
                fills_.push_back({rec.timeSeconds, rec.time, fq, bp/100.0, "aggress"});
            }
        }
        if (filled_ >= size_)   state_ = VState::FILLED;
        else if (filled_ > 0)   state_ = VState::PARTIAL;
        // 残量在 P 挂着(通常成为最优价, aheadQty_ 已为该档现存量; marketable 时该档多为空 → 0)
    }

    // ── 队列维护 ──
    void addRest(const dc::UnifiedRecord& rec) {
        rest_[rec.seqNo] = { rec.sysid, rec.volume };
        sysid2seq_[rec.sysid] = rec.seqNo;
    }
    void removeRest(int64_t sysid, int64_t qty) {
        auto it = sysid2seq_.find(sysid);
        if (it == sysid2seq_.end()) return;
        int64_t seq = it->second;
        auto rit = rest_.find(seq);
        if (rit != rest_.end()) {
            rit->second.second -= qty;
            if (rit->second.second <= 0) rest_.erase(rit);
        }
        sysid2seq_.erase(it);
    }
    // 从队首消耗 qty(可跨多笔前方单)
    void consumeFront(int64_t qty) {
        while (qty > 0 && !rest_.empty()) {
            auto it = rest_.begin();
            int64_t take = std::min(qty, it->second.second);
            it->second.second -= take; qty -= take;
            if (it->second.second <= 0) { sysid2seq_.erase(it->second.first); rest_.erase(it); }
        }
    }

    // 连续段: 撤单命中 P
    void onCancel(const dc::UnifiedRecord& rec) {
        auto it = sysid2seq_.find(rec.sysid);
        if (it == sysid2seq_.end()) return;          // 不在我前方(t0后到达/已消耗) → 忽略
        if (it->second < insertSeq_) aheadQty_ -= rec.volume;   // 在我前 → 前移
        if (aheadQty_ < 0) aheadQty_ = 0;
        removeRest(rec.sysid, rec.volume);
    }

    int64_t fillMe(int64_t avail, const dc::UnifiedRecord& rec, const char* venue) {
        int64_t fq = std::min(avail, size_ - filled_);
        if (fq > 0) {
            filled_ += fq;
            fills_.push_back({rec.timeSeconds, rec.time, fq, priceYuan_, venue});
            state_ = (filled_ == size_) ? VState::FILLED : VState::PARTIAL;
        }
        return fq;
        // avail 超出我剩余 size 的部分 = 吃我后面的真实单, 与我无关
    }

    // 集合竞价 uncross 撮合
    void auctionUncross(const dc::LobBuilder& book, double endSec, const std::string& endTime) {
        UncrossResult u = uncross(book.bestBid(), book.bestAsk());
        if (getenv("VSIM_DEBUG_AUC")) {
            int64_t better  = u.has ? sideVolStrictlyBetter(side_, u.clearIntPrice, book.bestBid(), book.bestAsk()) : -1;
            int64_t oppExec = u.has ? oppositeExecutableVol(side_, u.clearIntPrice, book.bestBid(), book.bestAsk()) : -1;
            fprintf(stderr, "[AUC] side=%c P=%lld t=%s has=%d p*=%lld maxVol=%lld better=%lld oppExec=%lld ahead=%lld filled=%lld\n",
                    side_, (long long)P_, endTime.c_str(), (int)u.has, (long long)u.clearIntPrice,
                    (long long)u.maxVol, (long long)better, (long long)oppExec,
                    (long long)aheadQty_, (long long)filled_);
        }
        if (u.has && executableAt(u.clearIntPrice)) {
            int64_t pstar = u.clearIntPrice;
            int64_t better  = sideVolStrictlyBetter(side_, pstar, book.bestBid(), book.bestAsk());
            int64_t oppExec = oppositeExecutableVol(side_, pstar, book.bestBid(), book.bestAsk());
            int64_t avail   = std::max<int64_t>(0, oppExec - better);  // 对手量扣掉我方严格更优真实同伴
            int64_t marginalCleared = std::max<int64_t>(0, u.maxVol - better);  // p*档真实边际清算量
            int64_t fq;
            if (strictlyBetter(pstar)) {
                // 严格优于 p* → 全成, 但不得超过可用对手量(委托量>对手深度时封顶)
                fq = std::min<int64_t>(size_ - filled_, avail);
                aheadQty_ = 0;   // 我价严格优于 p* → 我这一档的真实单在竞价里全部撮清
            } else {
                // P == p* 边际档: 我能拿 = 对手可成交量 − 排我前面的所有人(better + 同档前方)。
                // 不能拿「marginalCleared − aheadQty_」封顶: 对手方在 p* 有过剩时
                // (maxVol 受我方需求约束), 过剩部分本没人要, 我吃它不挤走任何人 ——
                // 旧公式在这种簿上把成交错判成 0(低估, 实测 case9)。
                fq = std::min<int64_t>(size_ - filled_,
                                       std::max<int64_t>(0, avail - aheadQty_));
                aheadQty_ -= std::min(aheadQty_, marginalCleared);  // 前方真实边际单被撮掉的部分
            }
            if (fq > 0) {
                filled_ += fq;
                fills_.push_back({endSec, endTime, fq, pstar / 100.0, "auction"});
                // 位移: 对手过剩量(oppExec − maxVol)以内是"没人要的量", 吃它零冲击;
                // 超出部分 = 从排我后面的真实同侧单嘴里挤走的量, 计入位移计。
                if (crossDisplaced_) {
                    int64_t surplus = std::max<int64_t>(0, oppExec - u.maxVol);
                    *crossDisplaced_ += std::max<int64_t>(0, fq - surplus);
                }
            }
        }
        // 与连续段 fillMe 同口径: 有成交但未全成 = PARTIAL(旧代码一律 RESTING, 状态口径不一致)
        state_ = (filled_ == size_) ? VState::FILLED
               : (filled_ > 0)      ? VState::PARTIAL : VState::RESTING;
    }

    // 开盘竞价后转连续: 校准 aheadQty_。取【自身追踪值】与【簿上该档深度】的较小者:
    //   - 同价位 FIFO, 09:25 之后到达的单只能排我后面 → 簿深(含后到者)只是上界, 不是我的前方量。
    //     旧版直接重置成整档深度, 会把 settle gap 内新到的同侧单全算到我前面
    //     (实测 000566: 真实前方 ~9.5M 被重置成 21.7M, 成交时点被错误推迟)。
    //   - 自身追踪值(插入时簿深 − uncross 边际结清 − 前方撤单)不应超过簿深; 若超过说明
    //     追踪遗漏(理论上不发生), 用簿深封顶兜底。
    void resetAheadFromBook(const dc::LobBuilder& book) {
        int64_t v = std::min(bookDepthAtP(book), aheadQty_);
        if (getenv("VSIM_DEBUG_AUC"))
            fprintf(stderr, "[RESET] side=%c P=%lld ahead %lld -> %lld filled=%lld\n",
                    side_, (long long)P_, (long long)aheadQty_, (long long)v, (long long)filled_);
        aheadQty_ = v;
        rest_.clear(); sysid2seq_.clear();
        if (v > 0) { rest_[-1] = { -1, v }; }   // 聚合哨兵节点(后续 cancel 无法按号定位)
    }

    // 配置
    char    side_;
    int64_t P_;
    double  priceYuan_;
    int64_t size_;
    double  insertSec_;

    // 状态
    VState  state_ = VState::RESTING;
    bool    inserted_ = false;
    int64_t insertSeq_ = 0;
    int64_t filled_ = 0;
    int64_t aheadQty_ = 0;
    std::map<int64_t, std::pair<int64_t,int64_t>> rest_;  // seqNo -> {sysid, restQty}
    std::unordered_map<int64_t,int64_t> sysid2seq_;
    std::vector<Fill> fills_;
    double  prevSec_ = -1;

    bool fromOpenAuction_ = false, fromCloseAuction_ = false;
    bool openUncrossed_ = false, closeUncrossed_ = false;
    bool enteredClose_ = false, contResumed_ = false;
    double arrivalBid_ = 0.0, arrivalAsk_ = 0.0;
    int64_t throughFilled_ = 0;

    Mode mode_ = Mode::NEUTRAL;            // 同毫秒缓冲/重排已上移到 SweepAllocator(全篮子共用一个重排)
    double cancelSec_ = 1e18;              // 策略主动撤单时刻

    // marketable 跨价共享层(由 SweepAllocator 注入; 单腿直跑时为 null → 退化为独占可见深度)
    std::map<int64_t,int64_t>* crossUsed_ = nullptr;   // intPrice -> 已被同时刻兄弟腿吃掉的量
    int64_t* crossDisplaced_ = nullptr;                // 累加到分配器的位移计
};

} // namespace vsim
