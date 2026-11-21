# -*- coding: utf-8 -*-
"""
ESP32 美柚助手（个人项目，与美柚、小智官方无关）

对着小智 AI 说一句「刚喝了 120 毫升」，它就替你记进美柚宝宝记。
本程序负责把账号和小智板子对接起来：

前提（第 0 步）：手机上装好美柚 App，注册账号并建好宝宝档案，且自己是「宝宝记」成员。
之后：手机号 → 短信验证码 → 自动读到账号下的宝宝（不需要选）→
填两个「选填」的档案字段 → 点一下按钮，直接把 meiyou_config.h 写进固件目录。

页面本身就是说明书：写入成功后会给出一段可以直接复制给 AI 助手（WorkBuddy 等）的话，
让它接着编译烧录；再往下是烧完之后的配网、激活步骤。

零第三方依赖（纯标准库），手机号加密见 aes_cbc.py。

运行：python server.py
访问：电脑 http://127.0.0.1:8899　手机（同一 WiFi）http://<电脑IP>:8899
"""
import json
import os
import random
import socket
import ssl
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import aes_cbc

PORT = int(os.environ.get("PORT", "8899"))
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_FILE = os.path.join(BASE_DIR, "config.json")
LOG_FILE = os.path.join(BASE_DIR, "trace.log")

HOST_USER = "users.seeyouyima.com"
HOST_BBJ = "api-bbj.meiyou.com"

# 美柚 App 的固定协议头（与账号无关，所有设备一致）
STATIC_HEADERS = {
    "accept": "*/*",
    "content-type": "application/json; charset=utf-8",
    "myclient": "0130909016900000",
    "myappinfo": "01-3-9.09.0.0-0169-0",
    "v1": "9.09.0.0",
    "v": "9.0.9",
    "version": "9.0.9",
    "platform": "android",
    "mode2": "3",
    "linggan_access_info": "NTk0NzQ0MTM1XzU5NDc0NDEzNV8yN18wMS0zLTkuMDkuMC4wLTAxNjktMA==",
    "sv": "KK77",
    "isol": "1169-2-80-1",
    "is-em": "1000300-/sys/module/vboxguest",
    "open-person-ad": "1",
    "open-person-eb-recomm": "1",
    "themeid1": "com.meetyou.skin.mrzt",
}
UA = ("com.lingan.seeyou/9.09.0.0 MeetYouClient/2.0.0 (0130909016900000) "
      "MYAPPINFO(01-3-9.09.0.0-0169-0)")

SSL_CTX = ssl.create_default_context()
_lock = threading.Lock()
trace_buf = []


# ---------------- 配置持久化 ----------------
def new_mac():
    """每台机器一个独立的设备标识，避免所有人共用同一个指纹"""
    return "".join(random.choice("0123456789abcdef") for _ in range(16))


CFG = {
    "device": {"mac": new_mac()},
    "account": {},          # phone / user_id / authorization / refresh_token / expires_at
    "baby": {},             # common_baby_id / baby_id / nickname / birthday
    "babies": [],           # 最近一次登录拉到的宝宝列表
    "runtime": {"uuid": ""},  # suspicious 下发的 uuid，每次登录刷新
    "profile": {},          # name / memo，编译进固件的宝宝档案（长期记忆）
}

_dirty = False


def load_cfg():
    global CFG
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, encoding="utf-8") as f:
                j = json.load(f)
            for k in ("device", "account", "baby", "babies", "runtime", "profile"):
                if isinstance(j.get(k), dict) or isinstance(j.get(k), list):
                    CFG[k] = j[k]
            CFG.setdefault("device", {})
            if not CFG["device"].get("mac"):
                CFG["device"]["mac"] = new_mac()
        except Exception as e:
            log("CFG", "读取失败，使用默认值：" + str(e))
    else:
        save_cfg()


def save_cfg():
    tmp = CONFIG_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(CFG, f, ensure_ascii=False, indent=2)
    os.replace(tmp, CONFIG_FILE)


def log(kind, msg):
    line = "[%s] %-7s %s" % (time.strftime("%H:%M:%S"), kind, msg)
    print(line, flush=True)
    with _lock:
        trace_buf.append(line)
        if len(trace_buf) > 400:
            del trace_buf[:80]
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


# ---------------- HTTP ----------------
def build_headers(authz="", mode=0):
    h = dict(STATIC_HEADERS)
    h["mode"] = str(mode)
    h["user-agent"] = UA
    h["session-id"] = str(int(time.time() * 1000))
    if authz:
        h["authorization"] = authz
    return h


def http_call(host, path, method="GET", body=None, headers=None, timeout=20):
    url = "https://%s%s" % (host, path)
    data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=SSL_CTX) as r:
            raw = r.read().decode("utf-8", "replace")
            return {"ok": True, "status": r.status, "raw": raw, "json": _json(raw)}
    except urllib.error.HTTPError as e:
        raw = ""
        try:
            raw = e.read().decode("utf-8", "replace")
        except Exception:
            pass
        return {"ok": False, "status": e.code, "raw": raw, "json": _json(raw)}
    except Exception as e:
        return {"ok": False, "status": 0, "raw": "", "json": None,
                "error": "%s: %s" % (type(e).__name__, e)}


def _json(s):
    try:
        return json.loads(s)
    except Exception:
        return None


# ---------------- 美柚接口 ----------------
def precheck(phone):
    """风控预检，返回新 uuid；pred=0 表示无需图形验证"""
    q = urllib.parse.urlencode({"phone": phone, "nation_code": "86", "op_type": 1})
    r = http_call(HOST_USER, "/v3/suspicious?" + q, "GET", None, build_headers())
    try:
        d = (r.get("json") or {}).get("data") or {}
        CFG["runtime"]["uuid"] = str(d.get("uuid") or "")
        CFG["runtime"]["pred"] = d.get("pred")
        save_cfg()
    except Exception:
        pass
    return r


