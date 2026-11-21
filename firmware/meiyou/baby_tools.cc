#include "baby_tools.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "mcp_server.h"
#include "meiyou_client.h"

static const char *TAG = "baby_tools";

namespace {

bool s_registered = false;

std::string json_dump(cJSON *o, const char *fallback) {
    char *s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : fallback;
    if (s) cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

std::string err_json(const std::string &err) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", err.c_str());
    return json_dump(o, "{\"ok\":false}");
}

// 今日汇总 + 上次喂养，一次请求就能给全，合并成一个工具省 token
std::string today_payload(const BabyToday &t) {
    cJSON *o = cJSON_CreateObject();
    if (!t.ok) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", t.error.c_str());
        return json_dump(o, "{}");
    }
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "today_ml", t.total_ml);
    cJSON_AddNumberToObject(o, "feed_count", t.count);
    cJSON_AddNumberToObject(o, "formula_ml", t.formula_ml);
    cJSON_AddNumberToObject(o, "breast_ml", t.breast_ml);

    if (t.last_at.empty()) {
        cJSON_AddBoolToObject(o, "has_last", false);
    } else {
        cJSON_AddBoolToObject(o, "has_last", true);
        cJSON_AddStringToObject(o, "last_at", t.last_at.c_str());
        if (t.last_is_nurse) {
            cJSON_AddStringToObject(o, "last_type", "nurse");  // 亲喂
            if (t.last_minutes > 0) cJSON_AddNumberToObject(o, "last_minutes", t.last_minutes);
        } else if (t.last_ml > 0) {
            cJSON_AddStringToObject(o, "last_type", "bottle");
            cJSON_AddNumberToObject(o, "last_ml", t.last_ml);
        }
        if (t.minutes_ago >= 0) {
            cJSON_AddNumberToObject(o, "minutes_ago", t.minutes_ago);
            cJSON_AddNumberToObject(o, "hours_ago", t.minutes_ago / 60);
        }
    }
    return json_dump(o, "{}");
}

std::string diaper_last_payload(const BabyLastDiaper &d) {
    cJSON *o = cJSON_CreateObject();
    if (!d.ok) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", d.error.c_str());
        return json_dump(o, "{}");
    }
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "has_record", d.has_record);
    if (d.has_record) {
        cJSON_AddStringToObject(o, "kind", d.kind.c_str());
        cJSON_AddStringToObject(o, "at", d.at.c_str());
        if (d.minutes_ago >= 0) {
            cJSON_AddNumberToObject(o, "minutes_ago", d.minutes_ago);
            cJSON_AddNumberToObject(o, "hours_ago", d.minutes_ago / 60);
        }
    }
    return json_dump(o, "{}");
}

}  // namespace

