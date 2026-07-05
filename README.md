# 乐鑫 LeXin ESP32-P4 边缘 AI 桌宠

乐鑫是一套运行在 ESP32-P4 Function EV Board 上的触摸屏边缘 AI 桌宠项目。它把天气、日历、科研/生活建议、触摸交互、专注计时、摄像头情绪识别和语音对话合并到同一个固件中，重点展示“边缘 AI 本地推理”，而不是只依赖云端问答。

项目核心链路：

```text
触摸/天气/日历/专注计时 -> ESP-DL 本地量化建议模型 -> LCD 当前建议
SC2336 摄像头 -> ESP-WHO 本地人脸检测/跟踪 -> LCD 情绪研伴
ES8311 麦克风 -> 能量门限分段 -> HTTP /voice-stream -> 电脑代理 FunASR/规则/DeepSeek -> LCD 语音回复
电脑代理 -> 天气/日期/DeepSeek 可选增强 -> LCD 云端建议
```

DeepSeek 是联网增强项；比赛展示时应重点说明 ESP32-P4 上的 ESP-DL 建议模型和 ESP-WHO 人脸识别都在本地运行。

## 项目详细介绍

LeXin 的定位是“面向学习与生活陪伴的边缘 AI 桌宠”。它不是一个单纯的天气屏，也不是只把大模型接口接到屏幕上的聊天终端，而是把多源数据采集、本地模型推理、人脸识别、触摸交互和云端增强整合成一个可以现场演示的嵌入式 AI 应用。

项目围绕比赛题目中的“嵌入式边缘 AI 应用”展开，重点体现以下能力：

- **边缘 AI 本地推理**：ESP-DL 建议模型在 ESP32-P4 上完成推理，根据天气、日期、触摸、空闲时长和专注计时输出本地建议。
- **本地视觉识别**：SC2336 摄像头采集画面后，由 ESP-WHO 在板端进行人脸检测和跟踪，UI 显示真实人脸框、置信度和延迟。
- **人机交互闭环**：用户点击天气、日历、研伴建议、情绪研伴等入口时，触摸行为会被记录，并参与后续建议判断。
- **科研生活场景**：默认用户画像是电子信息方向硕士，建议内容偏向学习专注、论文阅读、资料整理、休息、锻炼和健康提醒。
- **云端增强但不替代本地 AI**：DeepSeek 只作为联网增强和对照展示；本地模型、视觉识别和基础 UI 不依赖 DeepSeek。

比赛展示时可以这样介绍本项目：

```text
这是一个运行在 ESP32-P4 上的边缘 AI 桌宠系统。
它通过触摸屏、摄像头、天气、日历和专注计时采集上下文，
再由 ESP-DL 本地量化模型给出日常/科研建议，
同时用 ESP-WHO 做本地人脸识别与情绪研伴展示。
DeepSeek 作为云端增强，只用于和本地模型形成对比，不是主决策链路。
```

## 系统架构

整体分为五层：

```text
┌────────────────────────────────────────────────────────────┐
│                        LVGL 触摸 UI                         │
│       主界面 / 天气 / 日历 / 情绪研伴 / 研伴建议              │
└───────────────▲───────────────────────────▲────────────────┘
                │                           │
┌───────────────┴──────────────┐ ┌──────────┴────────────────┐
│        本地 AI 与交互层        │ │        摄像头视觉层          │
│ ESP-DL 建议模型               │ │ SC2336 摄像头               │
│ 触摸次数 / 空闲时长 / 专注计时 │ │ ESP-WHO 人脸检测与跟踪       │
└───────────────▲──────────────┘ └──────────▲────────────────┘
                │                           │
┌───────────────┴───────────────────────────┴────────────────┐
│                  ESP32-P4 主控与任务队列                     │
│ Wi-Fi / 代理发现 / HTTP 请求 / 上下文缓存 / 四任务队列         │
└───────────────▲────────────────────────────────────────────┘
                │
┌───────────────┴────────────────────────────────────────────┐
│                       电脑端代理                             │
│ tools/lexin_proxy.js                                        │
│ Open-Meteo 天气 / 本地日期农历 / DeepSeek 可选增强 / UDP 发现 │
└────────────────────────────────────────────────────────────┘
```

