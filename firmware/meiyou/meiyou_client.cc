// 美柚宝宝记 —— ESP32 直连实现（不经过任何中间服务器）
#include "meiyou_client.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "settings.h"

// HAS_MEIYOU_CONFIG 由 CMake 根据 meiyou_config.h 是否存在来定义。
// 不用 __has_include：它把「文件不存在」写进 .d 依赖表后，补上配置也不会重编。
#ifdef HAS_MEIYOU_CONFIG
#include "meiyou_config.h"
#else
#warning "缺少 meiyou_config.h —— 请用 PC 绑定助手导出后放进 main/meiyou/"
#define MYY_AUTHORIZATION ""
#define MYY_BBID 0
#define MYY_BBDAY ""
#define MYY_COMMON_BABY_ID 0
#define MYY_BABY_ID 0
#define MYY_EXPIRES_AT 0
#define MYY_API_BASE "https://api-bbj.meiyou.com"
#endif

#ifndef MYY_API_BASE
#define MYY_API_BASE "https://api-bbj.meiyou.com"
#endif

static const char *TAG = "baby";

namespace {

uint32_t s_rid = 0;

uint32_t next_rid() {
    if (s_rid == 0) {
        time_t now = time(nullptr);
        s_rid = (uint32_t)(now % 90000) + 1000;
    }
    return ++s_rid;
}

esp_err_t on_http_data(esp_http_client_event_t *e) {
    auto *acc = static_cast<std::string *>(e->user_data);
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0 && acc) {
        acc->append(static_cast<const char *>(e->data), e->data_len);
    }
    return ESP_OK;
}

// 返回 HTTP 状态码；网络层失败返回负数，错误描述写进 err
int http_call(esp_http_client_method_t method, const std::string &url,
              const std::string &body, std::string *resp, std::string *err) {
    if (resp) resp->clear();

    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url.c_str();
    cfg.method = method;
    cfg.event_handler = on_http_data;
    cfg.user_data = resp;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 12000;
    cfg.buffer_size = 2048;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        if (err) *err = "无法建立 HTTPS 连接";
        return -1;
    }

    // 美柚 App 的固定协议头（与账号无关的部分）
    esp_http_client_set_header(c, "accept", "*/*");
    esp_http_client_set_header(c, "user-agent", "okhttp/4.9.0");
    esp_http_client_set_header(c, "platform", "android");
    esp_http_client_set_header(c, "v", "9.0.9");
    esp_http_client_set_header(c, "v1", "9.09.0.0");
    esp_http_client_set_header(c, "version", "9.0.9");
    esp_http_client_set_header(c, "mode", "3");
    esp_http_client_set_header(c, "myclient", "0130909016900000");
    esp_http_client_set_header(c, "myappinfo", "01-3-9.09.0.0-0169-0");
    esp_http_client_set_header(c, "authorization", MYY_AUTHORIZATION);

    char bbid[24];
    snprintf(bbid, sizeof(bbid), "%d", (int)MYY_BBID);
    esp_http_client_set_header(c, "bbid", bbid);
    esp_http_client_set_header(c, "bbday", MYY_BBDAY);

    if (!body.empty()) {
        esp_http_client_set_header(c, "content-type", "application/json; charset=utf-8");
        esp_http_client_set_post_field(c, body.c_str(), (int)body.size());
    }

    esp_err_t ret = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "请求失败 %s: %s", url.c_str(), esp_err_to_name(ret));
        if (err) *err = "网络不通，连不上美柚（检查 WiFi 或稍后再试）";
        return -2;
    }
    return status;
}

// 解析 JSON 并检查业务 code。成功时 *root 由调用方 cJSON_Delete
bool parse_ok(const std::string &body, cJSON **root, std::string *err) {
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j) {
        if (err) *err = "美柚返回了无法解析的内容";
        return false;
    }
    cJSON *code = cJSON_GetObjectItem(j, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        cJSON *msg = cJSON_GetObjectItem(j, "message");
        std::string m = (msg && cJSON_IsString(msg)) ? msg->valuestring : "美柚接口返回错误";
        if (code->valueint == 401 || code->valueint == 11000401 ||
            code->valueint == 11000400) {
            m += "（登录已过期，需要重新用 PC 助手绑定一次）";
        }
        if (err) *err = m;
        cJSON_Delete(j);
        return false;
    }
    *root = j;
    return true;
}

