# 乐鑫 LeXin ESP32-P4 边缘 AI 桌宠

乐鑫是一套运行在 ESP32-P4 Function EV Board 上的触摸式边缘 AI 桌宠。天气、日历、研伴建议和情绪识别共用同一套固件与 UI；ESP-DL 建议模型和 ESP-WHO 人脸检测均在开发板本地运行，DeepSeek 仅作为联网增强能力。

## 项目亮点

- **本地建议模型**：`main/models/lexin_advisor.espdl` 在 ESP32-P4 上完成 INT8 推理。
- **本地视觉模型**：SC2336 摄像头输入由 ESP-WHO 在板端进行人脸检测和跟踪。
- **实时人机交互**：触摸次数、空闲时长、专注计时、天气和日期共同参与建议推理。
- **断网仍可展示**：摄像头、人脸框、情绪状态、本地建议和触摸 UI 不依赖云端。
- **云端增强可选**：电脑代理负责天气、时间和 DeepSeek，未配置 API Key 时不影响本地模型。

核心链路：

```text
SC2336 摄像头 -> ESP-WHO -> 人脸框与情绪状态 ----+
天气 / 日期 / 触摸 / 专注数据 -> ESP-DL 建议模型 ----+-> LVGL 触摸界面
电脑代理 -> 天气 / 时间 / DeepSeek 联网增强 --------+
```

## 四个展示入口

主屏提供四个独立入口：

1. **天气提醒**：温度、天气、降雨概率和生活提示。
2. **日程提醒**：北京时间、月历、农历、日期类型和今日安排。
3. **情绪研伴**：实时摄像头、人脸跟踪框、表情、置信度、推理延迟和桌宠响应。
4. **研伴建议**：左侧为 ESP-DL 本地建议，右侧为 DeepSeek 独立建议，二者使用同一上下文但分别显示。

## 硬件与软件

硬件：

- ESP32-P4 Function EV Board
- 配套 1024 x 600 触摸屏
- SC2336 MIPI-CSI 摄像头
- ESP32-C6 无线子板
- 两根数据线：开发板供电/调试与无线子板连接

新电脑需要：

- Windows 10/11
- VS Code 与 Espressif IDF 扩展，或 ESP-IDF 5.5.x 命令行环境
- Git
- Node.js 18 或更新版本
- 电脑连接项目预设的演示 Wi-Fi

本仓库默认目标已固定为 `esp32p4`，首次下载后不需要手动修改 CMake 目标。演示 Wi-Fi 已写入 `sdkconfig.defaults`；更换网络时只修改该文件中的 `CONFIG_LEXIN_WIFI_SSID` 和 `CONFIG_LEXIN_WIFI_PASSWORD`，然后重新构建烧录。

## 新电脑首次运行

### 1. 下载项目

```powershell
git clone https://github.com/Caspian66671/-.git LeXin
cd LeXin
```

如果通过网页下载 ZIP，请解压后直接打开包含根目录 `CMakeLists.txt` 的 `LeXin-main` 文件夹，不要只打开 `main` 子目录。

### 2. 检查环境

双击：

```text
new_pc_check.bat
```

脚本会检查 Git、Node.js、ESP-IDF 和项目必要文件。ESP-IDF 显示未就绪时，在 VS Code 中执行 `ESP-IDF: Open ESP-IDF Terminal`，再运行构建脚本。

### 3. 启动电脑代理

先让电脑连接与开发板相同的演示 Wi-Fi，然后双击：

```text
start_demo.bat
```

首次运行会申请一次 Windows 防火墙权限，以允许开发板访问本机 `8787` 端口。脚本会自动获取新电脑的局域网地址；开发板会自动发现代理，不需要手动填写电脑 IP。

窗口出现以下内容即可：

```text
Proxy OK.
Weather:
Time:
AI pet insight:
```

### 4. 构建固件

在 ESP-IDF 终端中双击或运行：

```text
build_firmware.bat
```

等价命令为：

```powershell
idf.py set-target esp32p4
idf.py build
```

### 5. 烧录

连接开发板后双击：

```text
flash_firmware.bat
```

脚本使用 ESP-IDF 自动选择可用串口，因此不把 COM3 写死。若电脑同时连接多个串口设备，可在 ESP-IDF 状态栏选择正确端口后执行：

```powershell
idf.py -p COMx flash monitor
```

烧录结束后按一次开发板复位键。屏幕应先显示乐鑫主界面，视觉服务随后在后台完成摄像头和模型初始化。

## DeepSeek 配置

DeepSeek 是可选增强，不是本地决策链路的必要条件。第一次配置时双击：

```text
set_deepseek_key.bat
```