核心文件职责：

| 模块 | 主要文件 | 职责 |
| --- | --- | --- |
| 应用入口 | `main/lexin_main.c` | 初始化 NVS、UI、本地模型、视觉服务、Wi-Fi、代理发现和任务队列 |
| UI 页面 | `main/lexin_display_test.c` | 绘制主界面、天气、日历、情绪研伴和研伴建议页面 |
| 本地建议模型 | `main/lexin_edge_advisor.cpp` | 解析上下文、构造特征、调用 ESP-DL 量化模型并输出建议 |
| 交互数据 | `main/lexin_interaction.c` | 记录触摸次数、空闲时长、专注/休息计时等行为数据 |
| 视觉识别 | `components/lexin_vision/` | 摄像头采集、ESP-WHO 人脸检测、框平滑、情绪状态输出 |
| 电脑代理 | `tools/lexin_proxy.js` | 获取天气、生成日历、调用 DeepSeek、提供 HTTP 接口 |
| 模型文件 | `main/models/lexin_advisor.espdl` | ESP-DL 本地 INT8 量化建议模型 |

## 核心数据流

### 1. 开机初始化流程

```text
app_main
  -> 初始化 NVS
  -> 创建触发队列和 4 个 trigger worker
  -> 启动 LVGL 主界面，先保证屏幕亮起来
  -> 延迟 800 ms 后初始化 ESP-DL 建议模型
  -> 启动 lexin_vision 摄像头和人脸识别服务
  -> 连接 Wi-Fi
  -> 启动 proxy_warmup_task 预取天气和日历
```

这样设计是为了让屏幕尽快显示，避免模型或摄像头初始化阻塞首帧，造成开机黑屏时间过长。

### 2. 天气数据流

```text
用户点击天气图标
  -> lexin_enqueue_trigger(LEXIN_ACTION_WEATHER)
  -> trigger_task 先尝试显示缓存天气
  -> ESP32-P4 访问 http://电脑IP:8787/weather
  -> 电脑代理请求 Open-Meteo
  -> 返回 TEMP / WEATHER / RAIN / ADVICE
  -> 开发板缓存天气上下文
  -> LVGL 天气页面显示温度、天气、降雨概率和生活建议
```

天气数据同时会进入本地建议模型，影响例如“带伞”“补水”“室内学习”“天气适合出门”等判断。

### 3. 日历数据流

```text
用户点击日历图标
  -> lexin_enqueue_trigger(LEXIN_ACTION_TIME)
  -> trigger_task 先尝试显示缓存日历
  -> ESP32-P4 访问 http://电脑IP:8787/time
  -> 电脑代理按 Asia/Shanghai 生成北京时间
  -> 本地生成公历、农历、节假日和日期类型
  -> 返回 TIME / DATE / LUNAR / HOLIDAY / DAY_TYPE
  -> 开发板缓存日历上下文
  -> LVGL 日历页面显示月历、农历和今日安排
```

日期类型会参与本地模型推理。工作日更偏学习、论文、科研节奏；周末和节假日更偏休息、锻炼、轻量计划。

### 4. 本地建议模型数据流

```text
用户点击研伴建议
  -> ESP32-P4 先把已缓存的天气和日历组合起来
  -> lexin_interaction_build_context 追加触摸和专注计时数据
  -> lexin_edge_advisor_infer_text 调用 ESP-DL 本地模型
  -> 输出 ESP-DL 本地建议、置信度和延迟
  -> 左侧卡片显示本地模型结果
```

本地模型输入特征包括：

- 当前小时
- 工作日、周末、节假日
- 天气类型
- 降雨风险
- 温度冷热
- 天气/日历/研伴建议的触摸次数
- 长时间未触摸屏幕的空闲状态
- 专注计时和休息计时