bool config_ready(std::string *err) {
    if (MYY_COMMON_BABY_ID == 0 || strlen(MYY_AUTHORIZATION) < 8) {
        if (err) *err = "还没有绑定美柚账号，请先用 PC 绑定助手导出配置并重新烧录";
        return false;
    }
    return true;
}

// "yyyy-MM-dd HH:mm:ss" -> time_t（系统时间本身就是本地时基，直接按同一基准解释）
time_t parse_local(const char *s) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday,
                     &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) != 6) {
        return 0;
    }
    tmv.tm_year -= 1900;
    tmv.tm_mon -= 1;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

std::string url_encode_time(const std::string &s) {
    // 只需要处理空格，时间格式里没有别的特殊字符
    std::string out;
    for (char ch : s) {
        if (ch == ' ') out += "%20";
        else out += ch;
    }
    return out;
}

// 联网时间是否已同步（OTA 会给系统对时）
bool time_ready() {
    return time(nullptr) > 1700000000;  // 晚于 2023-11 说明已同步
}

}  // namespace

namespace baby {

std::string NowLocal() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

bool TokenExpired() {
    if (!time_ready()) return false;
    if (MYY_EXPIRES_AT == 0) return false;
    return time(nullptr) > (time_t)MYY_EXPIRES_AT;
}

namespace {

// 所有记录类型共用的写入通道：只拼一次 body、一次 HTTPS
// end_override 非空时写进 end_at（亲喂靠 start_at→end_at 区间表示时长）
bool write_record(int record_type, int record_category, const std::string &detail_json,
                  const std::string &remark, const std::string &at_local, std::string *err,
                  const std::string &end_override = "") {
    if (!config_ready(err)) return false;
    if (TokenExpired()) {
        if (err) *err = "美柚登录已过期，需要用 PC 助手重新绑定并重烧固件";
        return false;
    }
    if (!time_ready()) {
        if (err) *err = "设备时间还没同步，暂时不能记录（等联网后重试）";
        return false;
    }

    std::string when = at_local.empty() ? NowLocal() : at_local;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "baby_id", MYY_BABY_ID);
    cJSON_AddNumberToObject(root, "client_rid", next_rid());
    cJSON_AddNumberToObject(root, "common_baby_id", MYY_COMMON_BABY_ID);
    cJSON_AddNumberToObject(root, "draft_id", 0);
    cJSON_AddNumberToObject(root, "from", 0);
    cJSON_AddStringToObject(root, "remark", remark.c_str());
    cJSON_AddNumberToObject(root, "time_state", 0);
    cJSON_AddNumberToObject(root, "type", 0);
    cJSON_AddBoolToObject(root, "updateTips", false);
    cJSON_AddNumberToObject(root, "record_category", record_category);
    cJSON_AddNumberToObject(root, "record_type", record_type);

    if (!detail_json.empty()) {
        cJSON *d = cJSON_Parse(detail_json.c_str());
        if (cJSON_IsObject(d)) {
            cJSON_AddItemToObject(root, "record_detail", d);
        } else {
            if (d) cJSON_Delete(d);
            cJSON_AddObjectToObject(root, "record_detail");
        }
    } else {
        cJSON_AddObjectToObject(root, "record_detail");
    }