namespace baby_tools {

void Register() {
    if (s_registered) return;
    s_registered = true;

    auto &mcp = McpServer::GetInstance();

    // ---------------- 1. 记一笔喝奶（毫升 / 亲喂分钟） ----------------
    mcp.AddTool(
        "self.baby.feed_add",
        "记录宝宝喝奶。当用户说喝了多少奶、喂了奶、吃了多少毫升，或者亲喂了多久时调用。"
        "两种情况二选一："
        "① 瓶喂有毫升数：说「喝了150」「喝了一百五」就把 150 填进 ml；"
        "② 亲喂讲时长：说「亲喂了20分钟」「吃了半小时」「喂了15分钟」就把分钟数填进 minutes，"
        "这时候 type 必须是 breast，ml 留 0。"
        "type：配方奶/奶粉填 formula；母乳（无论是瓶装挤出来的还是亲喂）填 breast。"
        "at 是用户说的时间，没说就留空表示现在。",
        PropertyList({
            Property("ml", kPropertyTypeInteger, 0, 0, 500),
            Property("minutes", kPropertyTypeInteger, 0, 0, 180),
            Property("type", kPropertyTypeString, std::string("formula")),
            Property("at", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList &p) -> ReturnValue {
            int ml = p["ml"].value<int>();
            int minutes = p["minutes"].value<int>();
            std::string type = p["type"].value<std::string>();
            std::string at = p["at"].value<std::string>();
            bool breast = (type == "breast" || type == "母乳");

            std::string err;
            cJSON *o = cJSON_CreateObject();

            if (ml <= 0 && minutes <= 0) {
                ESP_LOGW(TAG, "feed_add 既没有 ml 也没有 minutes");
                return err_json("没听清喝了多少，是毫升数还是亲喂的分钟数？");
            }

            // 亲喂：只有分钟数，没有毫升数
            if (ml <= 0) {
                if (baby::AddNurse(minutes, at, &err)) {
                    cJSON_AddBoolToObject(o, "ok", true);
                    cJSON_AddStringToObject(o, "type", "nurse");
                    cJSON_AddNumberToObject(o, "minutes", minutes);
                    cJSON_AddStringToObject(o, "recorded_at",
                                            at.empty() ? baby::NowLocal().c_str() : at.c_str());
                    return json_dump(o, "{\"ok\":true}");
                }
                ESP_LOGW(TAG, "亲喂记录失败: %s", err.c_str());
                return err_json(err);
            }

            // 瓶喂：有毫升数（母乳走瓶喂母乳，配方奶走配方奶）
            if (baby::AddFeed(ml, breast, at, &err)) {
                cJSON_AddBoolToObject(o, "ok", true);
                cJSON_AddNumberToObject(o, "ml", ml);
                cJSON_AddStringToObject(o, "type", breast ? "breast" : "formula");
                cJSON_AddStringToObject(o, "recorded_at",
                                        at.empty() ? baby::NowLocal().c_str() : at.c_str());
                return json_dump(o, "{\"ok\":true}");
            }
            ESP_LOGW(TAG, "记录失败: %s", err.c_str());
            return err_json(err);
        });

    // ---------------- 2. 查今日汇总 + 上次（合并原 feed_today / feed_last） ----------------
    mcp.AddTool(
        "self.baby.feed_query",
        "查询宝宝的喝奶情况，一次给出：今天一共喝了多少毫升、喂了几次、配方奶和母乳各多少、"
        "以及上一次喝奶是什么时候、隔了多久、喝了多少或亲喂了多久。"
        "当用户问「今天喝了多少」「总共喝了多少」「喂了几次」「今天吃得好不好」"
        "「上次什么时候喂的」「多久没喂了」「该喂奶了吗」「距离上次多久」时调用。",
        PropertyList(),
        [](const PropertyList &) -> ReturnValue {
            return today_payload(baby::Today());
        });

    // ---------------- 3. 记换尿布 ----------------
    mcp.AddTool(
        "self.baby.diaper_add",
        "记录宝宝换尿布。当用户说换了尿布、尿了、拉臭臭、便便了、换片时调用。"
        "kind 取值：只是尿了传 pee，拉臭臭/拉大便传 poop，两样都有传 both。用户没说清就传 pee。"
        "at 是时间，用户没说就留空表示现在。",
        PropertyList({
            Property("kind", kPropertyTypeString, std::string("pee")),
            Property("at", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList &p) -> ReturnValue {
            std::string kind = p["kind"].value<std::string>();
            std::string at = p["at"].value<std::string>();

            int k = 0;
            if (kind == "poop" || kind == "便" || kind == "大便") k = 1;
            else if (kind == "both" || kind == "混合") k = 2;

            std::string err;
            if (baby::AddDiaper(k, at, &err)) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddBoolToObject(o, "ok", true);
                cJSON_AddStringToObject(o, "kind", kind.c_str());
                cJSON_AddStringToObject(o, "recorded_at",
                                        at.empty() ? baby::NowLocal().c_str() : at.c_str());
                return json_dump(o, "{\"ok\":true}");
            }
            ESP_LOGW(TAG, "换尿布记录失败: %s", err.c_str());
            return err_json(err);
        });

    // ---------------- 4. 上次换尿布 ----------------
    mcp.AddTool(
        "self.baby.diaper_last",
        "查询宝宝上一次换尿布是什么时候、隔了多久、是尿还是拉臭臭。"
        "当用户问「上次换尿布什么时候」「多久没换尿布了」「尿布该换了吗」「上次拉臭臭是什么时候」时调用。",
        PropertyList(),
        [](const PropertyList &) -> ReturnValue {
            return diaper_last_payload(baby::LastDiaper());
        });

    // ---------------- 5. 辅食 / 营养补剂 ----------------
    mcp.AddTool(
        "self.baby.food_add",
        "记录宝宝吃进去的东西：辅食和营养补剂。"
        "当用户说吃了米糊、米粉、果泥、粥、蛋黄、面条，或者吃了维生素、AD、D3、钙、益生菌时调用。"
        "type 取值：辅食/米糊/果泥/粥这类吃的传 food；维生素/AD/D3/钙/益生菌/营养品传 supplement。"
        "name 写具体名称，尽量按用户原话，例如「90米糊加香蕉」「维生素AD」「D3」。"
        "at 是时间，用户没说就留空表示现在。",
        PropertyList({
            Property("type", kPropertyTypeString, std::string("food")),
            Property("name", kPropertyTypeString, std::string("")),
            Property("at", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList &p) -> ReturnValue {
            std::string type = p["type"].value<std::string>();
            std::string name = p["name"].value<std::string>();
            std::string at = p["at"].value<std::string>();

            bool sup = (type == "supplement" || type == "vitamin" || type == "补剂");
            int rt = sup ? 11 : 3;

            cJSON *d = cJSON_CreateObject();
            if (sup) {
                if (!name.empty()) {
                    cJSON *arr = cJSON_AddArrayToObject(d, "nutritional_supplements");
                    cJSON *it = cJSON_CreateObject();
                    cJSON_AddStringToObject(it, "name", name.c_str());
                    cJSON_AddItemToArray(arr, it);
                }
            } else if (!name.empty()) {
                cJSON_AddStringToObject(d, "food_name", name.c_str());
            }
            std::string detail = json_dump(d, "{}");

            std::string err;
            bool ok = baby::AddRecord(rt, 0, detail, name, at, &err);

            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", ok);
            cJSON_AddStringToObject(o, "type", sup ? "supplement" : "food");
            if (!name.empty()) cJSON_AddStringToObject(o, "name", name.c_str());
            cJSON_AddStringToObject(o, "recorded_at",
                                    at.empty() ? baby::NowLocal().c_str() : at.c_str());
            if (!ok) {
                ESP_LOGW(TAG, "辅食/补剂记录失败: %s", err.c_str());
                cJSON_AddStringToObject(o, "error", err.c_str());
            }
            return json_dump(o, "{}");
        });

    // ---------------- 6. 日常事件：洗澡 / 睡眠 / 玩耍 / 游泳 / 其他 ----------------
    mcp.AddTool(
        "self.baby.event_add",
        "记录宝宝的其他日常事件：洗澡、睡觉、玩耍、游泳、以及其它说不上分类的事。"
        "type 取值：洗澡/泡澡传 bath；睡觉/小睡/午睡/睡着了传 sleep；"
        "玩/玩耍/做游戏传 play；游泳/下水传 swim；其它情况传 other。"
        "name 是可选补充说明，用户提到细节时才填，例如「洗头也洗了」「睡了两个小时」。"
        "at 是时间，用户没说就留空表示现在。",
        PropertyList({
            Property("type", kPropertyTypeString, std::string("other")),
            Property("name", kPropertyTypeString, std::string("")),
            Property("at", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList &p) -> ReturnValue {
            std::string type = p["type"].value<std::string>();
            std::string name = p["name"].value<std::string>();
            std::string at = p["at"].value<std::string>();

            // record_type 对照美柚 /v3/life/record/types：2 睡眠 5 洗澡 6 玩耍 7 游泳 8 其他
            int rt = 8;
            if (type == "bath" || type == "洗澡") rt = 5;
            else if (type == "sleep" || type == "睡觉") rt = 2;
            else if (type == "play" || type == "玩耍") rt = 6;
            else if (type == "swim" || type == "游泳") rt = 7;

            std::string err;
            bool ok = baby::AddRecord(rt, 0, "{}", name, at, &err);

            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", ok);
            cJSON_AddStringToObject(o, "type", type.c_str());
            cJSON_AddStringToObject(o, "recorded_at",
                                    at.empty() ? baby::NowLocal().c_str() : at.c_str());
            if (!ok) {
                ESP_LOGW(TAG, "事件记录失败 type=%s: %s", type.c_str(), err.c_str());
                cJSON_AddStringToObject(o, "error", err.c_str());
            }
            return json_dump(o, "{}");
        });

    // ---------------- 7. 记忆：档案 + 长期记忆（合并原 profile / remember） ----------------
    // 档案内容直接拼进工具描述，模型每轮都能看到，正常对话不需要主动调用。
    std::string pname = baby::ProfileName();
    std::string pmemo = baby::ProfileMemo();
    std::string pbirth = baby::ProfileBirthday();

    std::string mdesc = "宝宝档案与长期记忆。";
    if (!pname.empty() || !pmemo.empty() || !pbirth.empty()) {
        mdesc += "已知信息：";
        if (!pname.empty()) mdesc += "宝宝小名叫「" + pname + "」；";
        if (!pbirth.empty()) mdesc += "生日 " + pbirth + "；";
        if (!pmemo.empty()) mdesc += pmemo + "；";
        mdesc += "上面这些可以直接用，不用调用工具。";
    }
    mdesc +=
        "调用时机有两种："
        "① 用户问「他多大了」「几个月了」「你还记得什么」「我之前跟你说过什么」，"
        "或者要查已存的长期记忆时，action 传 read；"
        "② 用户明确让你记住一件事（说「记住」「记一下」「以后都……」「别忘了」）时，"
        "action 传 add，text 用一句完整的话概括，例如「对牛奶蛋白不过敏」。";

    mcp.AddTool(
        "self.baby.memory",
        mdesc,
        PropertyList({
            Property("action", kPropertyTypeString, std::string("read")),
            Property("text", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList &p) -> ReturnValue {
            std::string action = p["action"].value<std::string>();
            std::string text = p["text"].value<std::string>();

            // 写：追加一条长期记忆到 NVS
            if (action == "add" || action == "write" || action == "记住") {
                std::string err;
                if (!baby::MemoryAppend(text, &err)) return err_json(err);
                cJSON *o = cJSON_CreateObject();
                cJSON_AddBoolToObject(o, "ok", true);
                cJSON_AddStringToObject(o, "saved", text.c_str());
                cJSON_AddStringToObject(o, "say", "好，我记住了");
                return json_dump(o, "{\"ok\":true}");
            }

            // 读：档案 + 年龄 + NVS 记忆
            cJSON *o = cJSON_CreateObject();
            std::string nm = baby::ProfileName();
            std::string bd = baby::ProfileBirthday();
            std::string mm = baby::ProfileMemo();
            cJSON_AddBoolToObject(o, "ok", true);

            if (!nm.empty()) cJSON_AddStringToObject(o, "name", nm.c_str());
            if (!bd.empty()) {
                cJSON_AddStringToObject(o, "birthday", bd.c_str());
                int y = 0, m = 0, d = 0;
                if (sscanf(bd.c_str(), "%d-%d-%d", &y, &m, &d) == 3) {
                    struct tm tmv;
                    memset(&tmv, 0, sizeof(tmv));
                    tmv.tm_year = y - 1900;
                    tmv.tm_mon = m - 1;
                    tmv.tm_mday = d;
                    tmv.tm_isdst = -1;
                    time_t bt = mktime(&tmv);
                    if (bt > 0) {
                        long days = (long)((time(nullptr) - bt) / 86400);
                        if (days >= 0) {
                            cJSON_AddNumberToObject(o, "age_days", (int)days);
                            cJSON_AddNumberToObject(o, "age_months", (int)(days / 30));
                        }
                    }
                }
            }
            if (!mm.empty()) cJSON_AddStringToObject(o, "memo", mm.c_str());

            std::string mem = baby::MemoryAll();
            if (!mem.empty()) cJSON_AddStringToObject(o, "memories", mem.c_str());
            return json_dump(o, "{}");
        });

    ESP_LOGI(TAG, "已注册 7 个工具：喝奶/汇总/尿布记/尿布查/辅食/事件/记忆");
}

}  // namespace baby_tools