这些特征共同决定建议类别，例如论文阅读、资料整理、专注学习、吃饭、喝水、锻炼、休息、睡眠或带伞。

### 5. DeepSeek 云端增强数据流

```text
用户点击研伴建议
  -> ESP32-P4 访问 http://电脑IP:8787/edge-context
  -> 电脑代理组合天气和日历上下文
  -> 若配置 DeepSeek API Key，则请求 DeepSeek
  -> 返回云端建议
  -> 右侧卡片显示 DeepSeek 独立建议
```

DeepSeek 和本地模型使用相近上下文，但两者独立输出。展示时可以说明：左侧是 ESP32-P4 本地模型结果，体现边缘 AI；右侧是云端大模型建议，用来做增强和对照。

### 6. 情绪研伴数据流

```text
SC2336 摄像头采集画面
  -> lexin_vision 获取帧
  -> ESP-WHO 本地人脸检测
  -> 输出 face yes/no、坐标框、置信度、延迟、表情状态
  -> vision_snapshot_callback 回传 UI
  -> LVGL 情绪研伴页面绘制实时画面、人脸框和回应语
```

情绪研伴页面中的人脸框不是固定图形，而是来自本地视觉结果。后续如果要接入更完整的表情分类模型，建议继续放在 `components/lexin_vision/` 内部，不要新建第二套摄像头初始化。

### 7. 触摸交互与专注计时数据流

```text
用户点击主界面或功能入口
  -> lexin_interaction 记录触摸类型和时间
  -> 统计天气、日历、研伴建议点击次数
  -> 计算多久没有触摸屏幕
  -> 维护专注/休息计时状态
  -> 作为本地模型特征进入 ESP-DL 推理
```

例如频繁点击研伴建议，系统会认为当前注意力可能被打断，更倾向输出“回到学习”“拆分任务”“专注一段时间”等建议；长时间未触摸则更倾向提醒喝水、休息或活动。

## 人脸识别锁屏

开机后自动进入锁屏界面，必须通过人脸识别才能进入主界面。不同用户有独立的
数据空间（交互次数、专注计时等通过 NVS 保存）。

### 1. 数据流

```text
SC2336 摄像头 + ESP-WHO 人脸检测
   -> 定位人脸框并裁剪人脸区域 (RGB565)
   -> HTTP POST /face-recognize 上传到电脑代理
   -> 代理用 64-bit 平均哈希 (aHash) 比对已注册用户
   -> 匹配成功：返回 user_id + user_name
   -> 匹配失败：提示"未识别到用户"，可创建新账户
```

### 2. 首次使用 / 注册新用户

1. 开机进入锁屏界面，摄像头自动开始扫描人脸
2. 人脸出现后进入识别流程
3. 若为陌生面孔，屏幕显示：

   - 状态：**未识别到用户**
   - 按钮：**创建新用户**
4. 点击按钮进入注册页面，用屏幕键盘输入用户名（支持 a-z / 空格 / 删除）
5. 点击 **确认创建**，板子上传裁剪好的人脸 + 用户名到 `/face-register`
6. 代理保存人脸哈希 + 用户名到 `tools/face_users.json`
7. 显示 **注册成功**，自动进入主界面

### 3. 已注册用户

1. 对着摄像头，人脸框出现后自动上传人脸
2. 匹配成功 → 显示 **欢迎回来，xxx**
3. 自动加载该用户的历史交互数据（天气/日历/研伴点击次数等）
4. 进入主界面，左上角显示用户名

### 4. 多用户数据隔离

每个用户在 NVS 中有独立命名空间（`{user_id}_wt` 等 key）。切换用户时：

- 当前用户数据自动保存到 NVS
- 新用户数据从 NVS 加载（首次为空）

主界面左上角显示当前用户名，表示正在该用户的专属空间中操作。

### 5. 脱网/超时回退

- 人脸识别需要代理（电脑端）运行
- 若摄像头 8 秒未检测到人脸，自动以 `乐鑫用户` 身份解锁，方便开发调试
- 已在 NVS 中缓存的用户登录状态会保留到下次开机