def send_sms(phone):
    body = {"phone": aes_cbc.encrypt_phone(phone), "nation_code": "86",
            "type": "verify_code_login"}
    return http_call(HOST_USER, "/v2/sms_code?v1=9.09.0.0", "POST", body,
                     build_headers())


def login(phone, code):
    body = {"account": aes_cbc.encrypt_phone(phone), "authnum": str(code),
            "mac_address": CFG["device"]["mac"], "nation_code": "86",
            "platform": "verify_code_logins",
            "suspicious_uuid": CFG["runtime"].get("uuid") or ""}
    path = "/v2/logins?mode=0&app_id=1&v1=9.09.0.0&v=9.0.9&platform=android"
    return http_call(HOST_USER, path, "POST", body, build_headers())


def fetch_babies(authz):
    r = http_call(HOST_BBJ, "/v3/babies", "GET", None, build_headers(authz, mode=3))
    out = []
    try:
        for b in ((r.get("json") or {}).get("data") or {}).get("list") or []:
            out.append({
                "common_baby_id": b.get("common_baby_id"),
                "baby_id": b.get("bbj_baby_id"),
                "nickname": b.get("nickname") or "宝宝",
                "birthday": b.get("birthday") or "",
                "sex": b.get("sex"),
                "relation": b.get("relation_name") or "",
                "can_record": bool(((b.get("permission") or {}).get("can_feed_record"))),
            })
    except Exception:
        pass
    return r, out


def verify(authz, baby):
    """用宝宝记接口验活：能拉到最后一次喂奶即视为可用"""
    h = build_headers(authz, mode=3)
    h["bbid"] = str(baby.get("common_baby_id") or "")
    h["bbday"] = (baby.get("birthday") or "").replace("-", "")
    q = urllib.parse.urlencode({"common_baby_id": baby.get("common_baby_id"),
                                "type_unis": "1_13"})
    r = http_call(HOST_BBJ, "/v3/life/record/recent?" + q, "GET", None, h)
    ok = bool(r.get("ok") and (r.get("json") or {}).get("code") == 0)
    return r, ok


# ---------------- 业务动作 ----------------
def do_send_code(phone):
    phone = (phone or "").strip()
    if not (phone.isdigit() and len(phone) == 11):
        return {"ok": False, "error": "手机号需为 11 位数字"}
    pc = precheck(phone)
    log("SMS", "预检 %s → HTTP %s pred=%s" % (phone, pc.get("status"), CFG["runtime"].get("pred")))
    r = send_sms(phone)
    j = r.get("json") or {}
    ok = bool(r.get("ok") and j.get("code") == 0)
    log("SMS", "发码 %s → %s" % (phone, "成功" if ok else (r.get("raw") or "")[:90]))
    return {"ok": ok, "status": r.get("status"),
            "message": j.get("message") or "", "code": j.get("code"),
            "cooldown": (j.get("data") or {}).get("time") if isinstance(j.get("data"), dict) else None,
            "raw": None if ok else (r.get("raw") or r.get("error") or "")[:300]}


def do_login(phone, code):
    phone = (phone or "").strip()
    code = (code or "").strip()
    if not code:
        return {"ok": False, "error": "请输入验证码"}
    r = login(phone, code)
    j = r.get("json") or {}
    if not isinstance(j, dict):
        j = {}
    d = j.get("data") if isinstance(j.get("data"), dict) else {}

    def pick(*keys):
        """美柚不同版本把 token 放在顶层或 data 里，两边都找一遍。"""
        for k in keys:
            for src in (d, j):
                v = src.get(k)
                if v not in (None, ""):
                    return v
        return None

    token = pick("authentication_token", "token")
    is_login = pick("is_login")
    code_v = j.get("code")
    # 只要拿到了 token 就算登录成功：有的响应没有 code 字段，
    # 有的把 is_login 写成 1/true/"1"，都认。
    ok = bool(token) and (is_login in (1, "1", True) or code_v == 0)
    if not ok:
        log("LOGIN", "失败 %s" % (r.get("raw") or "")[:300])
        return {"ok": False, "code": code_v, "message": j.get("message") or "登录失败",
                "raw": (r.get("raw") or "")[:400]}

    authz = "XDS " + str(token)
    user = pick("user")
    if not isinstance(user, dict):
        user = {}

    expires = pick("expires_at", "token_expired_at", "expire_time", "expired_at") or 0
    try:
        expires = int(expires)
    except (TypeError, ValueError):
        expires = 0
    if expires <= 0:
        # 接口没给有效期就按 30 天兜底，免得固件把它当成已过期
        expires = int(time.time()) + 30 * 86400

    CFG["account"] = {
        "phone": phone,
        "user_id": pick("user_id"),
        "nickname": user.get("nickname") or "",
        "authorization": authz,
        "refresh_token": pick("refresh_token") or "",
        "expires_at": expires,
        "logged_at": int(time.time()),
    }
    save_cfg()
    log("LOGIN", "登录成功 user_id=%s 有效期至 %s" % (
        CFG["account"]["user_id"],
        time.strftime("%Y-%m-%d", time.localtime(CFG["account"]["expires_at"] or 0))))

    br, babies = fetch_babies(authz)
    CFG["babies"] = babies
    save_cfg()
    log("BABY", "拉取宝宝列表 → %d 个 %s" % (len(babies), [b["nickname"] for b in babies]))
    return {"ok": True, "account": {"user_id": CFG["account"]["user_id"],
                                    "nickname": CFG["account"]["nickname"],
                                    "expires_at": CFG["account"]["expires_at"]},
            "babies": babies, "babies_status": br.get("status")}


