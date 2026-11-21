// 美柚宝宝记 —— ESP32 直连客户端
//
// 只走云：所有记录写到美柚，不在板子留副本（按需求选择）。
// 依赖：esp_http_client、cJSON、CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
// 配置：main/meiyou_config.h（由 PC 端绑定助手导出）
#pragma once

#include <string>

struct BabyToday {
    bool ok = false;
    int total_ml = 0;      // 今日总奶量
    int formula_ml = 0;    // 配方奶
    int breast_ml = 0;     // 母乳
    int count = 0;         // 今日喂养次数
    int last_ml = 0;       // 上次奶量
    int last_minutes = 0;  // 上次亲喂时长（category=11 时才有值）
    bool last_is_nurse = false;  // 上次是不是亲喂（不是瓶喂）
    std::string last_at;   // 上次时间 "yyyy-MM-dd HH:mm:ss"
    int minutes_ago = -1;  // 距上次多少分钟
    std::string error;     // ok=false 时的原因（直接给 LLM 转述）
};

// 上一次大小便
struct BabyLastDiaper {
    bool ok = false;
    bool has_record = false;
    std::string kind;      // "pee" 尿 / "poop" 大便 / "unknown" 未区分
    std::string at;        // "yyyy-MM-dd HH:mm:ss"
    int minutes_ago = -1;
    std::string error;
};

namespace baby {

// 当前本地时间，"yyyy-MM-dd HH:mm:ss"。
// 注意：小智固件在 OTA 时已把 timestamp+timezone_offset 写进系统时间，
// 所以 localtime() 拿到的就是北京时间，不要再叠加时区。
std::string NowLocal();

// token 是否已过本地记录的到期时间（只是预判，不代表服务端真的拒了）
bool TokenExpired();

// 记一次喂养。breast=true 为母乳，false 为配方奶。
// at_local 传空串表示"现在"。成功返回 true，失败把原因写进 err。
bool AddFeed(int ml, bool breast, const std::string &at_local, std::string *err);

// 记一次亲喂（按分钟）。美柚把亲喂存成 1_11，时长只认 start_at/end_at 区间，
// record_detail 里的任何时长字段都会被服务端丢弃（已实测）。
// at_local 是"喂完的那一刻"，start_at 会自动往前推 minutes 分钟。
bool AddNurse(int minutes, const std::string &at_local, std::string *err);

// 记一次大小便。kind: 0=尿(4_42) 1=便(4_43) 2=都有(两条)
bool AddDiaper(int kind, const std::string &at_local, std::string *err);

// 查上一次大小便（尿/便/未区分三类里取最近的）
BabyLastDiaper LastDiaper();

// 通用记录：record_type / record_category / record_detail(JSON 片段) 由调用方给。
// remark 会写进顶层备注（字段名未被逆向出来的类型靠它兜底）。
bool AddRecord(int record_type, int record_category, const std::string &detail_json,
               const std::string &remark, const std::string &at_local, std::string *err);

// 今日汇总 + 上次喂养（3 次 HTTPS，约 1-3 秒）
BabyToday Today();

// ---- 宝宝档案：编译期烧入的长期记忆 ----
std::string ProfileName();      // 宝宝小名，如「小宝」
std::string ProfileMemo();      // 家长留的长期记忆，如「对牛奶不过敏；爸爸叫…」
std::string ProfileBirthday();  // 生日 yyyy-MM-dd

// ---- 运行时记忆：写 NVS，掉电不丢 ----
bool MemoryAppend(const std::string &text, std::string *err);
std::string MemoryAll();

}  // namespace baby