### 6. 文件结构

```text
components/lexin_face_auth/
  CMakeLists.txt
  include/lexin_face_auth.h
  lexin_face_auth.c          # 人脸裁剪 + HTTP POST + 状态机 + NVS 缓存
tools/lexin_proxy.js          # /face-recognize /face-register /face-users
tools/face_users.json         # 用户人脸数据库 (自动生成)
```

## 语音对话（乐鑫乐鑫唤醒）

ESP32-P4 Function EV Board 自带 ES8311 codec + 板载麦克风。固件已经把它接进
`components/lexin_voice/`，主界面底部新增"语音对话"入口。

### 1. 数据流

```text
ES8311 麦克风 (16 kHz / mono / 16-bit)
   -> 板子端 30 ms 帧 + 能量门限 (RMS 阈值)
   -> 切分一句话（>= 阈值进入录音，< 阈值 900 ms 视为结束）
   -> 拼装 RIFF/WAVE，上传到电脑代理 /voice-stream
   -> 代理端 FunASR（或回退规则）做唤醒词 + 识别
   -> 本地规则 / FAQ 路由，复杂问题（"为什么/怎么/介绍" 等）走 DeepSeek
   -> 板子收到 JSON，在"语音对话"页显示识别文本 + 回复
```

### 2. 唤醒词与识别

- 唤醒词：默认 `乐鑫,lexin,lex i n,lexinlexin,乐心`，大小写不敏感。
  可以在代理启动前用环境变量 `LEXIN_WAKE_KEYWORDS=乐鑫,乐鑫乐鑫` 覆盖。
- 识别：默认走**回退规则**——板子把 WAV 传到代理，代理立刻视为"已唤醒"，
  并给一个文字回执，方便在没有安装任何 ASR 模型时先跑通闭环。
- 想要真实 ASR 时，安装 FunASR 或 Whisper，然后在启动代理前设置：
  ```powershell
  $env:LEXIN_ASR_CMD = "python tools/run_funasr.py"
  $env:LEXIN_ASR_LANG = "zh"
  ```
  代理会把 `/tmp/lexin-voice/voice_*.wav` 路径作为最后一个参数传给该命令，
  并把标准输出当作 `{"text": "..."}` 解析。
- 想让代理把识别文本发给 DeepSeek（仅在 FAQ 没有命中时）：
  ```powershell
  $env:LEXIN_DEEPSEEK_VOICE = "1"
  ```
  这会启用代理内置的"问题/解释/总结"等关键词路由。

### 3. 启动顺序

1. 启动代理：`start_demo.bat`，确认日志里有
   `Voice ASR command: <not configured>, using fallback wake+rules`。
2. 烧录固件，板子接同一个 Wi-Fi，屏幕底部出现"语音对话 喊 乐鑫乐鑫 即可唤醒"。
3. 点击 banner 进入语音对话页，看到状态从"待命中 -> 正在听 -> 上传中 ->
   思考中 -> 已回复"循环。
4. 对板子喊一次"乐鑫乐鑫"（或随便什么词），停顿约 1 秒，板子会回一句
   默认话。等 FunASR 接入后，识别文本会显示在"你说"位置，回复会显示在
   "回复"位置。

### 4. 不依赖真板子的冒烟测试

```powershell
node tools\test_voice_stream.js
```

会生成一段 880 Hz 的 800 ms beep 并 POST 到 `http://127.0.0.1:8787/voice-stream`。
代理回退路径会返回包含 `reply` 字段的 JSON，确认整个 HTTP 链路通。

### 5. 喇叭到位后怎么接

- 输出端用 `bsp_audio_codec_speaker_init()`，参考 BSP 文档初始化 I2S TX 通道。
- 在 `lexin_voice_reply_dispatch` 后增加一段"把 reply 文本丢进 TTS 队列"。
  推荐先用 `esp_tts`（组件管理器有）做中文 TTS，再走 `esp_codec_dev_write`
  播放。等喇叭接好，UI 上"回复"区域可直接念出来。