def find_baby(cid):
    for b in (CFG.get("babies") or []):
        if str(b.get("common_baby_id")) == str(cid):
            return b
    # 缓存里没有（比如重启后手动指定），回源再拉一次
    authz = (CFG.get("account") or {}).get("authorization", "")
    if authz:
        _, babies = fetch_babies(authz)
        CFG["babies"] = babies
        save_cfg()
        for b in babies:
            if str(b.get("common_baby_id")) == str(cid):
                return b
    return None


def c_escape(s):
    """把任意文本塞进 C 字符串字面量"""
    return (str(s or "").replace("\\", "\\\\").replace('"', '\\"')
            .replace("\r", "").replace("\n", "\\n"))


def do_bind(cid, baby_name="", memo=""):
    baby = find_baby(cid)
    if not baby:
        return {"ok": False, "error": "宝宝不存在，请重新登录"}
    r, ok = verify(CFG["account"].get("authorization", ""), baby)
    if not ok:
        return {"ok": False, "error": "验活失败：" + ((r.get("raw") or "")[:200]),
                "status": r.get("status")}
    CFG["baby"] = baby
    CFG["profile"] = {"name": (baby_name or "").strip()[:20],
                      "memo": (memo or "").strip()[:400]}
    save_cfg()
    log("BIND", "已绑定「%s」common_baby_id=%s 验活通过" % (baby["nickname"], baby["common_baby_id"]))
    if CFG["profile"]["name"] or CFG["profile"]["memo"]:
        log("PROFILE", "档案：小名=%s 备注=%d字" % (
            CFG["profile"]["name"] or "(空)", len(CFG["profile"]["memo"])))
    return {"ok": True, "baby": baby, "profile": CFG["profile"], "config": export_config()}


def default_out_path():
    """推断 meiyou_config.h 该写到哪：环境变量优先，其次同级 baby-esp32 工程"""
    env = (os.environ.get("MYY_CONFIG_OUT") or "").strip()
    if env:
        return env
    cand = os.path.abspath(os.path.join(BASE_DIR, "..", "baby-esp32", "main", "meiyou",
                                        "meiyou_config.h"))
    if os.path.isdir(os.path.dirname(cand)):
        return cand
    return os.path.join(BASE_DIR, "meiyou_config.h")


def do_save(baby_name, memo, out_path="", cid=""):
    """一次搞定：自动绑定宝宝（没绑就取第一个）→ 存档案 → 直接把 config.h 写到固件目录"""
    # 1) 宝宝：优先用指定的，否则取账号下第一个
    if cid:
        baby = find_baby(cid)
        if not baby:
            return {"ok": False, "error": "宝宝不存在，请重新登录"}
    else:
        baby = CFG.get("baby") or {}
        if not baby:
            lst = CFG.get("babies") or []
            if not lst:
                return {"ok": False, "error": "还没读到宝宝，请先登录"}
            baby = lst[0]

    if not CFG.get("baby") or CFG["baby"].get("common_baby_id") != baby.get("common_baby_id"):
        r, ok = verify(CFG["account"].get("authorization", ""), baby)
        if not ok:
            return {"ok": False, "error": "验活失败：" + ((r.get("raw") or "")[:200]),
                    "status": r.get("status")}
        CFG["baby"] = baby
        log("BIND", "已绑定「%s」common_baby_id=%s 验活通过" % (
            baby.get("nickname", ""), baby.get("common_baby_id")))

    # 2) 档案：只存用户真正填进去的内容，空就留空；
    #    导出配置时才回退到美柚昵称（见 export_config）
    CFG["profile"] = {"name": (baby_name or "").strip()[:20],
                      "memo": (memo or "").strip()[:400]}
    save_cfg()

    # 3) 写文件
    h = export_header()
    if not h:
        return {"ok": False, "error": "生成配置失败，检查是否已登录"}
    target = (out_path or "").strip() or default_out_path()
    target = os.path.abspath(os.path.expanduser(target))
    try:
        d = os.path.dirname(target)
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        with open(target, "w", encoding="utf-8", newline="\n") as f:
            f.write(h)
    except Exception as e:
        return {"ok": False, "error": "写入失败：%s" % e, "path": target}

    log("SAVE", "已写入 %s（%d 字节）" % (target, len(h.encode("utf-8"))))
    return {"ok": True, "path": target, "bytes": len(h.encode("utf-8")),
            "baby": {"nickname": baby.get("nickname", ""),
                     "common_baby_id": baby.get("common_baby_id"),
                     "birthday": baby.get("birthday", "")},
            "profile": CFG["profile"],
            "expires_at": (CFG.get("account") or {}).get("expires_at") or 0}


def do_profile(baby_name, memo):
    """绑定完成后单独更新档案（会重新导出 config.h）"""
    if not CFG.get("baby"):
        return {"ok": False, "error": "先在上面选一个宝宝完成绑定"}
    CFG["profile"] = {"name": (baby_name or "").strip()[:20],
                      "memo": (memo or "").strip()[:400]}
    save_cfg()
    log("PROFILE", "档案已更新：小名=%s 备注=%d字" % (
        CFG["profile"]["name"] or "(空)", len(CFG["profile"]["memo"])))
    return {"ok": True, "profile": CFG["profile"], "config": export_config()}


