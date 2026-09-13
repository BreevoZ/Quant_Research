// vorder_sim 合成场景测试: 手工构造事件流(已知预期), 逐个断言, 覆盖边界/corner case 找bug。
// 用 SZ 模式(isSH=false): 按 seqNo 排序、无 SH resequence、无派生单, 便于精确控制处理顺序。

#include "lob_builder.h"
#include "virtual_order_tracker.h"
#include "sweep_allocator.h"
#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <memory>
#include <cmath>

using dc::UnifiedRecord;
using namespace vsim;

static std::string secToStr(double s) {
    int h=(int)s/3600, m=((int)s%3600)/60, sec=(int)s%60;
    char b[24]; snprintf(b,sizeof(b),"%02d:%02d:%02d.000",h,m,sec); return b;
}

// 构造器
static UnifiedRecord ord(int64_t seq, double sec, int64_t sysid, char dir, double px, int64_t qty){
    UnifiedRecord r; r.seqNo=seq; r.timeSeconds=sec; r.time=secToStr(sec); r.actionType=0;
    r.direction=dir; r.sysid=sysid; r.price=px; r.intPrice=(int64_t)((px+1e-6)*100); r.volume=qty;
    r.priceType='2'; return r;
}
static UnifiedRecord ccl(int64_t seq, double sec, int64_t sysid, char dir, double px, int64_t qty){
    UnifiedRecord r; r.seqNo=seq; r.timeSeconds=sec; r.time=secToStr(sec); r.actionType=1;
    r.direction=dir; r.sysid=sysid; r.price=px; r.intPrice=(int64_t)((px+1e-6)*100); r.volume=qty;
    r.priceType='2'; return r;
}
static UnifiedRecord trd(int64_t seq, double sec, int64_t buyId, int64_t sellId, double px, int64_t qty, int td){
    UnifiedRecord r; r.seqNo=seq; r.timeSeconds=sec; r.time=secToStr(sec); r.actionType=2;
    r.buyId=buyId; r.sellId=sellId; r.price=px; r.intPrice=(int64_t)((px+1e-6)*100); r.volume=qty;
    r.tradeDirection=td; r.priceType='2'; return r;
}

static int g_pass=0, g_fail=0;

struct Expect { int64_t filled; VState state; double avgPx; int64_t aheadRem; bool checkAhead; bool checkAvg; };

static void run(const std::string& name, std::vector<UnifiedRecord> recs,
                char side, double px, int64_t qty, double t0, Expect e, bool isSH=false) {
    VirtualOrderTracker tk(side, px, qty, t0);
    auto orderQ = computeOrderBudget(recs);
    SweepAllocator al(Mode::NEUTRAL, &orderQ);
    al.addLeg(&tk);
    dc::LobBuilder b("TEST","20260707",isSH);
    b.load(std::move(recs));
    b.setEventHook([&](const UnifiedRecord& r, const dc::LobBuilder& bb){ al.onEvent(r,bb); });
    b.build();
    al.finalize(b);

    bool ok = (tk.filled()==e.filled) && (tk.state()==e.state);
    if (e.checkAhead) ok = ok && (tk.aheadRemaining()==e.aheadRem);
    if (e.checkAvg && tk.filled()>0) ok = ok && (std::abs(tk.avgFillPrice()-e.avgPx)<1e-6);

    if (ok) { g_pass++; std::cout << "  ✅ " << name << "\n"; }
    else {
        g_fail++;
        std::cout << "  ❌ " << name << "\n"
                  << "      期望: filled="<<e.filled<<" state="<<VirtualOrderTracker::stateName(e.state);
        if(e.checkAhead) std::cout<<" ahead="<<e.aheadRem;
        if(e.checkAvg) std::cout<<" avgPx="<<e.avgPx;
        std::cout << "\n      实际: filled="<<tk.filled()<<" state="<<VirtualOrderTracker::stateName(tk.state())
                  <<" ahead="<<tk.aheadRemaining();
        if(tk.filled()>0) std::cout<<" avgPx="<<tk.avgFillPrice();
        std::cout << "\n";
    }
}

// 便捷: 连续段场景公用 t0=36000(10:00:00), 用远价 dummy 单在 t0 触发插入
static UnifiedRecord dummy(int64_t seq, double sec){ return ord(seq,sec,90000+seq,'S',999.0,1); }

// ── 多虚拟单(共享主动单预算): 同价串行 / 异价耦合 / 位移计 ──
struct MLeg { char side; double px; int64_t qty; double t0; };
static void runMulti(const std::string& name, std::vector<UnifiedRecord> recs,
                     std::vector<MLeg> legs, std::vector<int64_t> expFilled, int64_t expDisplaced) {
    auto orderQ = computeOrderBudget(recs);
    SweepAllocator al(Mode::NEUTRAL, &orderQ);
    std::vector<std::unique_ptr<VirtualOrderTracker>> tks;
    for (const auto& L : legs) {
        tks.push_back(std::make_unique<VirtualOrderTracker>(L.side, L.px, L.qty, L.t0));
        al.addLeg(tks.back().get());
    }
    dc::LobBuilder b("M","20260707",false);
    b.load(std::move(recs));
    b.setEventHook([&](const UnifiedRecord& r, const dc::LobBuilder& bb){ al.onEvent(r,bb); });
    b.build();
    al.finalize(b);

    bool ok = (al.displaced() == expDisplaced);
    for (size_t i = 0; i < tks.size(); ++i) if (tks[i]->filled() != expFilled[i]) ok = false;
    if (ok) { g_pass++; std::cout << "  ✅ " << name << "\n"; }
    else {
        g_fail++;
        std::cout << "  ❌ " << name << "\n      期望: filled=[";
        for (size_t i=0;i<expFilled.size();++i) std::cout << (i?",":"") << expFilled[i];
        std::cout << "] displaced=" << expDisplaced << "\n      实际: filled=[";
        for (size_t i=0;i<tks.size();++i) std::cout << (i?",":"") << tks[i]->filled();
        std::cout << "] displaced=" << al.displaced() << "\n";
    }
}

