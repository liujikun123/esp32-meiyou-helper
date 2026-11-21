# ESP32 美柚助手 · esp32-meiyou-helper

> 小智 AI × 美柚宝宝记：说一句话，自动记上。
> **个人做的小工具**，与美柚、小智官方没有任何关系，用的是美柚 App 的非公开接口。

给 ESP32 小智 AI 加一个「嘴替」：说一句「喝了 120 毫升」，自动记进美柚宝宝记；问一句「今天喝了多少」，直接报数。数据存在美柚 App 里，设备不落任何本地记录。

---

## 这是什么

一套**插件式扩展**，往 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 固件里加一个 `main/meiyou/` 模块，注册 7 个**设备端 MCP 工具**，让语音助手能直接调美柚接口：

| 工具名 | 触发说法 | 行为 |
|---|---|---|
| `self.baby.feed_add` | 「刚喝了 120 毫升配方奶」<br>「亲喂了 20 分钟」 | 写一笔喂奶：有毫升数记瓶喂，只有分钟数记亲喂 |
| `self.baby.feed_query` | 「今天喝了多少」「喂了几次」<br>「上次什么时候喂的」「该喂了吗」 | 一次返回今日总量/次数/分类，加上次时间与间隔 |
| `self.baby.diaper_add` | 「换尿布了」「拉臭臭了」 | 写一笔尿布（尿 42 / 便 43） |
| `self.baby.diaper_last` | 「上次换尿布什么时候」「多久没换了」 | 查上次尿布时间、间隔、是尿还是便 |
| `self.baby.food_add` | 「吃了 90 米糊加香蕉」<br>「吃了维生素 AD」 | 写辅食 / 营养补剂 |
| `self.baby.event_add` | 「洗澡了」「睡着了」「游泳了」 | 写洗澡/睡眠/玩耍/游泳/其他 |
| `self.baby.memory` | 「他多大了」「你还记得什么」<br>「记住，对牛奶蛋白不过敏」 | 读宝宝档案与长期记忆；`action=add` 时写入 NVS |

> 工具刻意按**参数结构**而非业务分类拆分：`food_add`/`event_add` 各自只有一个窄枚举，模型不容易选错。

配合一个 **零依赖的 PC 配置助手**（Python 纯标准库）完成短信登录、读取宝宝、档案填写、配置导出。

## 不包含什么

- ❌ **小智固件本体**：请自行获取 `78/xiaozhi-esp32`，本项目基于 **v2.0.2** 开发验证
- ❌ **硬件**：ESP32-S3 开发板需自备
- ❌ **任何账号凭证**：所有 token 都由使用者自己登录生成

## 前置条件

| 项 | 要求 |
|---|---|
| 硬件 | ESP32-S3 + PSRAM（8MB Flash 起，建议 16MB + 8MB PSRAM） |
| 固件 | xiaozhi-esp32 **v2.0.2** |
| 工具链 | ESP-IDF **v5.4.x**（其它版本未验证） |
| Python | 3.9+（助手零第三方依赖；烧录需 `esptool`） |
| 账号 | 美柚账号，且是**宝宝记成员**（有记录权限） |

---

## 整体流程

```
⓪ 美柚 App 建账号+宝宝 → ① 拿固件 → ② 接入模块 → ③ 登录并写入配置 → ④ 编译烧入 → ⑤ 配网激活 → ⑥ 说话测试
```

**顺序不能颠倒**：必须先拿到有效 token 并让助手写好 `meiyou_config.h`，再编译烧录。否则固件里是「未绑定」空壳分支。

---

## 第零步：装美柚 App，建好宝宝

本项目只是「语音遥控器」，一条记录都不落地，全部写进你自己的美柚 App。所以账号和宝宝得先存在：

| # | 做什么 | 说明 |
|---|---|---|
| 1 | 装 App | 应用商店搜「美柚」安装（iPhone 用 App Store，安卓用华为 / 小米 / 应用宝），官网 `www.meiyou.com` |
| 2 | 注册登录 | 用手机号注册。**后面绑定必须用同一个号** |
| 3 | 建宝宝 | 打开「宝宝记」→ 创建宝宝档案（昵称、生日、性别） |
| 4 | 确认能记 | 手动记一条（喂奶或换尿布），能记上就说明有写入权限 |
| 5 | 宝宝是家人建的 | 让对方在「宝宝记 → 宝宝 → 邀请家人」里邀请你，接受后你才是成员 |

> 只有「宝宝记」的成员才能写入。App 里只能看不能记，就是没权限，先找创建者开。

---

## 第一步：准备固件