def export_header():
    """生成可直接 #include 的 C 头文件（编译进固件，不依赖局域网）"""
    c = export_config()
    if not c.get("authorization"):
        return None
    exp = c.get("expires_at") or 0
    exp_str = time.strftime("%Y-%m-%d %H:%M", time.localtime(exp)) if exp else "未知"
    lines = [
        "// 美柚宝宝记绑定配置 —— 由 meiyou-login 自动生成，勿手改",
        "// 账号：%s  宝宝：%s" % (c.get("phone") or c.get("user_id"), c.get("baby_nickname") or "?"),
        "// token 有效期至：%s（到点后重新跑绑定程序覆盖此文件并重烧）" % exp_str,
        "",
        "#pragma once",
        "",
        '#define MYY_AUTHORIZATION  "%s"' % c["authorization"],
        '#define MYY_API_BASE       "https://api-bbj.meiyou.com"',
        "",
        "#define MYY_USER_ID        %s" % (c.get("user_id") or 0),
        "#define MYY_BBID           %s" % (c.get("bbid") or 0),
        '#define MYY_BBID_STR       "%s"' % (c.get("bbid") or ""),
        '#define MYY_BBDAY          "%s"' % (c.get("bbday") or ""),
        "#define MYY_COMMON_BABY_ID %s" % (c.get("common_baby_id") or 0),
        "#define MYY_BABY_ID        %s" % (c.get("baby_id") or 0),
        '#define MYY_BABY_NICKNAME  "%s"' % c_escape(c.get("baby_nickname") or ""),
        '#define MYY_BABY_BIRTHDAY  "%s"' % c_escape(c.get("baby_birthday") or ""),
        "",
        "// ---- 宝宝档案：烧进固件当长期记忆，模型每次连上就能看到 ----",
        '#define MYY_BABY_NAME      "%s"' % c_escape(c.get("baby_name") or ""),
        '#define MYY_BABY_MEMO      "%s"' % c_escape(c.get("baby_memo") or ""),
        "",
        "#define MYY_EXPIRES_AT     %d" % exp,
        '#define MYY_UA             "%s"' % c["ua"],
        "",
    ]
    return "\n".join(lines)


def export_config():
    """给 ESP32 的完整绑定参数"""
    a = CFG.get("account") or {}
    b = CFG.get("baby") or {}
    pf = CFG.get("profile") or {}
    return {
        "phone": a.get("phone", ""),
        "user_id": a.get("user_id", ""),
        "authorization": a.get("authorization", ""),
        "refresh_token": a.get("refresh_token", ""),
        "expires_at": a.get("expires_at", 0),
        "bbid": b.get("common_baby_id", ""),
        "bbday": (b.get("birthday") or "").replace("-", ""),
        "common_baby_id": b.get("common_baby_id", ""),
        "baby_id": b.get("baby_id", ""),
        "baby_nickname": b.get("nickname", ""),
        "baby_birthday": b.get("birthday", ""),
        # 档案：没填小名就用美柚里的昵称，两者都空则留空
        "baby_name": (pf.get("name") or b.get("nickname") or ""),
        "baby_memo": pf.get("memo", ""),
        "mac_address": (CFG.get("device") or {}).get("mac", ""),
        "ua": UA,
        "api_base": "https://" + HOST_BBJ,
    }