// ── 随机化 fuzz: 生成随机场景, 检查必然成立的不变量(比死写预期更能揪 bug) ──
struct FuzzRes { int64_t filled, through; VState state; double avg; bool ok; };
static FuzzRes runOne(const std::vector<UnifiedRecord>& recs, char side,double px,
                      int64_t qty,double t0,double cancelSec,Mode m){
    VirtualOrderTracker tk(side,px,qty,t0,m,cancelSec);
    auto orderQ = computeOrderBudget(recs);
    SweepAllocator al(m, &orderQ);
    al.addLeg(&tk);
    dc::LobBuilder b("F","20260707",false);
    b.load(std::vector<UnifiedRecord>(recs));
    b.setEventHook([&](const UnifiedRecord&r,const dc::LobBuilder&bb){al.onEvent(r,bb);});
    b.build(); al.finalize(b);
    return { tk.filled(), tk.throughFilled(), tk.state(),
             tk.filled()>0?tk.avgFillPrice():0, true };
}

static void runFuzz(int nCases){
    std::mt19937 rng(12345);
    std::cout << "\n===== 随机 fuzz " << nCases << " 例 (检查不变量) =====\n";
    int localFail=0;
    int covFill=0, covFull=0, covCancel=0, covBand=0, covThrough=0, covAuc=0;
    for(int cs=0; cs<nCases; ++cs){
        // 随机场景参数
        std::uniform_int_distribution<int> flavor(0,3);   // 0连续被动 1连续marketable 2竞价 3带撤单
        int fl = flavor(rng);
        bool auction = (fl==2);
        double t0 = auction ? 33400.0 : 36000.0;
        int basP = 10000;                                  // 100.00 元 (intPrice)
        std::uniform_int_distribution<int> offd(-8,8);
        int Poff = offd(rng);
        double px = (basP + Poff) / 100.0;
        char side = (rng()&1)?'B':'S';
        std::uniform_int_distribution<int> qd(1,300);
        int64_t qty = qd(rng)*100;                         // 100..30000 (大单更多留在部分成交态)
        double cancelSec = 1e18;
        if(fl==3){ std::uniform_int_distribution<int> cd(2,60); cancelSec = t0 + cd(rng); }

        std::vector<UnifiedRecord> recs;
        int64_t seq=1;
        int oppActTd = (side=='B')?2:1;   // 能填我的主动方向: 我买←卖主动(2), 我卖←买主动(1)
        std::uniform_int_distribution<int> qd2(1,30), gap(0,2), typ(0,9);

        // ── t0 前: 造 K 笔"我前方"真实单(明确在我之前), 建 aheadQty ──
        std::vector<int64_t> aheadIds;
        std::uniform_int_distribution<int> kd(2,8), aqd(1,6);
        int K = auction?0:kd(rng);
        double sec = auction ? 33350.0 : (t0-20);
        auto nid = [&]() -> int64_t { return seq++; };   // 取一个新序号(同时用作订单号), 避免同表达式里 seq++ 与 seq 混用
        for(int k=0;k<K;++k){ int64_t i=nid(); recs.push_back(ord(i,sec,i,side,px,aqd(rng)*100)); aheadIds.push_back(i); }
        // t0 触发插入(远价, 不碰我档)
        sec = t0; { int64_t i=nid(); recs.push_back(ord(i,sec,i,(side=='B'?'S':'B'),999.0,1)); }

        std::uniform_int_distribution<int> nd(30,140);
        int N = nd(rng);
        for(int e=0;e<N;++e){
            if(auction && sec>=33895) sec = 33355 + (e%35);
            int r10 = typ(rng);
            // 30% 概率造"同秒突发": 撤一笔前方单 + 一笔能填我的成交 (专压上下界)
            if(!auction && !aheadIds.empty() && r10<3){
                int64_t aid = aheadIds[rng()%aheadIds.size()];
                recs.push_back(ccl(nid(),sec,aid,side,px,aqd(rng)*100));       // 撤前方(同秒)
                recs.push_back(trd(nid(),sec,0,0,px,qd2(rng)*100,oppActTd));   // 成交(同秒, 能填我)
                sec += gap(rng);
                continue;
            }
            sec += gap(rng);
            int pxo = std::uniform_int_distribution<int>(-10,10)(rng);
            double eprc=(basP+pxo)/100.0; int64_t evol=qd2(rng)*100;
            if(r10<5){ char d=(rng()&1)?'B':'S'; int64_t i=nid(); recs.push_back(ord(i,sec,i,d,eprc,evol)); }
            else if(r10<7){
                if(!aheadIds.empty()&&(rng()&1)) recs.push_back(ccl(nid(),sec,aheadIds[rng()%aheadIds.size()],side,px,evol));
                else { int64_t i=nid(); recs.push_back(ccl(i,sec,i,(rng()&1)?'B':'S',eprc,evol)); }
            } else { int td=(rng()&1)?1:2; recs.push_back(trd(nid(),sec,0,0,eprc,evol,td)); }
        }
        // 确保撤单场景: 事件流延伸到 cancelSec 之后(否则触发不了)
        if(fl==3){ int64_t i=nid(); recs.push_back(ord(i, cancelSec+1, i, (side=='B'?'S':'B'), 999.0, 1)); }
        // 竞价场景: 补几条对手盘保证 uncross 有两侧 + 一条跨09:25触发
        if(auction){
            int64_t a=nid(); recs.push_back(ord(a,33360,a,'B',(basP+Poff)/100.0,500));
            int64_t b=nid(); recs.push_back(ord(b,33370,b,'S',(basP+Poff)/100.0,500));
            int64_t c=nid(); recs.push_back(ord(c,33901,c,'S',999.0,1));   // 跨 09:25 触发 uncross
            int64_t d=nid(); recs.push_back(ord(d,34300,d,'S',999.0,1));   // 进连续
        }

        FuzzRes n=runOne(recs,side,px,qty,t0,cancelSec,Mode::NEUTRAL);
        FuzzRes o=runOne(recs,side,px,qty,t0,cancelSec,Mode::OPTIMISTIC);
        FuzzRes p=runOne(recs,side,px,qty,t0,cancelSec,Mode::PESSIMISTIC);

        // ── 不变量 ──
        std::vector<std::string> viol;
        auto rng01=[&](int64_t f){ return f>=0 && f<=qty; };
        if(!rng01(n.filled)||!rng01(o.filled)||!rng01(p.filled)) viol.push_back("filled越界[0,qty]");
        if(n.through > n.filled) viol.push_back("through>filled");
        if(!(p.filled <= n.filled && n.filled <= o.filled)) viol.push_back("上下界未括住 pess<=neu<=opt");
        // 成交价: 买单不高于限价, 卖单不低于限价
        double eps=1e-6;
        for(auto* r : {&n,&o,&p}) if(r->filled>0){
            if(side=='B' && r->avg > px+eps) viol.push_back("买成交价高于限价");
            if(side=='S' && r->avg < px-eps) viol.push_back("卖成交价低于限价");
        }
        // 状态一致性
        if(n.state==VState::FILLED && n.filled!=qty) viol.push_back("FILLED但filled!=qty");
        if(n.filled==qty && n.state!=VState::FILLED && n.state!=VState::CANCELLED) viol.push_back("filled==qty但非FILLED");
        if(n.state==VState::CANCELLED && n.filled>=qty) viol.push_back("CANCELLED但已全成");
        if(cancelSec>1e17 && n.state==VState::CANCELLED) viol.push_back("未设撤单却CANCELLED");

        // 覆盖率统计
        if(n.filled>0) covFill++;
        if(n.filled==qty) covFull++;
        if(n.state==VState::CANCELLED) covCancel++;
        if(p.filled<o.filled) covBand++;
        if(n.through>0) covThrough++;
        if(auction) covAuc++;

        if(!viol.empty()){
            localFail++; g_fail++;
            std::cout<<"  ❌ case#"<<cs<<" flavor="<<fl<<" side="<<side<<" px="<<px<<" qty="<<qty
                     <<" | pess="<<p.filled<<" neu="<<n.filled<<" opt="<<o.filled<<" through="<<n.through
                     <<" state="<<VirtualOrderTracker::stateName(n.state)<<" avg="<<n.avg<<"\n";
            for(auto&v:viol) std::cout<<"        违反: "<<v<<"\n";
        } else g_pass++;
    }
    std::cout<<"  fuzz 完成: "<<(nCases-localFail)<<"/"<<nCases<<" 通过\n";
    std::cout<<"  覆盖: 有成交="<<covFill<<" 全成="<<covFull<<" 撤单="<<covCancel
             <<" 上下界展开(pess<opt)="<<covBand<<" 有穿透="<<covThrough
             <<" 竞价="<<covAuc<<" (共"<<nCases<<"例)\n";

    // ── 覆盖率断言(关键!) ──
    // 上面那些不变量是【必要不充分】的: 一个永远返回 filled=0 的实现能全部通过。
    // 实测教训: 引入 SweepAllocator 时因主动单识别失败, 成交从 437 例塌到 74 例,
    //           500 例不变量【依然全过】—— 只有覆盖率暴露了它。故必须把覆盖率也断言死。
    struct Floor { const char* name; int got, min; };
    Floor floors[] = {
        {"有成交",      covFill,    nCases*3/5},   // 至少 60%
        {"全成",        covFull,    nCases*3/10},  // 至少 30%
        {"撤单",        covCancel,  nCases/20},
        {"上下界展开",  covBand,    nCases/50},
        {"有穿透",      covThrough, nCases/5},
        {"竞价",        covAuc,     nCases/10},
    };
    for (auto& f : floors) {
        if (f.got < f.min) { g_fail++;
            std::cout<<"  ❌ 覆盖率塌陷: "<<f.name<<"="<<f.got<<" < 下限"<<f.min
                     <<" (不变量全过也不代表对; 多半是某条成交路径被整个封死)\n"; }
        else g_pass++;
    }
}