- 启用 AFE + AEC（`esp-sr` AFE）后，唤醒检测可以从"能量门限"升级为
  WakeNet9 / WakeNet9s 离线唤醒，**组件级 API 边界不变**，不影响 UI / 代理。

### 6. 文件结构

```text
components/lexin_voice/
  CMakeLists.txt
  include/lexin_voice.h
  lexin_voice.c                  # 麦克风采集 + 能量门限 + WAV 上传
tools/lexin_proxy.js             # /voice-stream 路由 + 回退唤醒/规则/DeepSeek
tools/test_voice_stream.js       # 不需要板子也能跑的冒烟脚本
main/lexin_launcher.c            # 新增 LEXIN_SCREEN_VOICE 入口
main/lexin_main.c                # 启动 lexin_voice
main/lexin_display_test.c        # "语音对话"页 + 状态 banner
```

## 后续开发数据接口

后续扩展功能时建议按下面的边界添加，不要把新功能直接塞进 `app_main`：

| 想扩展的内容 | 推荐接入位置 | 输出给谁 |
| --- | --- | --- |
| 新传感器数据 | 新建 `components/xxx_sensor/`，在 `main/lexin_main.c` 注册回调 | UI 或本地模型 |
| 新视觉模型 | 扩展 `components/lexin_vision/` | 情绪研伴页面和建议模型 |
| 新建议特征 | `main/lexin_edge_advisor.cpp` 和模型导出脚本 | ESP-DL 本地模型 |
| 新页面 | `main/lexin_display_test.c` | LVGL 主界面 |
| 新云端接口 | `tools/lexin_proxy.js` 增加 HTTP endpoint | ESP32-P4 HTTP 客户端 |
| 新触摸行为 | `main/lexin_interaction.c` | 本地模型特征 |

开发新模块时，推荐保持下面的数据格式习惯：电脑代理输出简单的多行 `KEY: VALUE` 文本，开发板解析后缓存上下文，再交给 UI 或本地模型。这样调试时可以直接在浏览器里访问接口，也便于串口日志观察。

## 已验证环境

本项目当前验证通过的开发环境如下，建议新电脑尽量保持一致：

| 项目 | 版本 |
| --- | --- |
| ESP-IDF | v5.5.4 |
| ESP-IDF VS Code 扩展 | 使用 Espressif IDF 扩展并选择 ESP-IDF v5.5.4 |
| 芯片目标 | esp32p4 |
| CMake | 3.30.2 |
| Ninja | 1.12.1 |
| RISC-V 工具链 | riscv32-esp-elf gcc 14.2.0 |
| Python 环境 | ESP-IDF 自带 Python 3.11 环境 |
| Node.js | 18 或更高版本 |
| 操作系统 | Windows 10/11 |

ESP-IDF 组件管理器会自动拉取依赖。当前构建中使用到的主要组件包括：

- `espressif/esp-dl 3.3.6`
- `espressif/esp32_p4_function_ev_board 5.2.3`
- `espressif/esp_cam_sensor 2.0.1`
- `espressif/esp_hosted 2.12.9`
- `espressif/esp_wifi_remote 1.6.1`
- `espressif/esp_lcd_ek79007 2.0.2~1`
- `espressif/esp_lcd_touch_gt911 1.2.0~2`
- `espressif/esp_lvgl_port 2.8.0~1`
- `lvgl/lvgl 9.5.0`

## 硬件

- ESP32-P4 Function EV Board
- 配套 1024 x 600 触摸屏
- SC2336 MIPI-CSI 摄像头
- ESP32-C6 无线子板
- 两根数据线：一根用于主板供电/调试，一根用于无线子板连接

## 功能入口

主界面包含四个功能入口：

1. 天气提醒：显示温度、天气、降雨概率、体感、湿度、风力、气压和生活建议。
2. 日程提醒：显示北京时间、月历、农历、日期类型和今日安排。
3. 情绪研伴：调用本地摄像头画面、人脸框、置信度、推理延迟和研伴回应。
4. 研伴建议：左侧显示 ESP-DL 本地模型建议，右侧显示 DeepSeek 云端建议，两者使用同一上下文但独立输出。