# ---------------- 页面 ----------------
PAGE = r"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 美柚助手</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#f5f6f8;color:#1c1c1e;font:14px/1.6 -apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:720px;margin:0 auto;padding:18px 14px 70px}
h1{font-size:19px;margin:6px 0 3px}
.sub{color:#8a8a8e;font-size:12.5px;margin-bottom:16px}
.card{background:#fff;border:1px solid #e6e7eb;border-radius:12px;padding:15px;margin-bottom:12px}
.card h2{font-size:14px;margin:0 0 11px;display:flex;align-items:center;gap:8px}
.card h2 .n{width:20px;height:20px;border-radius:50%;background:#3b7ddd;color:#fff;font-size:12px;display:flex;align-items:center;justify-content:center;font-weight:500}
.badge{font-size:11px;color:#8a8a8e;border:1px solid #e0e2e7;border-radius:20px;padding:1px 8px;font-weight:400;margin-left:auto}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
input,textarea{flex:1;min-width:150px;padding:9px 11px;border:1px solid #d9dbe0;border-radius:8px;font-size:15px;font-family:inherit}
textarea{width:100%;resize:vertical;min-height:72px;line-height:1.6;font-size:14px}
input:focus,textarea:focus{outline:none;border-color:#3b7ddd;box-shadow:0 0 0 3px rgba(59,125,221,.12)}
button{padding:9px 15px;border:none;border-radius:8px;background:#3b7ddd;color:#fff;font-size:13.5px;cursor:pointer;font-family:inherit;white-space:nowrap}
button.ghost{background:#fff;color:#3b7ddd;border:1px solid #c9d8ef}
button:disabled{opacity:.45;cursor:not-allowed}
pre{background:#f7f8fa;border:1px solid #eceef2;border-radius:8px;padding:10px;font-size:12px;overflow:auto;max-height:300px;margin:9px 0 0;white-space:pre-wrap;word-break:break-all}
.dim{color:#8a8a8e}.ok{color:#1a7f37}.bad{color:#c62828}
.baby{display:flex;align-items:center;gap:10px;border:1px solid #e2e4e9;border-radius:10px;padding:10px 12px;margin-top:8px;cursor:pointer}
.baby:hover{border-color:#3b7ddd;background:#f7faff}
.baby .nm{font-weight:600}.baby .meta{font-size:12px;color:#8a8a8e}
.log div{font:12px/1.7 ui-monospace,Menlo,Consolas,monospace;color:#3c3c43}

/* —— 说明书部分 —— */
.tip{font-size:12.5px;color:#8a8a8e;margin:-4px 0 10px}
.opt{font-size:11px;color:#1a7f37;background:#eef8f0;border:1px solid #cfe9d6;border-radius:20px;padding:0 7px;margin-left:auto;font-weight:400}
.req{font-size:11px;color:#b26a00;background:#fff7e6;border:1px solid #ffe0a3;border-radius:20px;padding:0 7px;margin-left:auto;font-weight:400}
.lab{font-size:12.5px;color:#3c3c43;margin:10px 0 4px;display:flex;align-items:center;gap:6px}
#bname,#outpath{width:100%}
ol.how{margin:8px 0 0;padding-left:19px}
ol.how li{margin:6px 0;font-size:13.5px}
ol.how li b{color:#1c1c1e}
code{background:#f2f3f6;border:1px solid #e9ebef;border-radius:4px;padding:0 4px;font:12.5px ui-monospace,Menlo,Consolas,monospace;white-space:nowrap}
.prompt{background:#f7f8fa;border:1px solid #e6e7eb;border-radius:8px;padding:11px;margin:0;
        font:12.5px/1.8 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-word;color:#2b2b30}
.warn{background:#fff8e6;border:1px solid #ffe3a3;border-radius:8px;padding:9px 11px;font-size:12.5px;color:#7a5300;margin-top:10px;line-height:1.7}
.warn b{color:#8a5a00}
table.t{width:100%;border-collapse:collapse;font-size:13px;margin-top:6px}
table.t td,table.t th{border-bottom:1px solid #eceef2;padding:7px 5px;text-align:left;vertical-align:top}
table.t th{font-weight:500;color:#8a8a8e;font-size:12px}
details{margin-top:12px}
details summary{cursor:pointer;font-size:13px;color:#3b7ddd;outline:none;user-select:none}
.hl{border-color:#3b7ddd!important;box-shadow:0 0 0 3px rgba(59,125,221,.13)}
.foot{text-align:center;font-size:11.5px;color:#9a9aa0;margin:22px 0 0;line-height:1.9}
</style></head>
<body><div class="wrap">
<h1>ESP32 美柚助手</h1>
<div class="sub">说一句话，自动记进美柚宝宝记　·　个人做的小工具，与美柚 / 小智官方无关</div>

<div class="card">
  <h2>要做的事 <span class="badge" id="macTag"></span></h2>
  <table class="t">
    <tr><td style="width:34px;color:#3b7ddd;font-weight:600">⓪</td><td><b>装美柚 App</b>　注册账号、建好宝宝档案（已经有的跳过）</td></tr>
    <tr><td style="color:#3b7ddd;font-weight:600">①</td><td><b>登录美柚</b>　用手机号收短信验证码，程序自动读到你账号下的宝宝</td></tr>
    <tr><td style="color:#3b7ddd;font-weight:600">②</td><td><b>填宝宝档案</b>　两项都可以不填。填了会变成设备的「长期记忆」</td></tr>
    <tr><td style="color:#3b7ddd;font-weight:600">③</td><td><b>复制一句话给 AI</b>　让它把配置编译进固件并烧到板子上</td></tr>
    <tr><td style="color:#3b7ddd;font-weight:600">④</td><td><b>配网激活</b>　开机连 WiFi、拿 6 位激活码、去 xiaozhi.me 绑定</td></tr>
  </table>
  <div class="dim" style="font-size:12px;margin-top:10px" id="addr"></div>
</div>

<div class="card">
  <h2><span class="n">0</span>先有美柚账号和宝宝 <span class="badge">已经弄好就跳过</span></h2>
  <div class="tip">本程序不存任何记录，写的全是你的美柚 App 里的数据。所以得先有账号和宝宝。</div>
  <ol class="how">
    <li><b>装 App</b>：手机应用商店搜「美柚」安装（iPhone 在 App Store，安卓在华为 / 小米 / 应用宝）；官网 <code>www.meiyou.com</code>。</li>
    <li><b>注册登录</b>：用手机号注册。记住这个号码——下一步绑定必须用<b>同一个号</b>。</li>
    <li><b>建宝宝</b>：打开「宝宝记」，按提示创建宝宝档案（昵称、生日、性别）。</li>
    <li><b>确认能记</b>：在宝宝记里手动记一条（喂奶或换尿布），记上了就说明有写入权限。</li>
    <li><b>宝宝是家人建的？</b>让对方在「宝宝记 → 宝宝 → 邀请家人」里邀请你，接受后你才是成员；否则程序读不到宝宝。</li>
  </ol>
  <div class="warn">只有「宝宝记」的成员才能写入。App 里只能看不能记的话，先让对方给你开权限，再回来做下一步。</div>
</div>

<div class="card">
  <h2><span class="n">1</span>登录美柚 <span class="req">必填</span></h2>
  <div class="tip">用上一步注册美柚的那个手机号。必须是「宝宝记」里有记录权限的账号。</div>
  <div class="row" style="margin-bottom:8px">
    <input id="phone" placeholder="11 位手机号" inputmode="numeric" maxlength="11">
    <button class="ghost" id="send">发送验证码</button>
  </div>
  <div class="row">
    <input id="code" placeholder="6 位短信验证码" inputmode="numeric" maxlength="8">
    <button id="login">登录</button>
  </div>
  <pre id="out1" class="dim">等待操作</pre>
</div>

<div class="card" id="profileCard" style="display:none">
  <h2><span class="n">2</span>宝宝档案 <span class="opt">两项都选填</span></h2>
  <div class="dim" id="babyInfo" style="font-size:12.5px"></div>
  <div class="tip">填了会编译进固件，设备一开机就知道，不用每次重复说。空着也能正常用。</div>

  <div class="lab">宝宝姓名</div>
  <input id="bname" maxlength="20" placeholder="怎么称呼他，例如：小宝">
  <div class="dim" style="font-size:11.5px;margin-top:4px">选填，非必填。空着就用美柚里的昵称</div>

  <div class="lab">其他事项</div>
  <textarea id="bmemo" maxlength="400" placeholder="选填，非必填。一行一条，想到什么写什么，例如：&#10;对牛奶蛋白不过敏&#10;爸爸叫老王，妈妈叫小美&#10;晚上八点后不再喂辅食"></textarea>

  <div id="lastHint" style="display:none;margin-top:8px">
    <a href="javascript:;" id="fillLast" style="font-size:12.5px;color:#3b7ddd;text-decoration:none">↺ 用上次填的档案</a>
    <span class="dim" style="font-size:11.5px" id="lastWhat"></span>
  </div>

  <details>
    <summary>改写入路径（一般不用动）</summary>
    <input id="outpath" style="margin-top:8px;font-size:12.5px;color:#6b7280">
  </details>

  <div class="row" style="margin-top:12px">
    <button id="save">确认并写入配置</button>
  </div>
  <div class="dim" style="font-size:11.5px;margin-top:6px">点一下会自动创建 / 覆盖固件目录里的 meiyou_config.h，不需要下载文件</div>
  <pre id="outS" class="dim" style="display:none"></pre>
</div>

<div class="card" id="aiCard">
  <h2><span class="n">3</span>把这段话交给 AI 助手 <span class="req">接着做这步</span></h2>
  <div class="tip">复制下面整段，发给能操作你这台电脑的 AI。它会自己编译、自己烧，你不用管命令。</div>
  <pre class="prompt" id="prompt">（点一下下面的「确认并写入配置」，这里会自动填好文件路径）</pre>
  <div class="row" style="margin-top:10px">
    <button id="copy">复制这段话</button>
    <span class="dim" id="copyTip" style="font-size:12px"></span>
  </div>
  <div class="warn"><b>用哪种 AI：</b>需要能读写本机文件、能执行命令的（WorkBuddy、Claude Code、Cursor 等）。豆包、Kimi、ChatGPT 的网页聊天窗口没有电脑权限，读不到文件也烧不了，只能口头答疑。</div>
</div>

<div class="card">
  <h2><span class="n">4</span>烧完之后：联网 + 激活</h2>
  <div class="tip">这一步在板子上做。只有第一次开机需要，之后会自动连。</div>
  <ol class="how">
    <li><b>开机</b>：拔掉数据线再插上（或按一下 RST），屏幕出现配网提示。</li>
    <li><b>连设备热点</b>：手机 WiFi 列表里找 <code>Xiaozhi-XXXX</code>，点它。没有密码；提示「无法上网」是正常的，选「保持连接 / 仍然使用」。</li>
    <li><b>打开配网页</b>：一般会自动弹出；没弹就用手机浏览器输入 <code>192.168.4.1</code>。</li>
    <li><b>选 WiFi</b>：只能选 <b>2.4G</b> 的（5G 连不上），输密码 → 保存，设备会自动重启连网。</li>
    <li><b>记激活码</b>：连上后屏幕会显示 <b>6 位数字激活码</b>。</li>
    <li><b>去激活</b>：手机或电脑打开 <code>https://xiaozhi.me</code> → 登录 → 控制台 → 添加设备 → 输入这 6 位码。</li>
    <li><b>试一下</b>：听到提示音、屏幕进入待命，对它说 <b>「你好小智」</b>。</li>
  </ol>
  <div class="warn">配网页打不开？看三点：① 手机连的是设备热点，不是家里的 WiFi；② 先把手机流量关掉；③ 热点 5 分钟没人操作会自动关闭，重新上电再来一次。</div>
</div>

<div class="card">
  <h2>烧完之后说什么</h2>
  <div class="tip">先试只读的，能报数说明整条链路通了；再试写入。</div>
  <table class="t">
    <tr><th style="width:52%">说这句话</th><th>它会做什么</th></tr>
    <tr><td>今天喝了多少</td><td>报今日总量、次数、上次间隔</td></tr>
    <tr><td>上次换尿布什么时候</td><td>报上次尿布时间和隔了多久</td></tr>
    <tr><td>记一下，刚喝了 120 毫升</td><td>写一笔瓶喂进美柚</td></tr>
    <tr><td>亲喂了 20 分钟</td><td>写一笔母乳（按时间区间记）</td></tr>
    <tr><td>换尿布了 / 拉臭臭了</td><td>写尿 / 写便</td></tr>
    <tr><td>吃了 90 克米糊 / 吃了维生素 AD</td><td>写辅食 / 营养补剂</td></tr>
    <tr><td>洗澡了 / 睡着了 / 游泳了</td><td>写洗澡 / 睡眠 / 游泳</td></tr>
    <tr><td>记住，对牛奶蛋白不过敏</td><td>存进设备本地记忆，下次还记得</td></tr>
  </table>
  <details>
    <summary>出问题怎么办</summary>
    <table class="t">
      <tr><td style="width:46%">说「未绑定美柚」</td><td>配置没编进固件。确认 <code>meiyou_config.h</code> 在 <code>main/meiyou/</code> 下，让 AI 先 reconfigure 再 build 重烧</td></tr>
      <tr><td>说话没反应，只显示「听不清」</td><td>麦克风 / 喇叭的 I2S 引脚接反，改 <code>config.h</code> 里的 DIN / DOUT</td></tr>
      <tr><td>屏幕画面左右反了</td><td><code>DISPLAY_MIRROR_X</code> 改成 true</td></tr>
      <tr><td>提示「登录已过期」</td><td>token 约 30 天到期。回到本页第一步重新登录 + 写入，再让 AI 烧一次，<b>不用清 NVS</b></td></tr>
      <tr><td>WiFi 一直连不上</td><td>只支持 2.4G。路由器开了双频合一的话，单独开一个 2.4G 热点</td></tr>
    </table>
  </details>
</div>

<div class="card">
  <details style="margin-top:0">
    <summary>运行日志（排错时给 AI 看）</summary>
    <div class="log" id="log" style="margin-top:8px"><div class="dim">…</div></div>
  </details>
</div>

<div class="foot">ESP32 美柚助手 · 个人做的小工具，与美柚、小智官方没有任何关系<br>
用的是美柚 App 的非公开接口，随时可能失效；请只给自己家用</div>
</div>
<script>
const $ = s => document.querySelector(s);
const j = async (p, b, m='POST') => (await fetch(p,{method:m,headers:{'content-type':'application/json'},body:b?JSON.stringify(b):undefined})).json();
const put = (el, t, cls='') => { el.className = cls; el.textContent = typeof t==='string'?t:JSON.stringify(t,null,2); };
let cfgPath = '';
let lastProfile = null;

// 给 AI 助手的一段话：文件路径由写入结果决定，写入成功后才可用
function promptText(path){
  return [
    '我刚用「ESP32 美柚助手」生成好了配置，文件在这里：',
    path || '（还没写入）',
    '',
    '请帮我把小智固件编译并烧进这块 ESP32-S3，一步一步来，每步做完先告诉我结果：',
    '1. 检查上面这个文件存在，里面要有 MYY_AUTHORIZATION 和 MYY_COMMON_BABY_ID 两行',
    '2. 用 ESP-IDF 编译（之前编译过的话，先 reconfigure 再 build）',
    '3. 用 esptool 烧录，只写 app 分区 0x20000，绝对不要整片擦除，NVS 必须保留（配网和激活信息在里面）',
    '4. 烧完把串口日志里跟 meiyou、已注册工具有关的几行发给我',
    '5. 再告诉我怎么配网、去哪里激活'
  ].join('\n');
}
function setPath(p){
  cfgPath = (p || '').trim();
  $('#prompt').textContent = cfgPath ? promptText(cfgPath)
    : '（点一下上面的「确认并写入配置」，这里会自动填好文件路径）';
}

(async () => {
  const e = await j('/api/env', null, 'GET');
  $('#addr').textContent = `手机连同一个 WiFi 可打开：http://${e.ip}:${e.port}　设备标识 ${e.mac}`;
  $('#macTag').textContent = e.bound ? '已绑定过' : '未绑定';
  if (e.phone) $('#phone').value = e.phone;
  if (e.out_path) $('#outpath').value = e.out_path;
  // 档案一律留空：不预填、不给默认值。上次填过的只在下面放一个「用上次填的档案」的链接
  lastProfile = (e.profile && (e.profile.name || e.profile.memo)) ? e.profile : null;
  if (lastProfile) {
    $('#lastHint').style.display = 'block';
    $('#lastWhat').textContent = '　上次：' + (lastProfile.name || '没填姓名')
      + (lastProfile.memo ? '，' + lastProfile.memo.length + ' 字备注' : '');
  }
  if (e.bound) setPath(e.out_path);
  if (e.babies && e.babies.length) enterStep2(e.babies);
})();

$('#fillLast').onclick = () => {
  if (!lastProfile) return;
  $('#bname').value = lastProfile.name || '';
  $('#bmemo').value = lastProfile.memo || '';
  $('#lastHint').style.display = 'none';
};

$('#send').onclick = async () => {
  const phone = $('#phone').value.trim();
  if (phone.length !== 11) { put($('#out1'), '手机号需 11 位', 'bad'); return; }
  $('#send').disabled = true; put($('#out1'), '发送中…', 'dim');
  const r = await j('/api/send_code', {phone});
  put($('#out1'), r.ok ? `验证码已发送（${r.cooldown||60}s 后可重发）` : ('失败：' + (r.message||r.error||r.raw)), r.ok?'ok':'bad');
  let t = 60; const id = setInterval(()=>{ t--; $('#send').textContent = `重发(${t}s)`; if(t<=0){clearInterval(id); $('#send').disabled=false; $('#send').textContent='发送验证码';} }, 1000);
  logs();
};

$('#login').onclick = async () => {
  const phone = $('#phone').value.trim(), code = $('#code').value.trim();
  if (!code) { put($('#out1'), '请输入验证码', 'bad'); return; }
  $('#login').disabled = true; put($('#out1'), '登录中…', 'dim');
  const r = await j('/api/login', {phone, code});
  if (!r.ok) { put($('#out1'), '登录失败：' + (r.message||r.error||r.raw), 'bad'); }
  else {
    put($('#out1'), `登录成功　账号 ${r.account.nickname||r.account.user_id}　有效期至 ${new Date(r.account.expires_at*1000).toLocaleDateString()}`, 'ok');
    enterStep2(r.babies || []);
  }
  $('#login').disabled = false; logs();
};

// 登录成功后直接进入档案页：宝宝自动取第一个，不做选择，也不预填任何值
function enterStep2(list){
  if (!list.length){
    put($('#out1'), '登录成功，但这个账号下没读到宝宝，确认是「宝宝记」成员后再试', 'bad');
    return;
  }
  const b = list[0];
  $('#babyInfo').textContent = `已连接：${b.nickname}　生日 ${b.birthday||'—'}　${b.can_record?'可记录':'只读'}`;
  $('#profileCard').style.display = 'block';
}

$('#save').onclick = async () => {
  const baby_name = $('#bname').value.trim();
  const memo = $('#bmemo').value.trim();
  const out_path = $('#outpath').value.trim();
  $('#save').disabled = true;
  put($('#outS'), '正在写入…', 'dim'); $('#outS').style.display='block';
  const r = await j('/api/save', {baby_name, memo, out_path});
  if (!r.ok) {
    put($('#outS'), '写入失败：' + (r.error||''), 'bad');
  } else {
    const exp = r.expires_at ? new Date(r.expires_at*1000).toLocaleDateString() : '未知';
    let msg = `写入成功 → ${r.path}\n`;
    msg += `宝宝：${r.baby.nickname}　姓名：${r.profile.name||'（没填）'}　其他事项：${r.profile.memo.length} 字\n`;
    msg += `token 有效期至 ${exp}（到期回本页重来一次即可）\n\n`;
    msg += `下一步：把下面第 3 步那段话复制给 AI 助手，让它编译、烧录。`;
    put($('#outS'), msg, 'ok');
    $('#outpath').value = r.path;
    setPath(r.path);
    $('#aiCard').classList.add('hl');
    $('#aiCard').scrollIntoView({behavior:'smooth', block:'center'});
  }
  $('#save').disabled = false; logs();
};

$('#copy').onclick = async () => {
  if (!cfgPath) { $('#copyTip').textContent = '先完成上面的写入'; return; }
  const txt = $('#prompt').textContent;
  let done = false;
  try { await navigator.clipboard.writeText(txt); done = true; } catch(e) {}
  if (!done) {
    const t = document.createElement('textarea');
    t.value = txt; t.style.position='fixed'; t.style.opacity='0';
    document.body.appendChild(t); t.select();
    try { done = document.execCommand('copy'); } catch(e) {}
    t.remove();
  }
  $('#copyTip').textContent = done ? '已复制，直接粘给 AI' : '复制失败，手动选中上面文字复制';
};

async function logs(){
  const r = await j('/api/log', null, 'GET');
  $('#log').innerHTML = r.lines.map(l=>'<div>'+l.replace(/</g,'&lt;')+'</div>').join('') || '<div class="dim">空</div>';
}
setInterval(logs, 5000); logs();
</script></body></html>"""


def local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("223.5.5.5", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("content-type", ctype)
        self.send_header("content-length", str(len(data)))
        self.send_header("cache-control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass

    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/index.html"):
            return self._send(200, PAGE, "text/html; charset=utf-8")
        if p == "/api/env":
            a = CFG.get("account") or {}
            return self._send(200, {
                "ip": local_ip(), "port": PORT,
                "mac": CFG["device"]["mac"],
                "phone": a.get("phone", ""),
                "bound": bool(CFG.get("baby")),
                "babies": CFG.get("babies") or [],
                "profile": CFG.get("profile") or {},
                "out_path": default_out_path(),
                "expires_at": a.get("expires_at") or 0,
            })
        if p == "/api/log":
            with _lock:
                return self._send(200, {"lines": list(trace_buf)[-50:]})
        if p == "/api/config":
            return self._send(200, export_config())
        if p == "/api/export.h":
            h = export_header()
            if not h:
                return self._send(400, {"error": "还没完成绑定，先登录并点「确认并写入配置」"})
            data = h.encode("utf-8")
            self.send_response(200)
            self.send_header("content-type", "text/x-chdr; charset=utf-8")
            self.send_header("content-disposition", 'attachment; filename="meiyou_config.h"')
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            return self.wfile.write(data)
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        p = self.path.split("?")[0]
        n = int(self.headers.get("content-length") or 0)
        try:
            body = json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except Exception:
            body = {}
        if p == "/api/send_code":
            return self._send(200, do_send_code(body.get("phone")))
        if p == "/api/login":
            return self._send(200, do_login(body.get("phone"), body.get("code")))
        if p == "/api/bind":
            return self._send(200, do_bind(body.get("common_baby_id"),
                                           body.get("baby_name", ""), body.get("memo", "")))
        if p == "/api/save":
            return self._send(200, do_save(body.get("baby_name", ""), body.get("memo", ""),
                                           body.get("out_path", ""), body.get("common_baby_id", "")))
        if p == "/api/profile":
            return self._send(200, do_profile(body.get("baby_name", ""), body.get("memo", "")))
        if p == "/api/push":
            return self._send(200, do_push(body.get("url")))
        if p == "/api/probe":
            phone = (body.get("phone") or (CFG.get("account") or {}).get("phone") or "").strip()
            r = precheck(phone) if phone else {"status": 0, "raw": "缺少手机号"}
            return self._send(200, {"status": r.get("status"), "pred": CFG["runtime"].get("pred"),
                                    "uuid": CFG["runtime"].get("uuid"),
                                    "raw": (r.get("raw") or "")[:300]})
        return self._send(404, {"error": "not found"})


def do_push(url):
    payload = {"meiyou": export_config()}
    if not CFG.get("baby"):
        return {"ok": False, "error": "还没绑定宝宝"}
    if not url:
        return {"ok": False, "error": "请填设备地址",
                "hint": "curl -X POST " + url + " -H 'content-type: application/json' -d '" + json.dumps(payload, ensure_ascii=False) + "'"}
    try:
        req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                     method="POST", headers={"content-type": "application/json"})
        with urllib.request.urlopen(req, timeout=8) as r:
            txt = r.read().decode("utf-8", "replace")[:300]
        log("PUSH", "%s → HTTP %s" % (url, r.status))
        return {"ok": True, "status": r.status, "resp": txt}
    except Exception as e:
        log("PUSH", "失败 %s" % e)
        return {"ok": False, "error": str(e)}


def main():
    load_cfg()
    ip = local_ip()
    print("=" * 60)
    print(" ESP32 美柚助手（个人项目，非官方）")
    print(" 电脑：http://127.0.0.1:%d" % PORT)
    print(" 手机：http://%s:%d （同一 WiFi）" % (ip, PORT))
    print(" 设备标识：%s　配置：config.json" % CFG["device"]["mac"])
    if CFG.get("baby"):
        print(" 已绑定：%s（%s）" % (CFG["baby"].get("nickname"), CFG["baby"].get("common_baby_id")))
    print("=" * 60, flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n已停止")