```bash
git clone --depth 1 --branch v2.0.2 https://github.com/78/xiaozhi-esp32.git
cd xiaozhi-esp32
git describe --tags        # 必须输出 v2.0.2，不一致请停止，后面的补丁位置会对不上
```

ESP-IDF 环境（Windows 示例）：

```powershell
# 用 PowerShell，不要用 Git Bash —— MSYSTEM 环境变量会让 idf.py 静默跳过构建
$env:IDF_PATH="D:\esp\esp-idf"
$env:IDF_TOOLS_PATH="D:\esp\espressif"
```

## 第二步：接入美柚模块

### 2.1 拷贝文件

```
firmware/meiyou/         →  xiaozhi-esp32/main/meiyou/
firmware/examples/quandong-s3-dev/  →  xiaozhi-esp32/main/boards/quandong-s3-dev/   （仅在用这块板子时）
```

`main/meiyou/` 下应有 4 个文件：`meiyou_client.h/.cc`、`baby_tools.h/.cc`。

### 2.2 改 `main/CMakeLists.txt`（4 处）

**(a) 源文件列表** — 在 `set(SOURCES ...)` 里加两行：

```cmake
            "meiyou/meiyou_client.cc"
            "meiyou/baby_tools.cc"
```

**(b) 头文件目录 + 依赖** — 在 `set(INCLUDE_DIRS ...)` 里加 `"meiyou"`，然后紧跟一段：

```cmake
set(MYY_CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/meiyou/meiyou_config.h")
if(EXISTS "${MYY_CONFIG_H}")
    message(STATUS "meiyou_config.h found: ${MYY_CONFIG_H}")
    set_property(SOURCE "meiyou/meiyou_client.cc" APPEND PROPERTY OBJECT_DEPENDS "${MYY_CONFIG_H}")
    set_property(SOURCE "meiyou/baby_tools.cc" APPEND PROPERTY OBJECT_DEPENDS "${MYY_CONFIG_H}")
else()
    message(STATUS "meiyou_config.h 未导出，本次编译的固件将提示「未绑定美柚」")
endif()
```

> **为什么需要这段**：`meiyou_config.h` 是后补的文件。如果编进源码后用 `__has_include` 判断，编译器会把「不需要这个头」写进 `.d` 依赖表，之后补上配置也**不会重编**，固件里永远停在「未绑定」。这里显式挂成 `OBJECT_DEPENDS` 才能强制重编。

**(c) 编译宏** — 在 `target_compile_definitions(${COMPONENT_LIB} ...)` 之后加：

```cmake
if(EXISTS "${MYY_CONFIG_H}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HAS_MEIYOU_CONFIG=1)
endif()
```

**(d) 板型分支**（仅示例板型需要）— 在 `if(CONFIG_BOARD_TYPE_...)` 链里加：

```cmake
elseif(CONFIG_BOARD_TYPE_QUANDONG_S3_DEV)
    set(BOARD_TYPE "quandong-s3-dev")
    set(BUILTIN_TEXT_FONT font_puhui_basic_20_4)
    set(BUILTIN_ICON_FONT font_awesome_20_4)
    set(DEFAULT_EMOJI_COLLECTION twemoji_64)
```

### 2.3 改 `main/Kconfig.projbuild`（仅示例板型需要）

在其它 `config BOARD_TYPE_*` 旁边加：

```
    config BOARD_TYPE_QUANDONG_S3_DEV
        bool "LCDWIKI 2.8寸 ESP32-S3 (E32C28P/E32N28P)"
        depends on IDF_TARGET_ESP32S3
```

### 2.4 用别的板子？

`main/meiyou/` 本身与硬件无关。你只需要有一个能正常跑小智的板型，然后在 `config.h` 里保证音频链路正确。**最容易翻车的是 I2S 收放方向**，见「排错表」。

---

## 第三步：登录拿 token

```bash
cd pc
python server.py
# 电脑开 http://127.0.0.1:8899，手机同 WiFi 用 http://<电脑IP>:8899
```

页面流程只有两步：**输手机号 → 收验证码 → 登录 → （选填）宝宝姓名和其他事项 → 点「确认并写入配置」**。

宝宝不用选（取账号下第一个，多宝宝时取第一个）；也不需要下载文件——点确认后助手会**直接把 `meiyou_config.h` 写进固件目录**，页面会告诉你写到哪：

```
写入成功 → C:\...\xiaozhi-esp32\main\meiyou\meiyou_config.h
宝宝：宝宝　姓名：（没填）　其他事项：0 字
token 有效期至 2026/12/20（到期回本页重来一次即可）

下一步：把下面第 3 步那段话复制给 AI 助手，让它编译、烧录。
```