// ── 多虚拟单 fuzz: 带【真实订单号】的主动单 → 真正走共享预算路径(单腿 fuzz 永远走不到) ──
// 检查【无需 ground truth 的强不变量】:
//   1. 守恒: 同一笔主动单造成的虚拟成交总和 ≤ 它的原始委托量 Q   ← 独立模型必然违反
//   2. 单调: 多腿同跑时每条腿的成交 ≤ 它单独跑时的成交(加兄弟腿只会抢走, 不会送给你)
//   3. 位移: 0 ≤ displaced ≤ Σfilled
static void runMultiFuzz(int nCases){
    std::mt19937 rng(777);
    std::cout << "\n===== 多虚拟单 fuzz " << nCases << " 例 (共享预算强不变量) =====\n";
    int fail=0, covShrink=0, covDisp=0, covSameLvl=0;
    for(int cs=0; cs<nCases; ++cs){
        const double T0=36000.0;
        // 真实买盘三档(带订单号), 供主动卖单逐档吃
        std::uniform_int_distribution<int> dq(2,20), dn(2,4), dsz(1,40), dQ(1,60);
        int64_t rid[3]; int64_t rvol[3]; int64_t rip[3]={10000,9999,9998};
        std::vector<UnifiedRecord> recs;
        int64_t seq=1;
        for(int i=0;i<3;++i){ rid[i]=seq; rvol[i]=dq(rng)*100;
            recs.push_back(ord(seq,T0-30,seq,'B',rip[i]/100.0,rvol[i])); ++seq; }
        recs.push_back(ord(seq,T0,seq,'S',999.0,1)); ++seq;              // t0 触发插入

        // 2~4 条虚拟买腿, 随机落在这三档(可同档)
        int nLeg = dn(rng);
        std::vector<MLeg> legs;
        for(int i=0;i<nLeg;++i){
            int lv = rng()%3;
            legs.push_back({'B', rip[lv]/100.0, (int64_t)dsz(rng)*100, T0});
        }
        // 一笔主动卖单 A, 原始委托量 Q(可能大于/小于真实买盘总深度 → 覆盖 slack 与无 slack)
        int64_t A=seq, Q=(int64_t)dQ(rng)*100;
        recs.push_back(ord(A,T0+5,A,'S',rip[2]/100.0,Q)); ++seq;
        int64_t left=Q;
        for(int i=0;i<3 && left>0;++i){                                   // 逐档吃真实买盘
            int64_t v=std::min(left, rvol[i]); if(v<=0) continue;
            recs.push_back(trd(seq++, T0+5, rid[i], A, rip[i]/100.0, v, 2));   // 卖方主动
            left-=v;
        }
        recs.push_back(ord(seq,T0+60,seq,'S',999.0,1));                   // 收尾

        auto runSet=[&](const std::vector<MLeg>& L, std::vector<int64_t>& out)->int64_t{
            auto q=computeOrderBudget(recs);
            SweepAllocator al(Mode::NEUTRAL,&q);
            std::vector<std::unique_ptr<VirtualOrderTracker>> tks;
            for(auto&g:L){ tks.push_back(std::make_unique<VirtualOrderTracker>(g.side,g.px,g.qty,g.t0));
                           al.addLeg(tks.back().get()); }
            dc::LobBuilder b("MF","20260707",false);
            b.load(std::vector<UnifiedRecord>(recs));
            b.setEventHook([&](const UnifiedRecord&r,const dc::LobBuilder&bb){ al.onEvent(r,bb); });
            b.build(); al.finalize(b);
            out.clear(); for(auto&t:tks) out.push_back(t->filled());
            return al.displaced();
        };
        std::vector<int64_t> multi; int64_t disp = runSet(legs, multi);
        int64_t sumMulti=0; for(auto v:multi) sumMulti+=v;

        std::vector<std::string> viol;
        // ① 守恒: 所有虚拟成交都出自这一笔主动单的预算 Q
        if(sumMulti > Q) viol.push_back("Σfilled="+std::to_string(sumMulti)+" > 主动单 Q="+std::to_string(Q));
        // ③ 位移
        if(disp < 0)         viol.push_back("displaced < 0");
        if(disp > sumMulti)  viol.push_back("displaced="+std::to_string(disp)+" > Σfilled="+std::to_string(sumMulti));
        // ② 单调: 每条腿单独跑的成交 ≥ 它在多腿里的成交
        for(size_t i=0;i<legs.size();++i){
            std::vector<int64_t> solo; runSet({legs[i]}, solo);
            if(multi[i] > solo[0])
                viol.push_back("腿"+std::to_string(i)+" 多腿成交"+std::to_string(multi[i])
                               +" > 单独成交"+std::to_string(solo[0])+"(加兄弟腿反而多成交了)");
            if(multi[i] < solo[0]) covShrink++;
        }
        if(disp>0) covDisp++;
        for(size_t i=0;i+1<legs.size();++i)
            for(size_t j=i+1;j<legs.size();++j)
                if(legs[i].px==legs[j].px){ covSameLvl++; goto done; }
        done:;
        if(!viol.empty()){ ++fail; g_fail++;
            std::cout<<"  ❌ mcase#"<<cs<<" Q="<<Q<<" 腿数="<<nLeg<<" Σfilled="<<sumMulti<<" disp="<<disp<<"\n";
            for(auto&v:viol) std::cout<<"        违反: "<<v<<"\n";
        } else g_pass++;
    }
    std::cout<<"  多腿 fuzz 完成: "<<(nCases-fail)<<"/"<<nCases<<" 通过\n";
    std::cout<<"  覆盖: 被兄弟腿抢占(成交变少)="<<covShrink<<" 次  有位移="<<covDisp
             <<" 例  含同价多腿="<<covSameLvl<<" 例\n";
    if(covShrink < nCases/5){ g_fail++;
        std::cout<<"  ❌ 覆盖率塌陷: 几乎没有腿被兄弟腿抢占 → 共享预算可能没生效!\n"; }
    else g_pass++;
}

