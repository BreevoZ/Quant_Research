// vorder_sim — 基于原始 TL 逐笔的虚拟订单排队/成交模拟器 (B档 L3), 批量多订单。
//
// 真实盘口用 datacheck_final 的 LobBuilder 重建(原样, 虚拟单绝不进簿);
// 每笔虚拟订单一个追踪器旁挂在事件流上, 精确追踪排队与成交; 集合竞价按清算价 uncross。
// 多个虚拟订单【相互独立评估】(每笔都假设自己是唯一插入、排在真实队尾), 符合"不影响盘口"。
//
// 用法:
//   vsim --tl-dir DIR --date YYYYMMDD --orders orders.csv [--out results.csv]
//
// orders.csv 每行一笔生命周期动作(可带表头, 自动跳过):
//   id,action,market,symbol,side,price,qty,t
//   数字编码: action 1=place 2=cancel 3=modify | market 101=sh 102=sz | side 1=buy 2=sell
//            t=距零点毫秒数(整数, 1=1毫秒, 如 34210500)
//   o1,1,101,600000,2,8.93,300000,34210000   # place 卖 600000 (09:30:10)
//   o1,2,,,,,,34260000                        # cancel(仅需 id+t)
//   o2,1,101,600000,1,8.85,100000,34500000   # place 买
//   o2,3,,,,,8.88,100000,37800000            # modify 改价(继承 market/symbol/side)
//   兼容旧字符串写法: action=place/cancel/modify, market=sh/sz, side=buy/sell, t=09:30:10
//   (同一股票多笔会共享一次解析+建簿; 不同股票各扫一次数据。)

#include "lob_builder.h"
#include "tl_parser.h"
#include "virtual_order_tracker.h"
#include "sweep_allocator.h"
#include "rec_cache.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <sys/stat.h>

using namespace vsim;

static std::string lower(std::string s){ for(char&c:s)c=tolower(c); return s; }
static std::string trim(std::string s){
    size_t a=s.find_first_not_of(" \t\r\n"); if(a==std::string::npos) return "";
    size_t b=s.find_last_not_of(" \t\r\n"); return s.substr(a,b-a+1);
}
// "HH:MM:SS[.mmm]" -> 距零点秒数(含小数秒/毫秒)
static double hmsToSec(const std::string& t) {
    int h = std::stoi(t.substr(0,2));
    int m = std::stoi(t.substr(3,2));
    double s = (t.size()>=8) ? std::stod(t.substr(6)) : 0.0;   // 含小数秒(毫秒)
    return h*3600.0 + m*60.0 + s;
}
// 距零点秒数 -> "HH:MM:SS" 或 "HH:MM:SS.mmm"(用于结果显示, 输入数字/字符串输出都一致)
static std::string secToHms(double sec){
    if(sec<0) sec=0;
    long ms=std::lround(sec*1000.0);
    int h=(int)(ms/3600000); ms%=3600000; int m=(int)(ms/60000); ms%=60000;
    int s=(int)(ms/1000); ms%=1000;
    char buf[32];
    if(ms) std::snprintf(buf,sizeof buf,"%02d:%02d:%02d.%03d",h,m,s,(int)ms);
    else   std::snprintf(buf,sizeof buf,"%02d:%02d:%02d",h,m,s);
    return buf;
}
// ── 订单字段解析: 数字编码为主, 兼容旧字符串写法 ──
// t : "HH:MM:SS[.mmm]"(含':') 或 距零点毫秒数(整数, 1=1毫秒, 如 34210500)
static double parseTime(const std::string& s){
    std::string x=trim(s);
    return x.find(':')!=std::string::npos ? hmsToSec(x) : std::stod(x)/1000.0;  // 数字=毫秒 → 秒
}
// side: 1/buy/b -> 'B';  2/sell/s -> 'S';  其它 '?'
static char parseSide(std::string s){ s=lower(trim(s));
    if(s=="1"||s=="buy"||s=="b") return 'B';
    if(s=="2"||s=="sell"||s=="s") return 'S';
    return '?';
}
// market: 101/sh -> "sh";  102/sz -> "sz";  其它原样(分组时自然无数据)
static std::string parseMarket(std::string s){ s=lower(trim(s));
    if(s=="101"||s=="sh") return "sh";
    if(s=="102"||s=="sz") return "sz";
    return s;
}
// action: 1/place -> "place";  2/cancel -> "cancel";  3/modify -> "modify";  其它 ""
static std::string parseAction(std::string s){ s=lower(trim(s));
    if(s=="1"||s=="place")  return "place";
    if(s=="2"||s=="cancel") return "cancel";
    if(s=="3"||s=="modify") return "modify";
    return "";
}

struct OrderSpec {
    std::string dispId, market, symbol, t0s;
    char side; double price; int64_t qty; double t0;
    double cancelSec = 1e18;   // 策略主动撤单时刻(modify 会设成改单时刻)
};
struct Res {
    double arrivalMid=0, avgPx=0, slip=0;
    int64_t filled=0, filledLo=0, filledHi=0, through=0, aheadRem=0;   // filled=中性; [Lo,Hi]=悲观/乐观
    int64_t displaced=0;   // 位移计(整股): 从真实单挤走的量; 0=零冲击
    std::string state;
    bool hasData=true;
    std::vector<Fill> fills;   // 逐笔成交事件(时间/量/价/来源), 供成交时间线追踪
};