    cJSON_AddStringToObject(root, "start_at", when.c_str());
    if (!end_override.empty()) {
        cJSON_AddStringToObject(root, "end_at", end_override.c_str());
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    std::string url = std::string(MYY_API_BASE) + "/v3/life/record";
    std::string resp, err2;
    int status = http_call(HTTP_METHOD_POST, url, body ? body : "", &resp, &err2);
    if (body) cJSON_free(body);

    if (status < 0) {
        if (err) *err = err2;
        return false;
    }
    if (status != 200) {
        if (err) *err = "美柚拒绝了这次记录（HTTP " + std::to_string(status) + "）";
        return false;
    }

    cJSON *jr = nullptr;
    if (!parse_ok(resp, &jr, err)) return false;
    cJSON_Delete(jr);
    ESP_LOGI(TAG, "已记录 type=%d cat=%d @%s", record_type, record_category, when.c_str());
    return true;
}

}  // namespace

bool AddFeed(int ml, bool breast, const std::string &at_local, std::string *err) {
    if (ml <= 0 || ml > 500) {
        if (err) *err = "奶量看起来不对，应该在 1 到 500 毫升之间";
        return false;
    }
    // 13=配方奶  12=瓶喂母乳（要填毫升数，所以走瓶喂这一支）
    std::string detail = breast
        ? ("{\"bottle_breast_capacity\":" + std::to_string(ml) + "}")
        : ("{\"bottle_feed_capacity\":" + std::to_string(ml) + "}");
    return write_record(1, breast ? 12 : 13, detail, "", at_local, err);
}

bool AddNurse(int minutes, const std::string &at_local, std::string *err) {
    if (minutes <= 0 || minutes > 180) {
        if (err) *err = "亲喂时长应该在 1 到 180 分钟之间";
        return false;
    }
    std::string end_s = at_local.empty() ? NowLocal() : at_local;
    time_t end_t = parse_local(end_s.c_str());
    if (end_t <= 0) {
        if (err) *err = "时间格式不对，应该是 yyyy-MM-dd HH:mm:ss";
        return false;
    }
    time_t start_t = end_t - (time_t)minutes * 60;
    struct tm tmv;
    localtime_r(&start_t, &tmv);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);

    // 美柚亲喂：record_type=1 record_category=11，
    // 时长只认 start_at→end_at 区间，record_detail 传空（实测其他键都会被丢弃）。
    return write_record(1, 11, "", "", std::string(buf), err, end_s);
}

bool AddDiaper(int kind, const std::string &at_local, std::string *err) {
    // 实测：美柚原生把「尿」记为 4_42、「便」记为 4_43，
    // 旧的实现用固定 42 + shit_shape 区分，结果拉臭臭在 App 里显示成换尿布。
    if (kind < 0 || kind > 2) kind = 0;
    bool ok = true;
    if (kind == 0 || kind == 2) {
        ok = write_record(4, 42, "", "", at_local, err) && ok;
    }
    if (kind == 1 || kind == 2) {
        ok = write_record(4, 43,
                          "{\"shit_color\":0,\"shit_shape\":0,\"shit_photo\":[],\"red_buttock\":false}",
                          "", at_local, err) && ok;
    }
    return ok;
}

bool AddRecord(int record_type, int record_category, const std::string &detail_json,
               const std::string &remark, const std::string &at_local, std::string *err) {
    return write_record(record_type, record_category, detail_json, remark, at_local, err);
}

BabyLastDiaper LastDiaper() {
    BabyLastDiaper out;
    if (!config_ready(&out.error)) return out;
    if (!time_ready()) {
        out.error = "设备时间还没同步，等联网后再查";
        return out;
    }

    // 41=尿 42=尿 43=便 0=未区分。实测美柚当前的尿是 42、便是 43，41 是老版本。
    struct { const char *uni; const char *kind; } qs[] = {
        {"4_43", "poop"}, {"4_42", "pee"}, {"4_41", "pee"}, {"4_0", "unknown"},
    };

    std::string best_at;
    std::string best_kind;

    for (const auto &q : qs) {
        // 已经拿到明确的尿/便就不用再问「未区分」那一档了
        if (!best_at.empty() && std::string(q.kind) == "unknown") break;

        std::string url = std::string(MYY_API_BASE) + "/v3/life/record/recent?common_baby_id=" +
                          std::to_string((int)MYY_COMMON_BABY_ID) + "&type_unis=" + q.uni;
        std::string resp, err2;
        if (http_call(HTTP_METHOD_GET, url, "", &resp, &err2) != 200) continue;

        cJSON *jr = nullptr;
        if (!parse_ok(resp, &jr, &err2)) continue;

        cJSON *list = cJSON_GetObjectItem(cJSON_GetObjectItem(jr, "data"), "list");
        cJSON *grp = nullptr;
        cJSON_ArrayForEach(grp, list) {
            cJSON *recs = cJSON_GetObjectItem(grp, "records");
            cJSON *r = cJSON_GetArrayItem(recs, 0);
            if (!r) continue;
            cJSON *st = cJSON_GetObjectItem(r, "start_at");
            if (cJSON_IsString(st) &&
                (best_at.empty() || strcmp(st->valuestring, best_at.c_str()) > 0)) {
                best_at = st->valuestring;
                best_kind = q.kind;
            }
            break;  // 接口已按时间倒序，第一条就是最新的
        }
        cJSON_Delete(jr);
    }

    if (best_at.empty()) {
        out.ok = true;
        out.has_record = false;
        return out;
    }

    out.ok = true;
    out.has_record = true;
    out.at = best_at;
    out.kind = best_kind;
    time_t last = parse_local(best_at.c_str());
    if (last > 0) {
        long diff = (long)(time(nullptr) - last);
        out.minutes_ago = diff >= 0 ? (int)(diff / 60) : 0;
    }
    return out;
}