int main(){
    const double T0=36000.0;   // 10:00:00 连续段
    // 便捷 Expect
    auto E=[&](int64_t f,VState s){ return Expect{f,s,0,0,false,false}; };
    auto EA=[&](int64_t f,VState s,int64_t ahead){ return Expect{f,s,0,ahead,true,false}; };
    auto EV=[&](int64_t f,VState s,double avg){ return Expect{f,s,avg,0,false,true}; };

    std::cout << "===== 连续段: 被动买单 =====\n";

    // S1 队首直接成交(无前方), 分两笔吃满
    run("S1 队首两笔吃满", {
        dummy(1,T0),
        trd(2,T0+1, 0,0, 10.00, 400, 2),
        trd(3,T0+2, 0,0, 10.00, 600, 2),
    }, 'B',10.00,1000,T0, EV(1000,VState::FILLED,10.00));

    // S2 前方600先被吃, 再轮到我
    run("S2 前方消耗后成交", {
        ord(1,T0-2,101,'B',10.00,500),   // 前方 aheadQty=500
        dummy(2,T0),
        trd(3,T0+1, 0,0,10.00,300,2),    // 扣前方 500->200
        trd(4,T0+2, 0,0,10.00,400,2),    // 扣前方200 + 成交我200
    }, 'B',10.00,1000,T0, EA(200,VState::PARTIAL,0));

    // S3 撤前方单 → 我前移
    run("S3 撤前方单前移", {
        ord(1,T0-3,101,'B',10.00,500),
        ord(2,T0-2,102,'B',10.00,300),   // aheadQty=800
        dummy(3,T0),
        ccl(4,T0+1,101,'B',10.00,500),   // 撤前方101 → ahead 800->300
        trd(5,T0+2, 0,0,10.00,400,2),    // 扣300 + 成交我100
    }, 'B',10.00,1000,T0, E(100,VState::PARTIAL));

    // S4 撤我后面的单 → 不影响
    run("S4 撤后方单不影响", {
        ord(1,T0-2,101,'B',10.00,500),   // ahead=500
        dummy(2,T0),
        ord(3,T0+1,201,'B',10.00,300),   // 我后面(t0后)
        ccl(4,T0+2,201,'B',10.00,300),   // 撤后方 → 应忽略
        trd(5,T0+3, 0,0,10.00,600,2),    // 扣前方500 + 成交我100
    }, 'B',10.00,1000,T0, E(100,VState::PARTIAL));

    // S5 扫价穿透(买): 卖吃穿到 P 以下, 我应吃到穿透量
    run("S5 扫价穿透-买", {
        ord(1,T0-2,101,'B',10.00,500),   // ahead=500
        dummy(2,T0),
        trd(3,T0+1, 0,0,10.00,500,2),    // 扣前方 500->0, 我未成交
        trd(4,T0+2, 0,0, 9.99,1500,2),   // 吃穿到9.99: 我应成交1500
    }, 'B',10.00,3000,T0, EV(1500,VState::PARTIAL,10.00));

    // S6 无穿透对照: 若无 P 以下成交, 我只吃 P 上的
    run("S6 无穿透对照", {
        ord(1,T0-2,101,'B',10.00,500),
        dummy(2,T0),
        trd(3,T0+1, 0,0,10.00,500,2),    // 只扣前方, 我0成交
    }, 'B',10.00,3000,T0, E(0,VState::RESTING));

    // S7 边界: 前方量恰等于成交量 → overflow=0 不成交
    run("S7 前方恰好吃完不溢出", {
        ord(1,T0-2,101,'B',10.00,500),
        dummy(2,T0),
        trd(3,T0+1, 0,0,10.00,500,2),
    }, 'B',10.00,1000,T0, EA(0,VState::RESTING,0));

    // S8 恰好吃满 → FILLED, 后续成交忽略
    run("S8 恰好吃满后忽略", {
        dummy(1,T0),
        trd(2,T0+1, 0,0,10.00,1000,2),   // 满
        trd(3,T0+2, 0,0,10.00,500,2),    // 已FILLED, 忽略
    }, 'B',10.00,1000,T0, E(1000,VState::FILLED));

    // S9 价格没到我这 → 不成交
    run("S9 价格没到不成交", {
        dummy(1,T0),
        trd(2,T0+1, 0,0,10.05, 800, 2),  // 成交价10.05 > 我买价10.00, 无关
    }, 'B',10.00,1000,T0, E(0,VState::RESTING));

    // S10 穿透护栏: aheadQty>0 时来了 P 以下成交(异常序), 应不成交、不动ahead
    run("S10 穿透护栏(ahead>0不吃)", {
        ord(1,T0-2,101,'B',10.00,500),
        dummy(2,T0),
        trd(3,T0+1, 0,0, 9.99, 200, 2),  // ahead=500>0, P以下成交 → 忽略
    }, 'B',10.00,1000,T0, EA(0,VState::RESTING,500));

    // S11 单笔成交跨多笔前方单 + 溢出成交我
    run("S11 单成交跨多前方单", {
        ord(1,T0-3,101,'B',10.00,300),
        ord(2,T0-2,102,'B',10.00,200),   // ahead=500
        dummy(3,T0),
        trd(4,T0+1, 0,0,10.00,700,2),    // 吃穿500前方 + 成交我200
    }, 'B',10.00,1000,T0, EA(200,VState::PARTIAL,0));

    // S12 部分撤前方单(撤量<挂量)
    run("S12 部分撤前方", {
        ord(1,T0-2,101,'B',10.00,500),   // ahead=500
        dummy(2,T0),
        ccl(3,T0+1,101,'B',10.00,200),   // 部分撤200 → ahead 500->300
        trd(4,T0+2, 0,0,10.00,400,2),    // 扣300 + 成交我100
    }, 'B',10.00,1000,T0, EA(100,VState::PARTIAL,0));

    std::cout << "\n===== 连续段: 被动卖单(对称) =====\n";

    // S13 扫价穿透(卖): 买吃穿到 P 以上
    run("S13 扫价穿透-卖", {
        ord(1,T0-2,101,'S',10.00,500),   // ahead=500 (卖方P档)
        ord(2,T0,   90001,'B',0.01,1),   // dummy 远价买(不碰P)
        trd(3,T0+1, 0,0,10.00,500,1),    // 扣前方
        trd(4,T0+2, 0,0,10.01,1500,1),   // 买吃穿到10.01: 我卖单应成交1500
    }, 'S',10.00,3000,T0, EV(1500,VState::PARTIAL,10.00));

    // S14 卖单价格没到 → 不成交
    run("S14 卖-价格没到", {
        ord(1,T0, 90001,'B',0.01,1),
        trd(2,T0+1, 0,0, 9.95, 800, 1),  // 成交9.95 < 我卖10.00, 无关
    }, 'S',10.00,1000,T0, E(0,VState::RESTING));

    std::cout << "\n===== 集合竞价 =====\n";
    const double TA=33400.0;   // 09:16:40 开盘竞价内
    const double TX=33900.0+1; // 触发 uncross 的连续段外事件(>=09:25:00)

    // S15 竞价买严格优于清算价 → 全成于 p*(供给充足)
    run("S15 竞价严格优全成", {
        ord(1,TA-10,101,'B',10.00,400),  // 真实买
        ord(2,TA-5, 102,'S',10.00,600),  // 真实卖(交叉累积)
        ord(3,TA, 90001,'S',999.0,1),    // t0 dummy 触发插入
        ord(4,TX, 90002,'S',999.0,1),    // 跨09:25 触发 uncross
    }, 'B',10.05,500,TA, EV(500,VState::FILLED,10.00));

    // S16 竞价超额护栏: 委托量>对手供给 → 封顶到供给
    // 终态是 PARTIAL 不是 RESTING: handcheck case10 抓出的 bug ⑥「竞价部分成交状态误报
    // RESTING」已修复, 竞价与连续段统一用 PARTIAL 口径。本条预期当时漏改。
    run("S16 竞价超额封顶", {
        ord(1,TA-10,101,'B',10.00,100),
        ord(2,TA-5, 102,'S',10.00,300),  // 供给仅300
        ord(3,TA, 90001,'S',999.0,1),
        ord(4,TX, 90002,'S',999.0,1),
    }, 'B',10.05,500,TA, EV(300,VState::PARTIAL,10.00));  // 只成交300(封顶), 残量200转连续

    // S17 竞价边际(P==p*): 我在队尾, 前方真实单先清, 但对手方过剩簿上还有余量归我。
    // 成交 = avail − aheadQty = 600 − 400 = 200, 残量300转连续 → PARTIAL。
    // 旧预期是 filled=0/RESTING, 即 handcheck case9 抓出的 bug ④「竞价边际公式用真实清算量
    // 封顶, 对手方过剩簿上把成交错判为 0」的行为。代码已修, 本条预期当时漏改。
    run("S17 竞价边际吃过剩", {
        ord(1,TA-10,101,'B',10.00,400),  // 前方400 @ p*
        ord(2,TA-5, 102,'S',10.00,600),  // 对手供给600, 真实清算仅400, 过剩200
        ord(3,TA, 90001,'S',999.0,1),
        ord(4,TX, 90002,'S',999.0,1),
    }, 'B',10.00,500,TA, EV(200,VState::PARTIAL,10.00));

    // S18 竞价劣于清算价 → 不成, 转连续
    run("S18 竞价劣于清算不成", {
        ord(1,TA-10,101,'B',10.00,400),
        ord(2,TA-5, 102,'S',10.00,600),
        ord(3,TA, 90001,'S',999.0,1),
        ord(4,TX, 90002,'S',999.0,1),
    }, 'B',9.50,500,TA, E(0,VState::RESTING));

    // S19 竞价成交后残量转连续继续成交
    run("S19 竞价部分+连续补成", {
        ord(1,TA-10,101,'B',10.00,100),
        ord(2,TA-5, 102,'S',10.00,300),  // 竞价可给我 min(500, 300-0)=300
        ord(3,TA, 90001,'S',999.0,1),
        ord(4,TX, 90002,'S',999.0,1),    // uncross: 我竞价成交300
        // 转连续后, 卖单继续在10.00成交, 补我剩余200
        trd(5,CONT_AM_START+10, 0,0,10.00,250,2),
    }, 'B',10.05,500,TA, E(500,VState::FILLED));

    std::cout << "\n===== 其它边界 =====\n";

    // S20 size=1 微单
    run("S20 微单size=1", {
        dummy(1,T0),
        trd(2,T0+1, 0,0,10.00, 50, 2),
    }, 'B',10.00,1,T0, E(1,VState::FILLED));

    // S21 t0前前方单被成交光, t0时aheadQty=0
    run("S21 t0前前方已清空", {
        ord(1,T0-3,101,'B',10.00,500),
        trd(2,T0-2, 101,0,10.00,500,2),  // t0前就把前方吃光(带被吃单的订单号 → 真实簿才扣得掉)
        dummy(3,T0),
        trd(4,T0+1, 0,0,10.00,300,2),    // 我直接成交300
    }, 'B',10.00,1000,T0, EA(300,VState::PARTIAL,0));

    // S22 前方单先部分成交, 再撤掉剩余 → aheadQty 一致
    run("S22 前方部分成交后撤剩余", {
        ord(1,T0-2,101,'B',10.00,500),   // ahead=500
        dummy(2,T0),
        trd(3,T0+1, 0,0,10.00,300,2),    // 前方101 成交300 → 剩200, ahead 500->200
        ccl(4,T0+2,101,'B',10.00,200),   // 撤前方剩余200 → ahead 200->0
        trd(5,T0+3, 0,0,10.00,400,2),    // 我成交 min(400,1000)=400
    }, 'B',10.00,1000,T0, EA(400,VState::PARTIAL,0));

    // S23 穿透量 > 我剩余size → 封顶到size
    run("S23 穿透超size封顶", {
        dummy(1,T0),
        trd(2,T0+1, 0,0, 9.99, 5000, 2),  // 我在队首(ahead=0), 穿透5000 → 只成交我1000
    }, 'B',10.00,1000,T0, E(1000,VState::FILLED));

    // S24 卖方: 撤前方卖单 → 前移(对称验证)
    run("S24 卖-撤前方前移", {
        ord(1,T0-2,101,'S',10.00,500),   // 卖方前方 ahead=500
        ord(2,T0, 90001,'B',0.01,1),     // dummy
        ccl(3,T0+1,101,'S',10.00,500),   // 撤前方 → ahead 500->0
        trd(4,T0+2, 0,0,10.00,300,1),    // 买主动成交我300
    }, 'S',10.00,1000,T0, EA(300,VState::PARTIAL,0));

    // S25 收盘竞价: 连续段挂着 → 进收盘竞价 → 数据止于15:00前, finalize uncross
    run("S25 收盘竞价finalize撮合", {
        ord(1,50000, 90001,'S',999.0,1),      // t0=50000 触发插入(远价, ahead=0)
        ord(2,53850, 201,'B',10.00,400),      // 收盘竞价累积(交叉)
        ord(3,53860, 202,'S',10.00,600),
        // 无 >=54000 事件 → finalize 收口
    }, 'B',10.10,200,50000.0, EV(200,VState::FILLED,10.00));

    // S26 marketable买: 到达即按对手价从触及价往外扫单(成交在对手价, 非我限价)
    run("S26 marketable买-到达扫多档", {
        ord(1,T0-3,101,'S',10.00,300),   // 卖一 10.00 x300
        ord(2,T0-2,102,'S',10.05,200),   // 卖二 10.05 x200
        dummy(3,T0),                      // t0: 我买10.10 扫 10.00(300)+10.05(200)=500
    }, 'B',10.10,1000,T0, EV(500,VState::PARTIAL,10.02));  // avg=(300*10+200*10.05)/500

    // S27 marketable残量: 只被"卖方主动"(穿透)填充, "买方主动"不填(验证主动方向门)
    run("S27 残量只被卖方主动填", {
        ord(1,T0-2,101,'S',10.00,300),   // 卖一10.00x300
        dummy(2,T0),                      // 我买10.10扫300 → 残量700@10.10
        trd(3,T0+1, 0,0,10.05,200,1),    // 买方主动(td=1) → 不该填我买
        trd(4,T0+2, 0,0,10.05,400,2),    // 卖方主动(td=2)穿透10.05<10.10 → 填我400
    }, 'B',10.10,1000,T0, E(700,VState::PARTIAL));  // 300激进+400卖穿透

    // ── 乐观/悲观上下界: 同毫秒 撤前方 vs 成交 ──
    std::cout << "\n===== 同毫秒上下界 (opt/pess) =====\n";
    auto runBand=[&](const std::string& name, std::vector<UnifiedRecord> recs,
                     char side,double px,int64_t qty,double t0,
                     int64_t expPess,int64_t expNeu,int64_t expOpt){
        auto one=[&](Mode m)->int64_t{ VirtualOrderTracker tk(side,px,qty,t0,m);
            auto q=computeOrderBudget(recs); SweepAllocator al(m,&q); al.addLeg(&tk);
            dc::LobBuilder b("T","20260707",false); b.load(std::vector<UnifiedRecord>(recs));
            b.setEventHook([&](const UnifiedRecord&r,const dc::LobBuilder&bb){al.onEvent(r,bb);});
            b.build(); al.finalize(b); return tk.filled(); };
        int64_t p=one(Mode::PESSIMISTIC), n=one(Mode::NEUTRAL), o=one(Mode::OPTIMISTIC);
        bool ok=(p==expPess&&n==expNeu&&o==expOpt);
        if(ok){g_pass++;std::cout<<"  ✅ "<<name<<"  [pess="<<p<<" neu="<<n<<" opt="<<o<<"]\n";}
        else{g_fail++;std::cout<<"  ❌ "<<name<<"  期望[pess="<<expPess<<" neu="<<expNeu<<" opt="<<expOpt
            <<"] 实际[pess="<<p<<" neu="<<n<<" opt="<<o<<"]\n";}
    };
    // 前方500; 同毫秒(10:00:01): 撤前方500 + 成交300。撤在前:成交300归我; 成交在前:归前方→我0
    // B1: seqNo序为 撤→成交(neutral 同 opt)
    runBand("B1 撤在前(neutral=opt)", {
        ord(1,T0-2,101,'B',10.00,500),
        dummy(2,T0),
        ccl(4,T0+1,101,'B',10.00,500),
        trd(5,T0+1, 0,0,10.00,300,2),
    }, 'B',10.00,1000,T0, /*pess*/0,/*neu*/300,/*opt*/300);
    // B2: seqNo序为 成交→撤(neutral 同 pess)
    runBand("B2 成交在前(neutral=pess)", {
        ord(1,T0-2,101,'B',10.00,500),
        dummy(2,T0),
        trd(4,T0+1, 0,0,10.00,300,2),
        ccl(5,T0+1,101,'B',10.00,500),
    }, 'B',10.00,1000,T0, /*pess*/0,/*neu*/0,/*opt*/300);

    // ── 策略主动撤单 ──
    std::cout << "\n===== 策略主动撤单 =====\n";
    auto runCancel=[&](const std::string& name, std::vector<UnifiedRecord> recs,
                       char side,double px,int64_t qty,double t0,double cancelSec,
                       int64_t expFilled, VState expState){
        VirtualOrderTracker tk(side,px,qty,t0,Mode::NEUTRAL,cancelSec);
        auto q=computeOrderBudget(recs); SweepAllocator al(Mode::NEUTRAL,&q); al.addLeg(&tk);
        dc::LobBuilder b("T","20260707",false); b.load(std::move(recs));
        b.setEventHook([&](const UnifiedRecord&r,const dc::LobBuilder&bb){al.onEvent(r,bb);});
        b.build(); al.finalize(b);
        bool ok=(tk.filled()==expFilled && tk.state()==expState);
        if(ok){g_pass++;std::cout<<"  ✅ "<<name<<"\n";}
        else{g_fail++;std::cout<<"  ❌ "<<name<<" 期望 filled="<<expFilled<<" "<<VirtualOrderTracker::stateName(expState)
            <<" 实际 filled="<<tk.filled()<<" "<<VirtualOrderTracker::stateName(tk.state())<<"\n";}
    };
    // C1: 成交在 T0+2, 但 T0+1 撤单 → 撤在成交前, filled=0 CANCELLED
    runCancel("C1 撤单早于成交", {
        dummy(1,T0),
        trd(2,T0+2, 0,0,10.00,500,2),
    }, 'B',10.00,1000,T0, T0+1, 0, VState::CANCELLED);
    // C2: T0+1 成交200, T0+2 撤单 → 保留已成200, 撤余量, CANCELLED
    runCancel("C2 部分成交后撤单", {
        dummy(1,T0),
        trd(2,T0+1, 0,0,10.00,200,2),
        trd(3,T0+3, 0,0,10.00,500,2),   // 撤后成交, 不算我
    }, 'B',10.00,1000,T0, T0+2, 200, VState::CANCELLED);
    // C3: 撤单时刻晚于全成 → 已 FILLED, 撤单无效
    runCancel("C3 全成后撤单无效", {
        dummy(1,T0),
        trd(2,T0+1, 0,0,10.00,1000,2),
    }, 'B',10.00,1000,T0, T0+5, 1000, VState::FILLED);

    // ── 多虚拟单: 主动单预算共享(核心新逻辑) ──
    // 独立评估会让多条腿各自认领同一批流动性(凭空超发)。SweepAllocator 让所有腿从
    // 【同一笔主动单的预算 Q】里按 (价格优先 → 插入先后) 取量。真实簿全程只读。
    std::cout << "\n===== 多虚拟单: 主动单预算共享 =====\n";
    {
        const double TT = 36000.0;   // t0 = 10:00
        // M1 异价耦合: 三档各50真实; A挂买一50, B挂买二50; 卖单 Q=150 扫穿三档。
        //   预算走簿: 买一真实50 → A吃50 → 买二真实50 → 预算耗尽 → B吃0。
        //   (独立评估会给 A=50 且 B=50 —— 同一笔扫单被两档各认领一次)
        //   位移=50: 买三那50手真实成交, 在反事实里被 A 挤掉了。
        runMulti("M1 异价耦合(买一吃满→买二吃不到)", {
            ord(1,35000,1,'B',100.00,50), ord(2,35000,2,'B',99.99,50), ord(3,35000,3,'B',99.98,50),
            ord(4,TT,   4,'S',999.0, 1),                       // t0 触发插入(不穿价)
            ord(10,TT+10,10,'S',99.98,150),                    // 主动卖单 Q=150
            trd(11,TT+10, 1,10, 100.00, 50, 2),
            trd(12,TT+10, 2,10,  99.99, 50, 2),
            trd(13,TT+10, 3,10,  99.98, 50, 2),
        }, { {'B',100.00,50,TT}, {'B',99.99,50,TT} }, {50, 0}, /*displaced=*/50);

        // M2 同价串行: 买一有50真实; A、B 都挂买一各80(A先); 卖单 Q=200。
        //   队列 = [50真实][A 80][B 80] → 主动单200: 真实50 → A 80 → B 70(部分)。
        //   (独立评估会给 A=80 且 B=80 —— 同一份溢出量被算两遍)
        //   位移=100: 主动单在买一就被吃光, 买二/买三的真实成交(各50)在反事实里没发生。
        runMulti("M2 同价串行(A吃满→B只吃到剩下的)", {
            ord(1,35000,1,'B',100.00,50), ord(2,35000,2,'B',99.99,50), ord(3,35000,3,'B',99.98,50),
            ord(4,TT,   4,'S',999.0, 1),
            ord(10,TT+10,10,'S',99.98,200),                    // 主动卖单 Q=200
            trd(11,TT+10, 1,10, 100.00, 50, 2),
            trd(12,TT+10, 2,10,  99.99, 50, 2),
            trd(13,TT+10, 3,10,  99.98, 50, 2),
        }, { {'B',100.00,80,TT}, {'B',100.00,80,TT} }, {80, 70}, /*displaced=*/100);

        // M3 零位移(吃的是主动单的残量 slack): 只有两档各50真实(买盘共100);
        //   卖单 Q=150 → 现实里只成交100, 剩50挂住(slack=50)。
        //   A 吃的50手正好来自这50手 slack → 没从任何真实单嘴里抢 → 位移=0。
        //   这正好解释了"为什么有些例子里 独立/耦合 两个模型答案相同"——slack 够用时不冲突。
        runMulti("M3 零位移(A吃主动单残量, 不挤占真实单)", {
            ord(1,35000,1,'B',100.00,50), ord(2,35000,2,'B',99.99,50),
            ord(4,TT,   4,'S',999.0, 1),
            ord(10,TT+10,10,'S',99.99,150),                    // 主动卖单 Q=150, 只吃得到100
            trd(11,TT+10, 1,10, 100.00, 50, 2),
            trd(12,TT+10, 2,10,  99.99, 50, 2),
        }, { {'B',100.00,50,TT}, {'B',99.99,50,TT} }, {50, 0}, /*displaced=*/0);

        // M4 同时刻两笔 marketable 共享可见对手深度。
        //   卖盘: 100.00 有50, 100.01 有50。两笔虚拟买单同一 t0、都限价 100.01、各 60 手。
        //   A 先扫: 吃 50@100.00 + 10@100.01 = 60(全成)。
        //   B 后扫: 100.00 已被 A 吃光 → 只剩 100.01 的 40 手 → B 只成交 40(不是 60)。
        //   若不共享, B 会把同一批深度【再吃一遍】= 对同一个 D 的重复计数。
        //   位移 = 60 + 40 = 100: marketable 吃的是真实挂单, 全额计入(不能报"零冲击")。
        runMulti("M4 同时刻marketable共享对手深度(B只吃到A剩下的)", {
            ord(1,35000,1,'S',100.00,50), ord(2,35000,2,'S',100.01,50),
            ord(3,TT,   3,'B',1.0, 1),                         // t0 触发插入(远价买, 不穿价)
        }, { {'B',100.01,60,TT}, {'B',100.01,60,TT} }, {60, 40}, /*displaced=*/100);
    }

    runFuzz(500);
    runMultiFuzz(300);

    std::cout << "\n================= 汇总 =================\n";
    std::cout << "通过: " << g_pass << "  失败: " << g_fail << "\n";
    return g_fail>0 ? 1 : 0;
}