## 天气和日历数据来源

天气、日历和 DeepSeek 不直接由开发板访问公网，而是通过电脑端代理 `tools/lexin_proxy.js` 获取和整理。这样做有三个好处：开发板启动更快、API Key 不会写进固件、现场网络变化时只需要保证电脑和开发板在同一个 Wi-Fi。

### 天气数据

天气数据来自 Open-Meteo 免费天气接口：

```text
https://api.open-meteo.com/v1/forecast
```

当前项目默认查询西安坐标：

```text
latitude  = 34.3416
longitude = 108.9398
timezone  = Asia/Shanghai
```

代理请求的主要字段是：

- `current_weather=true`：获取当前温度和天气代码。
- `hourly=precipitation_probability`：获取逐小时降雨概率。
- `forecast_days=1`：只取当天数据，减少网络开销。
- `timezone=Asia/Shanghai`：统一使用北京时间。

代理会把 Open-Meteo 返回值整理成开发板容易解析的文本格式：

```text
TEMP: 31C
WEATHER: CLOUDY
RAIN: 0%
ADVICE: HOT
```

字段含义：

- `TEMP`：当前温度，单位摄氏度。
- `WEATHER`：天气类型，取值为 `SUNNY`、`CLOUDY`、`RAIN`、`SNOW` 或 `UNKNOWN`。
- `RAIN`：离当前时间最近一小时的降雨概率。
- `ADVICE`：代理给出的基础生活提示，取值为 `UMBRELLA`、`HOT`、`COLD`、`GOOD` 或 `CHECK_NETWORK`。

天气接口有 5 分钟缓存。若当前网络请求失败但之前成功取过天气，会优先使用缓存；若没有缓存，则返回 `UNKNOWN` 和 `CHECK_NETWORK`，屏幕会提示检查网络。

### 日历数据

日历数据不依赖外部云端接口，由电脑端代理根据本机时间本地生成：

- 当前时间：使用 `Asia/Shanghai` 时区生成北京时间。
- 公历日期：由电脑系统时间转换得到。
- 农历日期：使用 Node.js 的 `Intl.DateTimeFormat("zh-CN-u-ca-chinese")` 生成中国农历。
- 节假日：当前内置固定节日规则，包括元旦、劳动节、国庆节。
- 日期类型：根据节假日和周末判断为 `HOLIDAY`、`WEEKEND` 或 `WORKDAY`。

代理会输出：

```text
TIME: 14:40
DATE: 2026-06-04
LUNAR: si yue shi jiu
HOLIDAY: NONE
DAY_TYPE: WORKDAY
```

开发板收到后会在 LCD 上转换成中文显示，例如日期、农历、日期类型和今日安排。当前规则适合比赛展示和日常演示；如果后续需要更完整的中国调休表，可以继续在 `tools/lexin_proxy.js` 的 `holidayFor()` 函数中扩展。

### 开发板如何获得数据

主界面点击不同功能时，开发板会访问电脑代理的不同接口：

```text
天气提醒  -> http://电脑IP:8787/weather
日程提醒  -> http://电脑IP:8787/time
研伴建议  -> http://电脑IP:8787/edge-context
云端建议  -> http://电脑IP:8787/insight
健康检查  -> http://电脑IP:8787/health
```

开发板和电脑必须连接同一个 Wi-Fi。电脑代理会监听 `0.0.0.0:8787`，并通过 UDP `8788` 做局域网发现；开发板启动后会自动寻找代理，正常情况下不需要手动填写电脑 IP。

## 新电脑快速启动

### 1. 安装工具

在新电脑上先安装：

1. Git
2. VS Code
3. VS Code 的 Espressif IDF 扩展
4. ESP-IDF v5.5.4
5. Node.js 18 或更高版本

