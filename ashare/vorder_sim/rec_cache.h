#pragma once
// 解析结果缓存: 把某股票已解析的 UnifiedRecord 存成定长二进制, 同日重复跑免重扫大文件。
// 缓存按 (date, 源类型, market, symbol) 分文件; 换源(tl/flow)或换日自动隔离。

#include "lob_builder.h"
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <sys/stat.h>

namespace vsim {

// 定长序列化记录(time 用定长 char, 便于批量读写)。UnifiedRecord.time 实测 ≤18 字符。
#pragma pack(push, 1)
struct CachedRec {
    int64_t seqNo;
    double  timeSeconds;
    int32_t actionType;
    char    direction;
    char    priceType;
    int32_t tradeDirection;
    int64_t sysid, buyId, sellId;
    double  price, turnover;
    int64_t intPrice, volume;
    char    time[24];
};
#pragma pack(pop)

static const char VSIM_CACHE_MAGIC[8] = {'V','S','I','M','C','A','0','2'};
// 解析逻辑变更时 +1 → 旧缓存自动失效(如 SH 中性成交 tradeDirection 口径修正)。
constexpr uint32_t VSIM_PARSER_VERSION = 2;

inline bool fileExists(const std::string& p) {
    struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// 源文件指纹: 各源文件 (size, mtime) 的 FNV-1a 混合。任一源文件被重生成 → 指纹变 → 缓存失效。
inline uint64_t sourceFingerprint(const std::vector<std::string>& paths) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v){ h ^= v; h *= 1099511628211ull; };
    for (const auto& p : paths) {
        struct stat st;
        if (::stat(p.c_str(), &st) == 0) { mix((uint64_t)st.st_size); mix((uint64_t)st.st_mtime); }
        else mix(0xDEADBEEFull);
    }
    return h;
}

inline void writeRecCache(const std::string& path, const std::vector<dc::UnifiedRecord>& recs,
                          uint64_t fingerprint) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(VSIM_CACHE_MAGIC, 8);
    uint32_t ver = VSIM_PARSER_VERSION;
    f.write(reinterpret_cast<const char*>(&ver), sizeof(ver));
    f.write(reinterpret_cast<const char*>(&fingerprint), sizeof(fingerprint));
    uint64_t n = recs.size();
    f.write(reinterpret_cast<const char*>(&n), sizeof(n));
    std::vector<CachedRec> buf(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
        const auto& r = recs[i]; auto& c = buf[i];
        c.seqNo=r.seqNo; c.timeSeconds=r.timeSeconds; c.actionType=r.actionType;
        c.direction=r.direction; c.priceType=r.priceType; c.tradeDirection=r.tradeDirection;
        c.sysid=r.sysid; c.buyId=r.buyId; c.sellId=r.sellId;
        c.price=r.price; c.turnover=r.turnover; c.intPrice=r.intPrice; c.volume=r.volume;
        std::memset(c.time, 0, sizeof(c.time));
        std::strncpy(c.time, r.time.c_str(), sizeof(c.time) - 1);
    }
    if (!buf.empty()) f.write(reinterpret_cast<const char*>(buf.data()),
                              (std::streamsize)(buf.size() * sizeof(CachedRec)));
}

// 读缓存到 recs; 成功返回 true。失败返回 false(不存在/坏文件/版本或源指纹不匹配 → 视为未命中, 回源重解析)。
inline bool readRecCache(const std::string& path, std::vector<dc::UnifiedRecord>& recs,
                         uint64_t expectFingerprint, bool trustFingerprint = false) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8];
    if (!f.read(magic, 8) || std::memcmp(magic, VSIM_CACHE_MAGIC, 8) != 0) return false;
    uint32_t ver = 0;
    if (!f.read(reinterpret_cast<char*>(&ver), sizeof(ver)) || ver != VSIM_PARSER_VERSION) return false;
    uint64_t fp = 0;
    if (!f.read(reinterpret_cast<char*>(&fp), sizeof(fp))) return false;
    // trustFingerprint(--trust-cache): 源 CSV 已删、只剩缓存的场景(如半年批量回测)跳过指纹核对。
    // MAGIC/版本/长度校验仍然生效; 源真变过而缓存没重生成的风险由调用方承担。
    if (!trustFingerprint && fp != expectFingerprint) return false;  // 源文件变了
    uint64_t n = 0;
    if (!f.read(reinterpret_cast<char*>(&n), sizeof(n))) return false;
    std::vector<CachedRec> buf(n);
    if (n && !f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)(n * sizeof(CachedRec)))) return false;
    recs.clear(); recs.resize(n);
    for (uint64_t i = 0; i < n; ++i) {
        const auto& c = buf[i]; auto& r = recs[i];
        r.seqNo=c.seqNo; r.timeSeconds=c.timeSeconds; r.actionType=c.actionType;
        r.direction=c.direction; r.priceType=c.priceType; r.tradeDirection=c.tradeDirection;
        r.sysid=c.sysid; r.buyId=c.buyId; r.sellId=c.sellId;
        r.price=c.price; r.turnover=c.turnover; r.intPrice=c.intPrice; r.volume=c.volume;
        r.time.assign(c.time, strnlen(c.time, sizeof(c.time)));
    }
    return true;
}

} // namespace vsim