粘贴 API Key 后，配置只保存在本机忽略文件 `deepseek_config.ps1`，不会提交到仓库。随后重新运行 `start_demo.bat`。窗口出现 `DeepSeek enabled: deepseek-chat` 表示连接成功。

## 比赛现场展示顺序

建议按以下顺序演示，整套流程约 90 秒：

1. 上电后展示乐鑫主屏，说明四个功能属于同一固件。
2. 打开**情绪研伴**，正对摄像头并缓慢左右移动，展示 ESP-WHO 人脸框跟随、`FACE: YES`、置信度和毫秒级延迟。
3. 返回后打开**天气提醒**，展示实时天气和降雨概率。
4. 打开**日程提醒**，展示月历、农历、工作日/节假日和今日安排。
5. 打开**研伴建议**，说明左侧是 ESP32-P4 上的 ESP-DL 本地推理，右侧是 DeepSeek 联网增强。
6. 断开代理或关闭电脑网络，再次进入情绪研伴和本地建议，证明边缘 AI 仍可运行。

为了获得稳定的人脸框，摄像头与人脸建议保持 30 至 80 厘米距离，避免强逆光。检测框由真实 ESP-WHO 结果生成，不使用固定或模拟方框。

## 模型说明

### ESP-DL 研伴模型

模型文件：

```text
main/models/lexin_advisor.espdl
```

模型输入包含天气、日期类型、小时段、触摸交互、空闲时长和专注计时等 21 维特征。输出类别包括吃饭、科研专注、论文阅读、论文写作、锻炼、休息、睡眠、带伞、补水和任务规划。

重新训练和导出时运行：

```text
export_advisor_model.bat
```

脚本使用独立 `.advisor_venv`，不会污染 ESP-IDF Python 环境。仓库只提交烧录所需的 `.espdl`；`.onnx`、`.json` 和 `.info` 属于可再生成的中间产物。

### ESP-WHO 人脸检测

视觉子系统位于 `components/lexin_vision/`。摄像头画面在板端缩放为模型输入，ESP-WHO 输出真实人脸坐标，随后进行时间确认、框平滑和短时保持。ROI 质量检查只用于判断表情信息是否可靠，不会把已经成立的人脸框删除。

## 目录说明

```text
main/lexin_main.c                 Wi-Fi、代理、任务队列与应用入口
main/lexin_display_test.c         LVGL 主屏和四个功能页面
main/lexin_edge_advisor.cpp       ESP-DL 本地建议模型
main/lexin_interaction.c          触摸与专注计时数据
main/models/lexin_advisor.espdl   本地量化模型
components/lexin_vision/          摄像头、ESP-WHO、人脸跟踪与情绪状态
components/human_face_detect/     与当前 ESP-DL 运行时匹配的人脸模型组件
tools/lexin_proxy.js              天气、日期与 DeepSeek 电脑代理
tools/start_lexin_proxy.ps1       自动发现网络并启动代理
```

后续功能应在上述边界内扩展：视觉输入放入 `components/lexin_vision/`，新页面放入 `main/lexin_display_test.c`，新的本地建议特征放入 `main/lexin_edge_advisor.cpp`。不要再引入第二套 `app_main`、LCD 初始化或摄像头初始化，否则会造成黑屏、I2C 冲突或相机重复占用。

## 常见问题

### 黑屏

确认打开的是仓库根目录，并执行完整 `idf.py build` 后再烧录。不要把另一个项目的 `app_main`、BSP 初始化或旧 `sdkconfig` 单独覆盖进来。

### 天气提示“检查网络后再试”

确认电脑已连接演示 Wi-Fi，`start_demo.bat` 窗口显示 `Proxy OK.`，且首次防火墙授权已通过。可双击 `check_proxy.bat` 验证 `/weather` 和 `/time`。

### 相机有画面但没有人脸框

先确认摄像头无遮挡、光线均匀、人脸距离合适。串口日志应出现 `HumanFaceDetect model wrapper created` 和 `real vision backend active`。页面中的 `FACE: YES`、框坐标和置信度都来自本地模型。

### 中文显示方框

运行：

```powershell
node tools\generate_lexin_fonts.js
idf.py build
```

字体脚本会从 UI 源码提取实际使用的中文字符并重新生成字体。

## 安全与提交规则

- 不提交 `deepseek_config.ps1`、API Key、日志、`build/`、`sdkconfig` 和模型中间产物。
- `sdkconfig.defaults` 是新电脑构建基线；本机 `sdkconfig` 只是生成文件。
- 合并新功能前至少执行一次完整构建，并分别打开天气、日历、情绪研伴和研伴建议页面。