安装 ESP-IDF 时建议通过 VS Code 扩展安装或选择 `D:\Espressif\frameworks\esp-idf-v5.5.4` 这一类标准路径。只要 VS Code 右下角显示 ESP-IDF v5.5.4，即可继续。

### 2. 下载项目

推荐用 Git 克隆，并把本地文件夹命名为 `LeXin`：

```powershell
git clone https://github.com/Caspian66671/-.git LeXin
cd LeXin
```

如果从 GitHub 网页下载 ZIP，解压后的文件夹名可能由仓库名决定。无论文件夹叫什么，打开时都必须打开包含根目录 `CMakeLists.txt` 的项目根目录，不能只打开 `main` 子目录，否则会出现 `CMakeLists.txt not found in project directory`。

### 3. 检查环境

双击：

```text
new_pc_check.bat
```

它会检查 Git、Node.js、ESP-IDF 和项目关键文件。若提示 ESP-IDF 环境不可用，请在 VS Code 中执行：

```text
ESP-IDF: Open ESP-IDF Terminal
```

再进行构建。

### 4. 网络配置

演示 Wi-Fi 已写在 `sdkconfig.defaults`：

```text
CONFIG_LEXIN_WIFI_SSID="abc"
CONFIG_LEXIN_WIFI_PASSWORD="abc123456"
```

新电脑和开发板需要连接同一个 Wi-Fi。若现场网络不同，只改 `sdkconfig.defaults` 里的 SSID 和密码，然后重新构建烧录。

### 5. 启动电脑代理

天气、日历和 DeepSeek 需要电脑端代理。代理可以在烧录前启动，也可以在烧录后启动；只要开发板和电脑在同一个 Wi-Fi，开发板就能访问代理。

最简单的启动方式是双击：

```text
start_demo.bat
```

它会调用：

```text
tools/start_lexin_proxy.ps1
```

这个 PowerShell 脚本会做几件事：

1. 读取本地 `deepseek_config.ps1`，如果没有配置 DeepSeek，就自动使用本地兜底建议。
2. 启动 Node.js 代理 `tools/lexin_proxy.js`。
3. 监听 HTTP 端口 `8787`，给开发板提供 `/weather`、`/time`、`/edge-context` 等接口。
4. 监听 UDP 端口 `8788`，让开发板自动发现电脑 IP。
5. 自动访问 `http://127.0.0.1:8787/weather` 和 `http://127.0.0.1:8787/time` 做自检。

窗口显示以下内容即可：

```text
Proxy OK.
Weather:
Time:
AI pet insight:
```