BabyToday Today() {
    BabyToday out;
    if (!config_ready(&out.error)) return out;
    if (!time_ready()) {
        out.error = "设备时间还没同步，等联网后再查";
        return out;
    }

    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", &tmv);

    // 1) 拉今日全部记录，一次算出总量 + 今日最后一次
    std::string url = std::string(MYY_API_BASE) + "/v3/life/record/list?client_rtime=" +
                      url_encode_time(NowLocal()) + "&client_rid=" +
                      std::to_string(next_rid()) + "&size=100&common_baby_id=" +
                      std::to_string((int)MYY_COMMON_BABY_ID);

    std::string resp, err2;
    int status = http_call(HTTP_METHOD_GET, url, "", &resp, &err2);
    if (status < 0) {
        out.error = err2;
        return out;
    }
    if (status != 200) {
        out.error = "读取今日记录失败（HTTP " + std::to_string(status) + "）";
        return out;
    }

    cJSON *root = nullptr;
    if (!parse_ok(resp, &root, &out.error)) return out;

    std::string best_at;  // 今日最后一次喝奶的时间
    int best_ml = 0;
    int best_minutes = 0;
    bool best_nurse = false;

    cJSON *list = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "list");
    cJSON *grp = nullptr;
    cJSON_ArrayForEach(grp, list) {
        cJSON *recs = cJSON_GetObjectItem(grp, "records");
        cJSON *r = nullptr;
        cJSON_ArrayForEach(r, recs) {
            cJSON *type = cJSON_GetObjectItem(r, "record_type");
            if (!cJSON_IsNumber(type) || type->valueint != 1) continue;

            cJSON *st = cJSON_GetObjectItem(r, "start_at");
            if (!cJSON_IsString(st) || strncmp(st->valuestring, today, 10) != 0) continue;

            cJSON *q = cJSON_GetObjectItem(r, "quantity");
            int ml = cJSON_IsNumber(q) ? q->valueint : 0;
            cJSON *cat = cJSON_GetObjectItem(r, "record_category");
            int c = cJSON_IsNumber(cat) ? cat->valueint : 13;

            out.count++;
            out.total_ml += ml;
            if (c == 12) out.breast_ml += ml; else out.formula_ml += ml;

            if (best_at.empty() || strcmp(st->valuestring, best_at.c_str()) > 0) {
                best_at = st->valuestring;
                best_ml = ml;
                best_nurse = (c == 11);
                best_minutes = 0;
                // 亲喂的时长存在 start_at→end_at 区间里
                if (best_nurse) {
                    cJSON *et = cJSON_GetObjectItem(r, "end_at");
                    if (cJSON_IsString(et) && et->valuestring[0]) {
                        time_t s = parse_local(st->valuestring);
                        time_t e = parse_local(et->valuestring);
                        if (s > 0 && e > s) best_minutes = (int)((e - s) / 60);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);

    out.last_at = best_at;
    out.last_ml = best_ml;
    out.last_minutes = best_minutes;
    out.last_is_nurse = best_nurse;

    // 2) 今天还没喝过：查最近一次（配方奶 + 瓶喂母乳 + 亲喂三个口径，取较晚的）
    if (best_at.empty()) {
        const char *unis[] = {"1_13", "1_12", "1_11"};
        for (const char *u : unis) {
            std::string ru = std::string(MYY_API_BASE) + "/v3/life/record/recent?common_baby_id=" +
                             std::to_string((int)MYY_COMMON_BABY_ID) + "&type_unis=" + u;
            std::string r2, e2;
            if (http_call(HTTP_METHOD_GET, ru, "", &r2, &e2) != 200) continue;

            cJSON *jr = nullptr;
            if (!parse_ok(r2, &jr, &e2)) continue;

            cJSON *jlist = cJSON_GetObjectItem(cJSON_GetObjectItem(jr, "data"), "list");
            cJSON *jg = nullptr;
            cJSON_ArrayForEach(jg, jlist) {
                cJSON *recs = cJSON_GetObjectItem(jg, "records");
                cJSON *r = cJSON_GetArrayItem(recs, 0);
                if (!r) continue;
                cJSON *st = cJSON_GetObjectItem(r, "start_at");
                cJSON *q = cJSON_GetObjectItem(r, "quantity");
                if (cJSON_IsString(st) &&
                    (out.last_at.empty() || strcmp(st->valuestring, out.last_at.c_str()) > 0)) {
                    out.last_at = st->valuestring;
                    out.last_ml = cJSON_IsNumber(q) ? q->valueint : 0;
                    out.last_is_nurse = (u[2] == '1' && u[3] == '1');
                    out.last_minutes = 0;
                    if (out.last_is_nurse) {
                        cJSON *et = cJSON_GetObjectItem(r, "end_at");
                        if (cJSON_IsString(et) && et->valuestring[0]) {
                            time_t s = parse_local(st->valuestring);
                            time_t e = parse_local(et->valuestring);
                            if (s > 0 && e > s) out.last_minutes = (int)((e - s) / 60);
                        }
                    }
                }
                break;  // 接口已按时间倒序
            }
            cJSON_Delete(jr);
        }
    }

    if (!out.last_at.empty()) {
        time_t last = parse_local(out.last_at.c_str());
        if (last > 0) {
            long diff = (long)(now - last);
            out.minutes_ago = diff >= 0 ? (int)(diff / 60) : 0;
        }
    }

    out.ok = true;
    return out;
}

// ---------------- 宝宝档案（编译期烧入的长期记忆） ----------------

std::string ProfileName() {
#ifdef MYY_BABY_NAME
    return MYY_BABY_NAME;
#else
    return "";
#endif
}

std::string ProfileMemo() {
#ifdef MYY_BABY_MEMO
    return MYY_BABY_MEMO;
#else
    return "";
#endif
}

std::string ProfileBirthday() {
#ifdef MYY_BABY_BIRTHDAY
    return MYY_BABY_BIRTHDAY;
#else
    return "";
#endif
}

// ---------------- 运行时记忆（NVS，掉电不丢） ----------------

namespace {
constexpr const char *kMemNs = "baby_mem";
constexpr const char *kMemKey = "memo";
constexpr size_t kMemMax = 1200;  // 约 400 个汉字，超了从最旧的条目开始丢
}  // namespace

bool MemoryAppend(const std::string &text, std::string *err) {
    if (text.empty()) {
        if (err) *err = "要记的内容是空的";
        return false;
    }
    Settings st(kMemNs, true);
    std::string cur = st.GetString(kMemKey, "");
    std::string next = cur.empty() ? ("- " + text) : (cur + "\n- " + text);
    while (next.size() > kMemMax) {
        size_t p = next.find('\n');
        if (p == std::string::npos) break;
        next = next.substr(p + 1);
    }
    st.SetString(kMemKey, next);
    ESP_LOGI(TAG, "记忆已追加（%u 字节）", (unsigned)next.size());
    return true;
}

std::string MemoryAll() {
    Settings st(kMemNs, false);
    return st.GetString(kMemKey, "");
}

}  // namespace baby