> **两个档案字段都是选填**，页面不预填任何默认值。填了会变成 `MYY_BABY_NAME` / `MYY_BABY_MEMO` 编进固件当长期记忆；不填就用美柚里的昵称，功能不受影响。上次填过的内容不会自动回填，但页面会给一个「用上次填的档案」的链接点一下填回来。

**这个页面本身就是说明书**：写入成功后，第 3 张卡片会自动填好文件路径并给出一段可以直接复制给 AI 助手（WorkBuddy / Claude Code / Cursor 等）的话，第 4 张卡片是烧完之后的配网、激活步骤（含 `xiaozhi.me`），再往下是试机清单和排错表。

写入位置按这个顺序决定：环境变量 `MYY_CONFIG_OUT` → 与 `pc/` 同级的 `baby-esp32/main/meiyou/` → `pc/meiyou_config.h`。页面上的路径框可以直接改。

生成的文件长这样（已脱敏）：

```c
#pragma once
#define MYY_AUTHORIZATION  "XDS x.xxxxxxxxxxxxxxxxxxxx"
#define MYY_API_BASE       "https://api-bbj.meiyou.com"
#define MYY_USER_ID        123456789
#define MYY_BBID           272000000
#define MYY_BBID_STR       "272000000"
#define MYY_BBDAY          "20260101"
#define MYY_COMMON_BABY_ID 272000000
#define MYY_BABY_ID        129000000
#define MYY_BABY_NICKNAME  "宝宝"
#define MYY_BABY_BIRTHDAY  "2026-01-01"
#define MYY_BABY_NAME      "小宝"                          // 你填的小名，进 LLM 上下文
#define MYY_BABY_MEMO      "对牛奶蛋白不过敏；爸爸叫老王"   // 你填的长期记忆
#define MYY_EXPIRES_AT     1797771926
```

> ⚠️ 这个文件含个人登录凭证，**不要提交到 git、不要发给别人**。

## 第四步：校验后烧入

烧之前做两项确认，任何一项不符就停下：

```bash
# 1. 芯片型号必须是 esp32s3，且能读到 MAC
python -m esptool --chip esp32s3 -p COM5 flash_id

# 2. 本地固件版本必须是 2.0.2
#    esp_app_desc_t 的 version 字段位于 app 分区 0x20000 + 0x30
python -m esptool --chip esp32s3 -p COM5 read_flash 0x20030 0x20 ver.bin
#    打开 ver.bin，或用下面这行直接看（PowerShell）：
#    ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("ver.bin")) -replace "[^\x20-\x7e]","")
```

编译：

```bash
python "$env:IDF_PATH/tools/idf.py" build
# 若上次编译时还没有 meiyou_config.h，先执行 reconfigure：
# python "$env:IDF_PATH/tools/idf.py" reconfigure
```

烧录（**保留 NVS**，不清配网和激活信息）：

```bash
python -m esptool --chip esp32s3 -p COM5 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size keep --flash_freq 80m \
  0x20000 build/xiaozhi.bin
```

全新板子首次烧录需要连引导一起：

```bash
python -m esptool --chip esp32s3 -p COM5 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size keep --flash_freq 80m \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xd000  build/ota_data_initial.bin \
  0x20000 build/xiaozhi.bin \
  0x800000 build/generated_assets.bin
```

> `--flash_size keep` 必须加，否则 esptool 会在探测时卡住。

## 第五步：配网激活