int main(int argc, char** argv) {
    std::string tlDir, flowPath, date, ordersPath, outPath, reportPath, fillsPath, cacheDir, symbolsArg;
    bool bounds = true;   // 是否算乐观/悲观上下界(跑3个追踪器); --no-bounds 只跑中性(省2/3追踪开销)
    bool precache = false;// 预缓存模式: 扫源、把(全市场或--symbols指定的)股票解析结果落盘, 不跑订单
    bool trustCache = false;// --trust-cache: 跳过缓存的源指纹核对(源CSV已删、只剩缓存的批量回测场景)
    int workers = 60;     // 解析 + 多股建簿的线程数
    for (int i=1;i<argc;++i){ std::string a=argv[i]; auto nx=[&]{return std::string(argv[++i]);};
        if      (a=="--tl-dir"   && i+1<argc) tlDir=nx();
        else if (a=="--flow"     && i+1<argc) flowPath=nx();
        else if (a=="--date"     && i+1<argc) date=nx();
        else if (a=="--orders"   && i+1<argc) ordersPath=nx();
        else if (a=="--out"      && i+1<argc) outPath=nx();
        else if (a=="--report"   && i+1<argc) reportPath=nx();
        else if (a=="--fills"    && i+1<argc) fillsPath=nx();
        else if (a=="--cache-dir"&& i+1<argc) cacheDir=nx();
        else if (a=="--symbols"  && i+1<argc) symbolsArg=nx();
        else if (a=="--workers"  && i+1<argc) workers=std::stoi(nx());
        else if (a=="--precache")  precache=true;
        else if (a=="--trust-cache") trustCache=true;
        else if (a=="--no-bounds") bounds=false;
        else if (a=="--bounds")    bounds=true;
        else if (a=="-h"||a=="--help"){ std::cout<<
            "用法: vsim (--tl-dir DIR | --flow flow.csv) --date YYYYMMDD --orders orders.csv [选项]\n"
            "  数据源二选一: --tl-dir 解压后的TL目录 / --flow 单个Flow格式CSV(tltoflow产物, 含全市场)\n"
            "  orders.csv 列: id,action,market,symbol,side,price,qty,t  (可带表头)\n"
            "    数字编码: action 1=place 2=cancel 3=modify | market 101=sh 102=sz | side 1=buy 2=sell\n"
            "             t=距零点毫秒数(整数, 1=1毫秒); 旧字符串写法(place/sh/buy/09:30:10)仍兼容\n"
            "  --out FILE       逐腿明细结果CSV\n"
            "  --report FILE    组合级纸面汇总报告(总览/终态/按方向/按市场/按股票/成交追踪); FILE='-' 打到 stdout\n"
            "  --fills FILE     逐笔成交事件CSV(每笔单每次成交一行: 时间/量/价/来源/累计/剩余)\n"
            "  --cache-dir DIR  解析结果缓存目录(同日重复跑免重扫大文件; 按 日期/源/市场/代码 分文件;\n"
            "                   带源文件指纹, 源重生成/解析器升级 → 旧缓存自动失效回源)\n"
            "  --precache       预缓存模式: 扫源一次, 把股票解析结果全部落盘(配 --cache-dir --date, 不需 --orders)\n"
            "  --trust-cache    跳过缓存源指纹核对(源CSV已删只剩缓存的批量回测; MAGIC/版本/长度校验仍生效)\n"
            "  --symbols LIST   配 --precache: 逗号分隔的代码(如 600000,000001; 缺省=全市场)\n"
            "  --workers N      解析+多股并行线程数(默认60)\n"
            "  --no-bounds      只算中性成交量(不算同毫秒上下界, 省 2/3 追踪开销)\n"; return 0; }
    }
    if (workers < 1) workers = 1;

    // ── 预缓存模式: 扫源一次, 把(全市场或 --symbols 指定的)股票解析结果落盘, 不跑订单 ──
    if (precache) {
        if (cacheDir.empty() || date.empty() || (tlDir.empty()&&flowPath.empty())) {
            std::cerr<<"--precache 需要 --cache-dir --date 和数据源(--tl-dir 或 --flow)\n"; return 1; }
        bool useFlow = !flowPath.empty();
        std::string srcTag = useFlow ? "flow" : "tl";
        std::string cacheSub = cacheDir + "/" + date;
        ::mkdir(cacheDir.c_str(),0755); ::mkdir(cacheSub.c_str(),0755);
        auto cachePath=[&](const std::string& key)->std::string{
            std::string k=key; for(char&ch:k) if(ch=='|') ch='_';
            return cacheSub + "/" + srcTag + "_" + k + ".bin"; };
        std::string shP=tlDir+"/mdl_4_24_0.csv", szO=tlDir+"/mdl_6_33_0.csv", szT=tlDir+"/mdl_6_36_0.csv";
        uint64_t fpFlow=sourceFingerprint({flowPath}), fpSH=sourceFingerprint({shP}), fpSZ=sourceFingerprint({szO,szT});
        // --symbols: 显式列表(按代码首位或 .SH/.SZ 后缀分市场); 缺省=全市场
        std::unordered_set<std::string> shSyms, szSyms; bool all = trim(symbolsArg).empty();
        if(!all){ std::stringstream ss(symbolsArg); std::string t;
            while(std::getline(ss,t,',')){ t=trim(t); if(t.empty()) continue;
                size_t dot=t.find('.'); std::string code=(dot==std::string::npos)?t:t.substr(0,dot);
                if(dot!=std::string::npos){ if(lower(t.substr(dot+1))=="sh") shSyms.insert(code); else szSyms.insert(code); }
                else if(!code.empty() && code[0]=='6') shSyms.insert(code); else if(!code.empty()) szSyms.insert(code);
            }
        }
        std::cerr<<"[预缓存] "<<(useFlow?"Flow":"TL")<<" "<<date<<" "<<(all?"全市场":"指定 "+std::to_string(shSyms.size()+szSyms.size())+" 只")<<" ...\n";
        auto _p0=std::chrono::steady_clock::now();
        int nWritten=0;
        if(useFlow){
            std::unordered_map<std::string,std::string> instrToKey;
            if(!all){ for(auto&s:shSyms) instrToKey[s+".SH"]="sh|"+s;
                      for(auto&s:szSyms) instrToKey[padSZ(s)+".SZ"]="sz|"+padSZ(s); }
            auto m=parseFlowSymbols(flowPath, instrToKey, workers, all);
            for(auto&[key,recs]:m){ writeRecCache(cachePath(key), recs, fpFlow); ++nWritten; }
        } else {
            if(all||!shSyms.empty()){ auto m=parseSHSymbols(shP, shSyms, workers, all);
                for(auto&[sym,recs]:m){ writeRecCache(cachePath("sh|"+sym), recs, fpSH); ++nWritten; } }
            if(all||!szSyms.empty()){ auto m=parseSZSymbols(szO, szT, szSyms, workers, all);
                for(auto&[sym,recs]:m){ writeRecCache(cachePath("sz|"+sym), recs, fpSZ); ++nWritten; } }
        }
        auto _p1=std::chrono::steady_clock::now();
        std::cerr<<"[预缓存] 写入 "<<nWritten<<" 只 -> "<<cacheSub<<"  ("
                 <<std::chrono::duration_cast<std::chrono::milliseconds>(_p1-_p0).count()<<" ms)\n";
        return 0;
    }

    if ((tlDir.empty()&&flowPath.empty())||date.empty()||ordersPath.empty()){
        std::cerr<<"缺参数(需 --tl-dir 或 --flow, 且 --date --orders), 见 --help\n"; return 1; }

    // ── 读订单文件: 支持 place/cancel/modify 生命周期动作 ──
    // 格式A(动作): id,action,market,symbol,side,price,qty,t   (action∈place/cancel/modify)
    //   cancel: 仅用 id + t; modify: 用 id + 新 price/qty + t(market/symbol/side 继承原单)
    // 格式B(兼容旧版, 纯下单): id,market,symbol,side,price,qty,t0
    // modify = A股 cancel-replace: t 时刻撤旧单 + 按新价量重挂(丢失排队优先级), 新腿 id 记作 <id>#k
    std::vector<OrderSpec> specs;
    { std::ifstream f(ordersPath);
      if(!f){ std::cerr<<"无法打开订单文件: "<<ordersPath<<"\n"; return 1; }
      std::string line;
      std::unordered_map<std::string,int> live, legcnt;  // 用户id -> 当前腿 index / 腿计数
      auto isAct=[](const std::string&s){ return !parseAction(s).empty(); };
      int lineNo=0, badRows=0; std::string firstBad;
      auto headerLike=[&](const std::string&L){ std::string x=lower(L);
          return x.find("symbol")!=std::string::npos||x.find("price")!=std::string::npos
               ||x.find("action")!=std::string::npos||x.find("qty")!=std::string::npos; };
      auto noteBad=[&](const std::string&L){ if(!badRows) firstBad="第"+std::to_string(lineNo)+"行: "+trim(L); ++badRows; };
      while(std::getline(f,line)){
        ++lineNo;
        if(line.empty()) continue;
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        std::vector<std::string> c; std::stringstream ss(line); std::string tok;
        while(std::getline(ss,tok,',')) c.push_back(tok);
        if(c.size()<7){ if(!headerLike(line)) noteBad(line); continue; }

        std::string kind, id, mkt, sym, sds, ps, qs, ts;
        if(c.size()>=8 && isAct(c[1])){        // 格式A(带动作)
            id=trim(c[0]); kind=parseAction(c[1]); mkt=parseMarket(c[2]); sym=trim(c[3]); sds=c[4]; ps=c[5]; qs=c[6]; ts=c[7];
        } else {                                // 格式B(兼容旧版, 纯下单)
            kind="place"; id=trim(c[0]); mkt=parseMarket(c[1]); sym=trim(c[2]); sds=c[3]; ps=c[4]; qs=c[5]; ts=c[6];
        }

        if(kind=="cancel"){
            double t; try{ t=parseTime(ts);}catch(...){ noteBad(line); continue; }
            auto it=live.find(id);
            if(it!=live.end()) specs[it->second].cancelSec=t;
            else std::cerr<<"  [警告] cancel 引用未知 id: "<<id<<"\n";
            continue;
        }
        if(kind=="modify"){
            double t; double np; int64_t nq;
            try{ t=parseTime(ts); np=std::stod(trim(ps)); nq=std::stoll(trim(qs));}catch(...){ noteBad(line); continue; }
            auto it=live.find(id);
            if(it==live.end()){ std::cerr<<"  [警告] modify 引用未知 id: "<<id<<"\n"; continue; }
            int cur=it->second; specs[cur].cancelSec=t;                 // 结束当前腿
            OrderSpec o=specs[cur];                                     // 继承 market/symbol/side
            o.price=np; o.qty=nq; o.t0=t; o.t0s=secToHms(t); o.cancelSec=1e18;
            int k=++legcnt[id]; o.dispId=id+"#"+std::to_string(k);
            specs.push_back(o); live[id]=(int)specs.size()-1;
            continue;
        }
        // place
        OrderSpec o; o.dispId=id; o.market=mkt; o.symbol=sym;
        o.side=parseSide(sds);
        try { o.price=std::stod(trim(ps)); o.qty=std::stoll(trim(qs)); o.t0=parseTime(ts); }
        catch(...) { if(!headerLike(line)) noteBad(line); continue; }  // 表头静默跳过, 坏行计数
        if(o.side=='?'||o.qty<=0||o.price<=0){ if(!headerLike(line)) noteBad(line); continue; }
        o.t0s=secToHms(o.t0);
        specs.push_back(o); live[id]=(int)specs.size()-1; legcnt[id]=1;
      }
      if(badRows) std::cerr<<"  [警告] 跳过 "<<badRows<<" 行无效订单(字段数/数值/时间格式错误), 首个 "<<firstBad<<"\n";
    }
    if(specs.empty()){ std::cerr<<"订单文件无有效订单\n"; return 1; }
    std::cerr<<"[1] 读入 "<<specs.size()<<" 条订单腿(含 modify 拆出的新腿)\n";

    // ── 按(市场,股票)分组 ──
    std::vector<Res> results(specs.size());
    std::unordered_map<std::string,std::vector<int>> groups;  // key = market|symbol
    for(int i=0;i<(int)specs.size();++i) groups[specs[i].market+"|"+specs[i].symbol].push_back(i);

    // ── 解析数据源(TL 或 Flow), 统一归一到 bySymKey[market|symbol]; 支持缓存 ──
    bool useFlow = !flowPath.empty();
    std::string srcTag = useFlow ? "flow" : "tl";
    std::string cacheSub;
    if(!cacheDir.empty()){ cacheSub = cacheDir + "/" + date;
        ::mkdir(cacheDir.c_str(),0755); ::mkdir(cacheSub.c_str(),0755); }
    auto cachePath=[&](const std::string& key)->std::string{
        std::string k=key; for(char&ch:k) if(ch=='|') ch='_';
        return cacheSub + "/" + srcTag + "_" + k + ".bin"; };
    // 源文件指纹: 缓存失效检测(源被重生成/解析器版本变 → 命中的缓存自动作废回源)
    std::string shPath=tlDir+"/mdl_4_24_0.csv", szOrdPath=tlDir+"/mdl_6_33_0.csv", szTrdPath=tlDir+"/mdl_6_36_0.csv";
    uint64_t fpFlow = sourceFingerprint({flowPath});
    uint64_t fpSH   = sourceFingerprint({shPath});
    uint64_t fpSZ   = sourceFingerprint({szOrdPath, szTrdPath});
    auto fpFor=[&](const std::string& market)->uint64_t{
        return useFlow ? fpFlow : (market=="sh" ? fpSH : fpSZ); };

    std::cerr<<"[2] 解析数据源: "<<(useFlow?("Flow "+flowPath):("TL "+tlDir))
             <<" ("<<groups.size()<<" 只, workers="<<workers<<")...\n";
    auto _t0=std::chrono::steady_clock::now();
    std::unordered_map<std::string,std::vector<dc::UnifiedRecord>> bySymKey;   // key = market|symbol

    // 1) 先从缓存加载能命中的(校验源指纹, 过期自动回源)
    std::vector<std::string> needParse;
    int nCacheHit=0;
    for(auto&[key,idxs]:groups){
        if(!cacheDir.empty()){
            std::vector<dc::UnifiedRecord> r;
            if(readRecCache(cachePath(key), r, fpFor(specs[idxs[0]].market), trustCache)){ bySymKey[key]=std::move(r); ++nCacheHit; continue; }
        }
        needParse.push_back(key);
    }
    if(!cacheDir.empty()) std::cerr<<"    缓存命中 "<<nCacheHit<<"/"<<groups.size()<<" 只\n";

    // 2) 未命中的从源解析(只扫这些股票), 并写缓存
    if(!needParse.empty()){
        std::unordered_set<std::string> needKeys(needParse.begin(), needParse.end());
        if(useFlow){
            std::unordered_map<std::string,std::string> instrToKey;
            for(const auto&key:needParse){ const auto&s0=specs[groups[key][0]];
                std::string instr=(s0.market=="sh")?(s0.symbol+".SH"):(padSZ(s0.symbol)+".SZ");
                instrToKey[instr]=key; }
            auto m = parseFlowSymbols(flowPath, instrToKey, workers);
            for(const auto&key:needParse) bySymKey[key] = std::move(m[key]);
        } else {
            std::unordered_set<std::string> shSyms, szSyms;
            for(const auto&key:needParse){ const auto&s0=specs[groups[key][0]];
                if(s0.market=="sh") shSyms.insert(s0.symbol); else szSyms.insert(s0.symbol); }
            std::unordered_map<std::string,std::vector<dc::UnifiedRecord>> shMap, szMap;
            if(!shSyms.empty()) shMap = parseSHSymbols(shPath, shSyms, workers);
            if(!szSyms.empty()) szMap = parseSZSymbols(szOrdPath, szTrdPath, szSyms, workers);
            for(const auto&key:needParse){ const auto&s0=specs[groups[key][0]];
                if(s0.market=="sh"){ auto it=shMap.find(s0.symbol); if(it!=shMap.end()) bySymKey[key]=std::move(it->second); }
                else               { auto it=szMap.find(padSZ(s0.symbol)); if(it!=szMap.end()) bySymKey[key]=std::move(it->second); }
            }
        }
        // 写缓存(带源指纹; 仅当源文件存在, 避免把"文件缺失导致的空结果"缓存成"该股无数据")
        if(!cacheDir.empty()){
            for(const auto&key:needParse){ auto it=bySymKey.find(key); if(it==bySymKey.end()) continue;
                const auto& s0=specs[groups[key][0]];
                bool srcOk = useFlow ? fileExists(flowPath)
                           : (s0.market=="sh" ? fileExists(shPath) : (fileExists(szOrdPath)&&fileExists(szTrdPath)));
                if(srcOk) writeRecCache(cachePath(key), it->second, fpFor(s0.market));
            }
        }
    }
    auto _t1=std::chrono::steady_clock::now();
    std::cerr<<"    解析耗时: "<<std::chrono::duration_cast<std::chrono::milliseconds>(_t1-_t0).count()<<" ms\n";

    // ── 多股并行: 每组(股票)独立建簿+追踪, 写各自 results[idx](互不冲突) ──
    std::cerr<<"[3] 逐股重建盘口 + 追踪 ("<<groups.size()<<" 组, 并行)...\n";
    struct WorkItem { std::string key; std::vector<int> idxs; std::vector<dc::UnifiedRecord> recs; };
    std::vector<WorkItem> items;
    items.reserve(groups.size());
    for(auto&[key,idxs]:groups){ WorkItem w; w.key=key; w.idxs=idxs;
        auto it=bySymKey.find(key); if(it!=bySymKey.end()) w.recs=std::move(it->second);
        items.push_back(std::move(w)); }

    auto doItem=[&](WorkItem& w){
        const OrderSpec& s0 = specs[w.idxs[0]];
        bool isSH = (s0.market=="sh");
        if(w.recs.empty()){
            for(int idx:w.idxs){ results[idx].hasData=false; results[idx].state="NO_DATA"; }
            return;
        }
        // 主动单预算 Q(用【重排前的原始 seqNo】算, 故在 load 之前先扫一遍)
        auto orderQ = computeOrderBudget(w.recs);

        std::vector<std::unique_ptr<VirtualOrderTracker>> tkN, tkO, tkP;
        for(int idx:w.idxs){ const auto&o=specs[idx];
            tkN.push_back(std::make_unique<VirtualOrderTracker>(o.side,o.price,o.qty,o.t0,Mode::NEUTRAL,o.cancelSec));
            if(bounds){
                tkO.push_back(std::make_unique<VirtualOrderTracker>(o.side,o.price,o.qty,o.t0,Mode::OPTIMISTIC,o.cancelSec));
                tkP.push_back(std::make_unique<VirtualOrderTracker>(o.side,o.price,o.qty,o.t0,Mode::PESSIMISTIC,o.cancelSec));
            }
        }
        // 每个口径一个分配器: 同股票所有腿共享【主动单预算】, 消除凭空超发(同价串行 + 异价耦合)
        SweepAllocator alN(Mode::NEUTRAL, &orderQ);
        SweepAllocator alO(Mode::OPTIMISTIC, &orderQ), alP(Mode::PESSIMISTIC, &orderQ);
        for(auto&t:tkN) alN.addLeg(t.get());
        for(auto&t:tkO) alO.addLeg(t.get());
        for(auto&t:tkP) alP.addLeg(t.get());

        dc::LobBuilder b(s0.symbol,date,isSH);
        b.setLiteMode(true);
        b.load(std::move(w.recs));
        b.setEventHook([&](const dc::UnifiedRecord& r, const dc::LobBuilder& bb){
            alN.onEvent(r,bb);
            if(bounds){ alO.onEvent(r,bb); alP.onEvent(r,bb); }
        });
        b.build();
        alN.finalize(b);
        if(bounds){ alO.finalize(b); alP.finalize(b); }

        for(size_t k=0;k<tkN.size();++k){
            int idx=w.idxs[k]; const auto&o=specs[idx]; auto&R=results[idx];
            R.filled=tkN[k]->filled(); R.through=tkN[k]->throughFilled();
            R.aheadRem=tkN[k]->aheadRemaining(); R.arrivalMid=tkN[k]->arrivalMid();
            R.state=VirtualOrderTracker::stateName(tkN[k]->state());
            if(bounds){ R.filledLo=tkP[k]->filled(); R.filledHi=tkO[k]->filled(); }
            else { R.filledLo=R.filled; R.filledHi=R.filled; }
            if(R.filled>0){ R.avgPx=tkN[k]->avgFillPrice();
                R.slip=(o.side=='B')?(R.avgPx-R.arrivalMid):(R.arrivalMid-R.avgPx); }
            R.fills=tkN[k]->fills();   // 逐笔成交事件(中性追踪器为准)
        }
        // 位移计(整只股票共用): 从真实单挤走的量。0 = 零冲击。
        for(int idx:w.idxs) results[idx].displaced = alN.displaced();
    };

    std::atomic<size_t> nextItem{0};
    int nt = std::min<int>(workers, (int)items.size()); if(nt<1) nt=1;
    std::vector<std::thread> pool;
    for(int t=0;t<nt;++t) pool.emplace_back([&](){
        size_t gi; while((gi=nextItem.fetch_add(1)) < items.size()) doItem(items[gi]);
    });
    for(auto&t:pool) t.join();

    auto _t2=std::chrono::steady_clock::now();
    std::cerr<<"    建簿+追踪耗时: "<<std::chrono::duration_cast<std::chrono::milliseconds>(_t2-_t1).count()<<" ms\n";

    // ── 输出 ──
    auto emit=[&](std::ostream& os){
        os<<"id,market,symbol,side,price,qty,t0,arrival_mid,filled,filled_pess,filled_opt,fill_pct,avg_price,through_filled,slippage,state,ahead_remaining,displaced,n_fills,first_fill_t,last_fill_t\n";
        for(int i=0;i<(int)specs.size();++i){ const auto&o=specs[i]; const auto&R=results[i];
            double pct = o.qty>0 ? 100.0*R.filled/o.qty : 0;
            os<<o.dispId<<","<<o.market<<","<<o.symbol<<","<<(o.side=='B'?"buy":"sell")<<","
              <<o.price<<","<<o.qty<<","<<o.t0s<<","
              <<(R.hasData?R.arrivalMid:0)<<","<<R.filled<<","<<R.filledLo<<","<<R.filledHi<<","<<pct<<","
              <<(R.filled>0?R.avgPx:0)<<","<<R.through<<","<<(R.filled>0?R.slip:0)<<","
              <<R.state<<","<<R.aheadRem<<","<<R.displaced<<","
              <<R.fills.size()<<","
              <<(R.fills.empty()?"":R.fills.front().timeStr)<<","
              <<(R.fills.empty()?"":R.fills.back().timeStr)<<"\n";
        }
    };
    // 逐笔成交事件: 每笔单每次成交一行, 追踪到全成(remaining=0)
    auto emitFills=[&](std::ostream& os){
        os<<"id,market,symbol,side,limit_price,order_qty,seq,fill_time,fill_qty,fill_price,venue,cum_filled,remaining\n";
        for(int i=0;i<(int)specs.size();++i){ const auto&o=specs[i]; const auto&R=results[i];
            int64_t cum=0; int seq=0;
            for(const auto&f:R.fills){ cum+=f.qty; ++seq;
                os<<o.dispId<<","<<o.market<<","<<o.symbol<<","<<(o.side=='B'?"buy":"sell")<<","
                  <<o.price<<","<<o.qty<<","<<seq<<","<<f.timeStr<<","<<f.qty<<","<<f.price<<","
                  <<f.venue<<","<<cum<<","<<(o.qty-cum)<<"\n";
            }
        }
    };

    // ── 组合级纸面汇总报告 ──
    auto writeReport=[&](std::ostream& os){
        const int N=(int)specs.size();
        auto grp=[](int64_t v){                 // 千分位, 负数保号
            bool neg=v<0; unsigned long long u=neg?-(unsigned long long)v:v;
            std::string s=std::to_string(u); for(int p=(int)s.size()-3;p>0;p-=3) s.insert(p,",");
            return neg? "-"+s : s; };
        auto money=[](double v){                 // 名义金额: 取整到元 + 千分位
            char b[40]; std::snprintf(b,sizeof b,"%.0f",v); std::string s=b;
            bool neg=!s.empty()&&s[0]=='-'; if(neg)s=s.substr(1);
            for(int p=(int)s.size()-3;p>0;p-=3) s.insert(p,",");
            return (neg?"-":"")+s; };
        // 组合级聚合(displaced 是【每股共享】的量, 组合合计须按股票去重)
        struct Agg{ int legs=0,noData=0,zeroImp=0; int64_t qty=0,filled=0,lo=0,hi=0,through=0;
                    double reqNotional=0,filNotional=0,slipWtSum=0; int64_t slipWtVol=0; };
        auto fold=[&](Agg&a,int i){ const auto&o=specs[i]; const auto&R=results[i];
            a.legs++; if(!R.hasData){a.noData++;return;}
            a.qty+=o.qty; a.filled+=R.filled; a.lo+=R.filledLo; a.hi+=R.filledHi; a.through+=R.through;
            a.reqNotional+=o.price*(double)o.qty;
            if(R.filled>0){ a.filNotional+=R.avgPx*(double)R.filled;
                a.slipWtSum+=R.slip*(double)R.filled; a.slipWtVol+=R.filled; }
        };
        Agg all; for(int i=0;i<N;++i) fold(all,i);
        // 位移按股票去重: 同股票所有腿共享同一个 displaced 值
        std::unordered_map<std::string,int64_t> symDisp; std::unordered_set<std::string> mkts;
        for(int i=0;i<N;++i){ if(!results[i].hasData) continue;
            symDisp[specs[i].symbol]=results[i].displaced; mkts.insert(specs[i].market); }
        int64_t totDisp=0; for(auto&kv:symDisp) totDisp+=kv.second;
        for(int i=0;i<N;++i) if(results[i].hasData && results[i].displaced==0) all.zeroImp++;

        std::string mktList; for(auto&m:mkts){ if(!mktList.empty()) mktList+=", "; mktList+=m; }
        double fillPct = all.qty>0 ? 100.0*all.filled/all.qty : 0;
        double thrPct  = all.filled>0 ? 100.0*all.through/all.filled : 0;
        double dispPct = all.filled>0 ? 100.0*totDisp/all.filled : 0;
        double wSlip   = all.slipWtVol>0 ? all.slipWtSum/all.slipWtVol : 0;

        os<<"================================================================\n";
        os<<"            虚拟订单批量模拟 — 组合汇总报告\n";
        os<<"================================================================\n";
        os<<"日期        "<<date<<"\n";
        os<<"数据源      "<<(useFlow?("Flow  "+flowPath):("TL    "+tlDir))<<"\n";
        os<<"订单腿      "<<all.legs<<"   (含 modify 拆出的新腿)\n";
        os<<"股票 / 市场 "<<symDisp.size()<<" 只 / "<<mktList<<"\n";
        if(all.noData) os<<"无数据腿    "<<all.noData<<"  (该股当日无逐笔, 记 NO_DATA)\n";
        os<<"\n── 组合总览 ────────────────────────────────────────────────\n";
        os<<"  委托量          "<<grp(all.qty)<<" 股\n";
        os<<"  成交量(中性)    "<<grp(all.filled)<<" 股   ("<<std::fixed<<std::setprecision(1)<<fillPct<<"% 填单率)\n";
        os<<"  成交量区间      [悲观 "<<grp(all.lo)<<" , 乐观 "<<grp(all.hi)<<"] 股  (同毫秒次序不确定性)\n";
        os<<"  委托名义        "<<money(all.reqNotional)<<" 元\n";
        os<<"  成交名义        "<<money(all.filNotional)<<" 元\n";
        os<<"  其中穿透成交    "<<grp(all.through)<<" 股   ("<<std::setprecision(1)<<thrPct<<"% of filled, 即到达即扫价穿透部分)\n";
        os<<"  市场冲击(位移)  "<<grp(totDisp)<<" 股   ("<<dispPct<<"% of filled, 从真实挂单挤走; 按股去重)\n";
        os<<"  零冲击腿        "<<all.zeroImp<<" / "<<(all.legs-all.noData)<<"  (displaced=0, 只吃了主动单余量)\n";
        os<<"  成交加权滑点    "<<std::showpos<<std::setprecision(4)<<wSlip<<std::noshowpos
          <<" 元/股  vs 到达中价 (正=不利: 买贵了/卖低了)\n";

        // 终态分布
        std::map<std::string,int> stCnt; for(int i=0;i<N;++i) stCnt[results[i].state]++;
        os<<"\n── 终态分布 ────────────────────────────────────────────────\n";
        for(auto&kv:stCnt) os<<"  "<<std::left<<std::setw(12)<<kv.first<<std::right<<kv.second<<"\n";

        // 分组表(按方向 / 按市场): displaced 是股票级共享量, 无法干净拆到 side/market, 故此处不列
        auto groupTable=[&](const std::string& title, auto keyOf){
            std::map<std::string,Agg> g;
            for(int i=0;i<N;++i) fold(g[keyOf(i)], i);
            os<<"\n── "<<title<<" ──────────────────────────────────────────────\n";
            // 表头用 ASCII: setw 按字节数对齐, 中文列宽会与数字列错位
            os<<"  "<<std::left<<std::setw(6)<<"group"<<std::right<<std::setw(5)<<"legs"
              <<std::setw(14)<<"req_qty"<<std::setw(14)<<"filled"<<std::setw(8)<<"fill%"
              <<std::setw(12)<<"through"<<std::setw(11)<<"wslip"<<"\n";
            for(auto&kv:g){ Agg&a=kv.second;
                double fp=a.qty>0?100.0*a.filled/a.qty:0;
                double ws=a.slipWtVol>0?a.slipWtSum/a.slipWtVol:0;
                os<<"  "<<std::left<<std::setw(6)<<kv.first<<std::right<<std::setw(5)<<a.legs
                  <<std::setw(14)<<grp(a.qty)<<std::setw(14)<<grp(a.filled)
                  <<std::fixed<<std::setprecision(1)<<std::setw(8)<<fp
                  <<std::setw(12)<<grp(a.through)
                  <<std::showpos<<std::setprecision(4)<<std::setw(11)<<ws<<std::noshowpos<<"\n";
            }
        };
        groupTable("按方向", [&](int i){ return std::string(specs[i].side=='B'?"buy":"sell"); });
        groupTable("按市场", [&](int i){ return specs[i].market; });

        // 按股票(位移在此层良好定义: 每股一个值)
        os<<"\n── 按股票 (position=filled, impact=displaced) ───────────────\n";
        os<<"  "<<std::left<<std::setw(8)<<"symbol"<<std::setw(4)<<"mkt"<<std::right<<std::setw(5)<<"legs"
          <<std::setw(14)<<"req_qty"<<std::setw(14)<<"filled"<<std::setw(8)<<"fill%"
          <<std::setw(14)<<"displaced"<<std::setw(8)<<"disp%"<<"\n";
        std::map<std::string,Agg> gs; std::map<std::string,std::string> smk;
        for(int i=0;i<N;++i){ fold(gs[specs[i].symbol], i); smk[specs[i].symbol]=specs[i].market; }
        for(auto&kv:gs){ Agg&a=kv.second; int64_t d=symDisp.count(kv.first)?symDisp[kv.first]:0;
            double fp=a.qty>0?100.0*a.filled/a.qty:0;
            double dp=a.filled>0?100.0*d/a.filled:0;
            os<<"  "<<std::left<<std::setw(8)<<kv.first<<std::setw(4)<<smk[kv.first]<<std::right<<std::setw(5)<<a.legs
              <<std::setw(14)<<grp(a.qty)<<std::setw(14)<<grp(a.filled)
              <<std::fixed<<std::setprecision(1)<<std::setw(8)<<fp
              <<std::setw(14)<<grp(d)<<std::setw(8)<<dp<<"\n";
        }

        // 成交进度: 每笔单一行 — 首笔成交→末笔成交时刻 + 笔数, 逐笔明细见 --fills CSV
        auto hhmsms=[](const std::string& t){ return t.size()>12? t.substr(0,12): t; };  // 截到毫秒
        os<<"\n── 成交进度 (每笔: 首成→全成 时刻; 逐笔明细见 --fills) ───────\n";
        os<<"  "<<std::left<<std::setw(10)<<"id"<<std::setw(8)<<"symbol"<<std::setw(5)<<"side"
          <<std::right<<std::setw(11)<<"order_qty"<<std::setw(11)<<"filled"
          <<"  "<<std::left<<std::setw(13)<<"first_fill"<<std::setw(13)<<"last_fill"
          <<std::right<<std::setw(4)<<"n"<<"  "<<std::left<<"state"<<"\n";
        for(int i=0;i<N;++i){ const auto&o=specs[i]; const auto&R=results[i];
            std::string ff = R.fills.empty()? "-" : hhmsms(R.fills.front().timeStr);
            std::string lf = R.fills.empty()? "-" : hhmsms(R.fills.back().timeStr);
            bool full = R.filled>=o.qty;
            os<<"  "<<std::left<<std::setw(10)<<o.dispId<<std::setw(8)<<o.symbol
              <<std::setw(5)<<(o.side=='B'?"buy":"sell")
              <<std::right<<std::setw(11)<<grp(o.qty)<<std::setw(11)<<grp(R.filled)
              <<"  "<<std::left<<std::setw(13)<<ff<<std::setw(13)<<lf
              <<std::right<<std::setw(4)<<R.fills.size()
              <<"  "<<std::left<<R.state<<(full&&R.filled>0?" ✓":"")<<"\n";
        }
        os<<"================================================================\n";
    };

    std::cout<<"\n================ 批量虚拟订单结果 ================\n";
    std::cout<<date<<"  共 "<<specs.size()<<" 笔  (filled=中性; [pess,opt]=同毫秒下/上界)\n";
    printf("%-6s %-3s %-7s %-4s %9s %9s %9s %-19s %-8s\n",
           "id","mkt","symbol","side","price","qty","filled","[pess , opt]","state");
    for(int i=0;i<(int)specs.size();++i){ const auto&o=specs[i]; const auto&R=results[i];
        char band[32]; snprintf(band,sizeof(band),"[%lld,%lld]",(long long)R.filledLo,(long long)R.filledHi);
        printf("%-6s %-3s %-7s %-4s %9.3f %9lld %9lld %-19s %-8s\n",
               o.dispId.c_str(), o.market.c_str(), o.symbol.c_str(), (o.side=='B'?"buy":"sell"),
               o.price, (long long)o.qty, (long long)R.filled, band, R.state.c_str());
    }

    if(!outPath.empty()){ std::ofstream of(outPath); emit(of);
        std::cerr<<"结果已写: "<<outPath<<"\n"; }
    if(!fillsPath.empty()){ std::ofstream ff(fillsPath); emitFills(ff);
        std::cerr<<"成交事件已写: "<<fillsPath<<"\n"; }
    if(!reportPath.empty()){
        if(reportPath=="-"){ std::cout<<"\n"; writeReport(std::cout); }
        else { std::ofstream rf(reportPath); writeReport(rf);
            std::cerr<<"报告已写: "<<reportPath<<"\n"; }
    }
    return 0;
}