如果想手动启动，也可以在项目根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\start_lexin_proxy.ps1
```

如果想检查数据是否已经能被访问，可以双击：

```text
check_proxy.bat
```

也可以在浏览器或 PowerShell 中打开：

```text
http://127.0.0.1:8787/weather
http://127.0.0.1:8787/time
http://127.0.0.1:8787/health
```

正常天气返回类似：

```text
TEMP: 31C
WEATHER: CLOUDY
RAIN: 0%
ADVICE: HOT
```

正常日历返回类似：

```text
TIME: 14:40
DATE: 2026-06-04
LUNAR: si yue shi jiu
HOLIDAY: NONE
DAY_TYPE: WORKDAY
```

首次运行如果 Windows 防火墙弹窗，请允许访问当前网络。若没有授权，电脑自己访问 `127.0.0.1` 可能正常，但开发板访问电脑 IP 会失败，天气页面就会显示“检查网络后再试”。

### 6. 构建固件

在 ESP-IDF 终端中运行，或双击：

```text
build_firmware.bat
```

等价命令是：

```powershell
idf.py set-target esp32p4
idf.py build
```

本项目的根 `CMakeLists.txt` 已固定目标为 `esp32p4`，正常情况下新电脑不需要手动改目标。

### 7. 烧录

连接开发板后双击：

```text
flash_firmware.bat
```

如果电脑上有多个串口，也可以手动指定：

```powershell
idf.py -p COM3 flash monitor
```

烧录完成后按一次开发板复位键。主界面应在几秒内显示，天气、日历、研伴建议和情绪研伴都可以从主界面快速进入。

## DeepSeek 配置

DeepSeek 是可选增强，不影响本地模型和情绪识别。首次配置双击：

```text
set_deepseek_key.bat
```

输入 API Key 后会生成本机忽略文件 `deepseek_config.ps1`，该文件不会提交到仓库。随后重新双击 `start_demo.bat`。窗口出现 `DeepSeek enabled` 表示云端建议可用。

请不要把真实 API Key 写入 README 或提交到 GitHub。
## 比赛演示流程

建议按以下顺序展示：

1. 开机展示主界面，说明四个功能入口都在同一个 ESP32-P4 固件里。
2. 打开情绪研伴，正对摄像头并缓慢移动，展示人脸框、`FACE: YES`、置信度、延迟和情绪回应。
3. 返回主界面，打开天气提醒，展示真实天气数据和生活建议。
4. 打开日程提醒，展示月历、农历、日期类型和今日安排。
5. 打开研伴建议，说明左侧是 ESP-DL 本地量化模型，右侧是 DeepSeek 云端建议。
6. 断开电脑代理或不配置 DeepSeek，再进入情绪研伴和本地建议，证明边缘 AI 能独立运行。

## 目录说明

```text
main/lexin_main.c                 Wi-Fi、代理、任务队列和应用入口
main/lexin_display_test.c         LVGL 主屏和四个功能页面
main/lexin_edge_advisor.cpp       ESP-DL 本地建议模型推理
main/lexin_interaction.c          触摸交互和专注计时数据
main/models/lexin_advisor.espdl   本地 INT8 量化建议模型
components/lexin_vision/          摄像头、ESP-WHO、人脸跟踪和情绪状态
components/human_face_detect/     与当前 ESP-DL 运行时匹配的人脸模型组件
tools/lexin_proxy.js              天气、日期和 DeepSeek 电脑代理
tools/start_lexin_proxy.ps1       自动发现网络并启动代理
```

`build/`、`managed_components/`、`sdkconfig`、日志文件和本机 API Key 都是生成文件或本地文件，不需要提交。新电脑构建时会自动重新生成。

## 重新生成字体

如果修改了 UI 中的中文文案，运行：

```powershell
node tools\generate_lexin_fonts.js
idf.py build
```

字体脚本会从 `main/lexin_display_test.c` 提取中文字符并生成 LVGL 字体。若屏幕出现方框，通常就是某个新中文字符没有进入字体，需要重新生成并烧录。

## 常见问题

### 1. 黑屏

确认打开的是项目根目录，且完整执行过 `idf.py build`。不要把另一个项目的 `app_main`、BSP 初始化或旧 `sdkconfig` 直接覆盖进来，否则可能造成 LCD 初始化冲突。

### 2. 天气显示“检查网络后再试”

确认电脑和开发板在同一个 Wi-Fi，`start_demo.bat` 窗口显示 `Proxy OK.`，并且 Windows 防火墙允许端口 `8787`。可以双击 `check_proxy.bat` 验证 `/weather` 和 `/time`。

### 3. 相机有画面但没有人脸框

保持人脸距离摄像头约 30 到 80 厘米，避免强逆光。串口日志应出现真实视觉后端初始化信息。页面里的 `FACE: YES`、坐标和置信度都来自 ESP-WHO 本地推理，不是固定假框。

### 4. 中文显示方框

运行：

```powershell
node tools\generate_lexin_fonts.js
idf.py build
```

然后重新烧录。主页标题中的 `LeXin` 使用英文字体，避免项目名缺字造成左上角方框。

## 合并新功能注意事项

- 不要新增第二个 `app_main`。
- 不要重复初始化 LCD、触摸、摄像头和 BSP。
- 新的视觉能力放到 `components/lexin_vision/`。
- 新的 UI 页面放到 `main/lexin_display_test.c`。
- 新的本地建议特征放到 `main/lexin_edge_advisor.cpp`。
- 合并后至少验证四个入口：天气、日历、情绪研伴、研伴建议。
