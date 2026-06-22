# WorkBuddy ESP32-P4 边缘 AI 桌宠

这是一个面向“嵌入式边缘 AI 应用”赛题的 ESP32-P4 触摸屏桌宠项目。项目重点不是单纯云端问答，而是在 ESP32-P4 端使用 ESP-DL 量化模型完成本地推理，根据天气、时间、日历/节假日等上下文给出硕士日常建议。

当前链路：

```text
触摸屏 -> ESP32-P4 -> 获取天气/日历上下文 -> ESP-DL 本地量化模型 -> LCD 当前建议
```

DeepSeek 仍可作为电脑端云端增强能力保留，但比赛展示时核心应强调：

```text
ESP32-P4 本地完成 AI 推理，DeepSeek 不是主决策链路。
```

## 当前功能

- 天气助手：显示西安天气、温度、降雨概率和出行提醒。
- 日程提醒：显示北京时间、日期、农历和节假日。
- 边缘 AI 研伴：ESP32-P4 本地运行 `main/models/workbuddy_advisor.espdl`，根据天气和日历输出本地推理结果。
- 边缘视觉：SC2336 摄像头采集画面，ESP-WHO 在 ESP32-P4 本地完成人脸检测，情绪页面显示人脸、表情、置信度和推理耗时。
- DeepSeek 建议：联网时由电脑端代理返回云端建议，屏幕左侧显示最终建议，右侧同时展示本地推理和 DeepSeek 建议。
- UI：LVGL 触摸界面，中文字体由 `tools/generate_workbuddy_fonts.js` 自动从屏幕源码提取生成。

## 本地 AI 模型

模型文件：

```text
main/models/workbuddy_advisor.espdl
```

这是通过 ESP-PPQ 导出的 ESP-DL INT8 量化模型，输入 21 个特征：

- 当前小时
- 是否工作日/节假日
- 降雨风险
- 是否炎热
- 是否寒冷
- 是否晴天
- 是否多云
- 是否节假日
- 研伴、天气和日历的触摸次数
- 距离上次交互的空闲时间
- 当前专注时间和休息时间
- 早晨、上午、午间、下午、晚餐、晚间和深夜时间窗

输出建议类别包括：

```text
BREAKFAST / LUNCH / DINNER / RESEARCH_FOCUS / PAPER_READING
WRITE_THESIS / EXERCISE / REST / SLEEP / UMBRELLA / HYDRATE / PLAN
```

摄像头结果已经通过独立的 `workbuddy_vision` 组件接入 UI。当前人脸检测使用真实 ESP-WHO 模型；表情分类仍是本地轻量判定，仓库不会将其描述成已训练完成的表情模型。后续加入正式 `expression.espdl` 后，可以继续把情绪结果扩展进 `workbuddy_edge_advisor.cpp` 的特征向量。

## 重新导出模型

不要在 ESP-IDF 自带 Python 环境里安装 ESP-PPQ，容易和 ESP-IDF 依赖冲突。直接运行：

```text
export_advisor_model.bat
```

脚本会创建独立 `.advisor_venv`，安装 PyTorch CPU 版和 ESP-PPQ，然后重新生成：

```text
main/models/workbuddy_advisor.espdl
main/models/workbuddy_advisor.info
main/models/workbuddy_advisor.json
main/models/workbuddy_advisor.onnx
```

仓库只提交 `.espdl`，其它导出中间产物会被 `.gitignore` 忽略。

## 启动电脑代理

电脑和开发板需要在同一 WiFi。双击：

```text
start_demo.bat
```

代理提供：

```text
/health
/weather
/time
/edge-context
/insight
```

ESP32-P4 的 AI 按钮现在请求 `/edge-context`，只取天气/日历原始上下文，然后在板端本地推理。`/insight` 仍保留给 DeepSeek 云端增强演示。

首次接入 DeepSeek 时双击：

```text
set_deepseek_key.bat
```

只需要粘贴 API Key。脚本会固定使用 `deepseek-chat`，保存本地配置并自动启动代理；窗口看到 `DeepSeek enabled: deepseek-chat` 才表示接入成功。

## 构建和烧录

推荐 VSCode ESP-IDF 插件，也可以命令行：

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COM3 flash
```

本次 ESP-DL 接入已用 ESP-IDF v5.5.4 构建通过。
项目分区表把 factory app 分区扩到 8M。融合 ESP-WHO、人脸模型和情绪页面后，当前固件仍有约 59% app 分区空间可供后续扩展。

## 新电脑从零运行

```powershell
git clone https://github.com/hcc666616/-.git workbuddy-edge-ai
cd workbuddy-edge-ai
```

然后双击：

```text
new_pc_check.bat
start_demo.bat
```

构建固件：

```text
build_firmware.bat
```

公开仓库不包含 Wi-Fi 密码。首次烧录前运行 `idf.py menuconfig`，在
`WorkBuddy Configuration` 中填写本机 Wi-Fi；生成的 `sdkconfig` 已被
`.gitignore` 忽略，不会上传。

## 项目文件

- `main/workbuddy_main.c`：主入口、WiFi、代理请求、本地 advisor 调用。
- `main/workbuddy_edge_advisor.cpp`：ESP-DL 本地模型加载、输入量化、推理和建议输出。
- `main/models/workbuddy_advisor.espdl`：ESP-DL 量化模型。
- `main/workbuddy_display_test.c`：LVGL UI。
- `components/workbuddy_vision/`：来自 `Jnassh/LeXin` 的摄像头、ESP-WHO 人脸检测和轻量表情管线适配组件。
- `tools/workbuddy_proxy.js`：电脑端天气/日历/DeepSeek 代理。
- `tools/export_workbuddy_advisor_espdl.py`：训练并导出 ESP-DL advisor 模型。
- `export_advisor_model.bat`：独立虚拟环境导出模型。

## 比赛讲解建议

可以这样介绍：

“我们的桌宠在 ESP32-P4 本地同时运行 ESP-DL 建议模型和 ESP-WHO 人脸模型，根据天气、日历、触摸交互及视觉状态提供陪伴。云端 DeepSeek 只作为联网增强，断网时仍能完成人脸检测和本地建议。情绪页面会明确展示模型来源、置信度和板端推理耗时。”