1. 手机连设备热点 `Xiaozhi-XXXX`（无跳转就浏览器开 `192.168.4.1`）
2. 选 2.4G WiFi 输密码
3. 屏幕出现 **6 位激活码** → 到 [xiaozhi.me](https://xiaozhi.me/) 控制台 → 新建智能体 → 添加设备 → 输码
4. 设备播报就绪音，进入待命

## 第六步：验证

串口 115200 应看到：

```
I (481)  baby_tools: 已注册 7 个工具：喝奶/汇总/尿布记/尿布查/辅食/事件/记忆
I (5621) MCP: Add tool: self.get_device_status
I (5621) MCP: Add tool: self.audio_speaker.set_volume
I (5631) MCP: Add tool: self.screen.set_brightness
I (5631) MCP: Add tool: self.screen.set_theme
I (5641) MCP: Add tool: self.get_system_info [user]
I (5641) MCP: Add tool: self.reboot [user]
I (6480) Application: STATE: idle          ← 待命，可以说话了
```

> 带 `[user]` 标记的是 `AddUserOnlyTool` 注册的，**不会下发给模型**（`mcp_server.cc` 里 `if (!list_user_only_tools && (*it)->user_only()) continue;`），所以它们不占 token。实际进模型上下文的是 4 个系统工具 + 7 个 baby 工具 = 11 个。

然后依次试：

1. 「今天喝了多少」 → 只读，最安全，能报数就说明链路全通
2. 「上次什么时候喂的」 → 看能否算间隔
3. 「记一下，刚喝了 120 毫升」 → 会真的写入，去美柚 App 里确认

---

## 排错表

| 现象 | 原因 | 处理 |
|---|---|---|
| 说了「记一下」但提示**未绑定美柚** | 编译时没带上 `meiyou_config.h`，或编译缓存没刷新 | 确认文件在 `main/meiyou/` 下 → `idf.py reconfigure` → 再 build。检查 `.obj` 时间戳是否比 `meiyou_config.h` 新 |
| 唤醒后进聆听状态，**说话没反应** | **I2S 收放接反**：`DOUT`/`DIN` 写反了 | 见下方「I2S 方向」 |
| 屏幕画面**左右镜像** | 屏幕 MX 位与出厂扫描方向相反 | `esp_lcd_panel_mirror(panel, true, false)`，即 `DISPLAY_MIRROR_X=true` |
| 喇叭一直静音 / 一直响 | 功放使能极性反了 | 量一下 PA 引脚：低电平使能则 `pa_inverted=true` |
| 屏幕颜色像**负片** | 反相位不对 | 改 `DISPLAY_INVERT_COLOR` |
| 红蓝互换 | RGB/BGR 顺序 | 改 `DISPLAY_BGR_ORDER` |
| 登录返回 200 却提示失败 | 美柚有些版本把 token 放 JSON **顶层**而非 `data` 里 | 助手的 `do_login` 已兼容两层，无需改 |
| `idf.py build` 秒退、无任何输出 | 在 Git Bash 里跑，`MSYSTEM` 变量导致跳过构建（退出码仍是 0） | 改用 PowerShell；或 `idf.py reconfigure` + 在 `build/` 里直接 `ninja -j 1` |

### 亲喂时长的字段（最容易被静默丢弃的坑）

美柚「亲喂母乳」是 `record_type=1, record_category=11`。它**不吃 detail 里的任何时长字段**：实测把 `breast_duration`、`feed_duration`、`breast_left_duration`、`suckle_duration` 等 12 个候选键一起塞进去，读回来 `record_detail` 是 `{}`，顶层 `quantity` 也会被重置为 0，接口照样返回 `{"code":0,"data":"bingo"}`——**静默丢弃，不报错**。

唯一生效的写法是给时间区间：

```json
{"start_at":"2026-11-21 07:01:26","end_at":"2026-11-21 07:21:26","record_detail":{}}
```

服务端会原样保留这对时间，`unit_type` 自动为 `1`（时长）。所以 `add_nurse()` 的做法是：把用户说的「喂完的那一刻」当 `end_at`，往前推 N 分钟算 `start_at`。

> 顺便记一个调试技巧：`DELETE /v3/life/record?id=<id>&common_baby_id=<cid>&baby_id=<bid>` 可以删记录，用来清理字段探测留下的测试数据。

### I2S 方向（最容易踩的坑）

厂商 wiki 表格里标的 `output`/`input` 通常是**站在 codec 视角**。对 ES8311：

```
IO6 = ES8311 ASDOUT (net I2S_DO) → 麦克风数据 → ESP32 的 DIN
IO8 = ES8311 DSDIN  (net I2S_DI) → 喇叭数据   → ESP32 的 DOUT
```

所以 `config.h` 应该写成：

```c
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8   // ESP → codec（喇叭）
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6   // codec → ESP（麦克风）
```

接反的表现：**麦克风采样全是 0，喇叭也没声音，但状态机一切正常**（会进聆听、会显示「听不清」）。诊断方法：在 `Read()` 里打印 PCM 峰值，全 0 即确诊。

---

## 限制与风险

- **token 约 30 天过期**：过期后所有工具会返回「登录已过期」。重新跑第三步、重编重烧即可
- **非公开接口**：美柚 App 升级可能改接口或加风控，届时需要重新逆向
- **只做了喂奶/尿布**：睡眠、身高体重、辅食等未接入
- **唤醒词固定为「你好小智」**：WakeNet9 预训练模型，不是配置项。想换只能（a）换 ESP-SR 里其它预置词，（b）切 MultiNet 命令词自定义（唤醒率会下降），（c）找乐鑫定制模型
- **不要分发 `meiyou_config.h`**：里面是别人的登录凭证

## 原理一句话

小智设备端 MCP 工具运行在 ESP32 上，服务器只是把「用户说了什么」转成工具调用下发；工具内部用 `esp_http_client` 直连美柚 API。所以**不需要任何中间服务器，也不依赖局域网**——烧好固件、配好网，就能用。

## License

仅供个人学习与自用。请遵守美柚的服务条款，不要用于批量自动化或商业用途。
