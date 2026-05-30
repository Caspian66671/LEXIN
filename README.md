# ESP32-P4 桌宠助手 AI 展示版

这是 ESP32-P4 Function EV Board 的触摸屏展示项目。当前版本面向“桌面智能陪伴宠物”比赛场景，保留稳定的本地代理方案，不走 QClaw 实时生成，展示时响应更快、更稳。

运行链路：

```text
触摸屏 -> ESP32-P4 -> WiFi -> 电脑本地代理 -> 天气/日历数据 -> LCD 显示
```

## 当前功能

- `天气助手`：显示西安天气、温度、降雨概率、出行建议，并作为桌宠环境判断依据。
- `日程提醒`：显示月份、日期、时间、农历、节假日，并作为桌宠提醒依据。
- `智能建议`：把天气、时间、节假日、后续情绪/人脸识别结果汇总为桌宠建议。

屏幕 UI 使用 LVGL，英文数字使用 Montserrat，中文使用项目内自定义字库：

```text
main/workbuddy_cn_20.c
main/workbuddy_cn_28.c
```

如果后面新增固定中文显示内容，需要先把新增汉字加入：

```text
tools/generate_workbuddy_fonts.js
```

然后重新生成字库并构建。

## AI 接入原则

屏幕端只显示板端已有模板，不直接显示大模型返回的任意长文本。这样可以避免缺字、乱码和展示现场网络波动。

推荐后续让外部模型或识别模块返回结构化字段：

```json
{
  "face": "detected",
  "emotion": "tired",
  "advice_type": "rest"
}
```

ESP32-P4 端再映射为固定模板，例如：

```text
检测疲惫建议休息
雨天记得带伞
今天适合专注学习
```

已经预留的屏幕接口：

```c
workbuddy_screen_update_ai_context(WORKBUDDY_FACE_DETECTED, WORKBUDDY_EMOTION_TIRED);
```

后续人脸识别、情绪识别模块只需要调用这个接口或通过串口/HTTP 转成同样的枚举即可。

## 启动电脑代理

电脑和开发板需要连接同一个 WiFi。展示前双击：

```text
start_demo.bat
```

这个脚本会启动本地代理，并检查 `/health`、`/weather`、`/time`。

也可以只检查代理：

```text
check_proxy.bat
```

代理默认监听：

```text
http://0.0.0.0:8787
```

可用接口：

```text
/health
/weather
/time
```

如果想让 Windows 登录后自动启动代理，可以运行：

```text
install_proxy_startup.bat
```

## 接线提醒

保持 MIPI 屏幕排线连接。显示屏供电/控制排针：

- 显示屏 `5V` -> 主板 `5V`
- 显示屏 `GND` -> 主板 `GND`
- 显示屏 `RST_LCD` -> 主板 `GPIO5`
- 显示屏从主板供电时，不要再接显示屏独立的 `5V INPUT` Type-C

GPIO 使用：

- `GPIO5`：LCD reset
- `GPIO7`：GT911 touch I2C SDA
- `GPIO8`：GT911 touch I2C SCL
- `GPIO22`：LCD backlight

## 构建和烧录

推荐使用 VSCode ESP-IDF 插件构建、烧录。命令行也可以：

```powershell
idf.py build
idf.py -p COM3 flash
```

如果 Windows 分配了其他串口，把 `COM3` 改成实际端口。

## 配置

默认配置在 `sdkconfig.defaults`：

```text
CONFIG_WORKBUDDY_WIFI_SSID="abc"
CONFIG_WORKBUDDY_WIFI_PASSWORD="abc123456"
CONFIG_WORKBUDDY_PROXY_BASE_URL="auto"
CONFIG_WORKBUDDY_PROXY_PREFERRED_HOST="172.31.169.142"
CONFIG_WORKBUDDY_PROXY_PORT=8787
```

如需修改，运行：

```powershell
idf.py menuconfig
```

进入 `WorkBuddy Configuration` 修改。

## 项目文件说明

- `main/workbuddy_main.c`：主入口，初始化 WiFi、LCD、触摸、业务逻辑。
- `main/workbuddy_display_test.c`：屏幕 UI 和触摸交互。
- `main/workbuddy_actions.c`：天气/日历请求和解析。
- `main/workbuddy_launcher.c`：桌面图标入口。
- `tools/workbuddy_proxy.js`：电脑端本地代理，提供天气和日历数据。
- `tools/generate_workbuddy_fonts.js`：生成中文 LVGL 字库。
- `start_demo.bat`：一键启动并检查代理。

## 后续优化方向

1. 把情绪识别、人脸识别结果接入 `workbuddy_screen_update_ai_context()`。
2. 给智能建议增加串口/HTTP 输入，方便另外两位同学联调。
3. 把本地代理扩展为可选 Doubao 结构化建议生成，但保留本地固定模板作为比赛兜底。
