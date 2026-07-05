# 表情情绪识别模型（FER2013 → ESP32-P4 ESP-DL）

本地训练一个 7 类表情情绪 CNN，量化导出为 `main/models/expression.espdl`，
固件构建时自动嵌入并把表情后端从「启发式」切换到「ESP-DL 神经网络」。

## 7 类情绪与索引顺序（固定，勿改）

```
0 neutral  1 happy  2 sad  3 angry  4 surprise  5 fear  6 disgust
```

索引 0/1/2 保持 neutral/happy/sad，与固件里旧的三类逻辑兼容。
此顺序在三处必须一致：
- `tools/train_emotion_fer2013.py` 的 `CANONICAL_LABELS`
- `components/lexin_vision/port/expression_adapter_esp_dl.h` 的 `expression_fer_label_t`
- 导出的 `expression.espdl` 输出张量

## 一键训练 + 导出

> **Python 版本要求：3.10 / 3.11 / 3.12。esp-ppq 不支持 Python 3.13+。**
> 如果你默认的 `python` 是 3.13/3.14，装一个 3.12（勾选 Add to PATH），
> `.bat` 会用 `py -3.12` 启动器自动挑对版本，两个版本可共存。

双击 / 运行：

```
train_emotion_model.bat
```

它会：
1. 建 `.emotion_venv` 虚拟环境，装 `torch(cpu) / esp-ppq / numpy / pillow / datasets`
2. `tools/train_emotion_fer2013.py` —— 下载 FER2013、训练 CNN、存检查点
   `tools/emotion/emotion_cnn.pt`，并打印测试集准确率
3. `tools/export_emotion_espdl.py` —— 量化导出 `main/models/expression.espdl`

完成后重新构建 + 烧录固件：`build_firmware.bat` 然后 `flash_firmware.bat`。

## 数据集来源（二选一，脚本自动判断）

1. **本地 Kaggle CSV**（优先）：把 `fer2013.csv` 放到 `tools/emotion/data/`，
   或用环境变量 `FER2013_CSV=路径` 指定。列格式：`emotion, pixels, Usage`。
2. **HuggingFace 镜像**（无需登录）：找不到本地 CSV 时自动从公开镜像下载，
   可用 `FER2013_HF_REPO=用户/仓库` 覆盖默认镜像。

> FER2013：35887 张 48×48 灰度图，7 类。小 CNN 量化后测试准确率约 60–66%
> （人类水平约 65%±5%）。这对情绪趋势记录足够，单帧别太较真。

## 想再提高准确率？

训练脚本已内置：强数据增强（翻转/旋转/平移/亮度对比度/cutout）、标签平滑、
余弦退火、类别加权、BatchNorm 头。还能再做：

1. **多训几轮**：`set EMOTION_EPOCHS=80` 再跑（默认 60）。
2. **用标准完整 FER2013**（28709 训练，比默认的 enhanced 镜像多 ~3500 张）：
   去 Kaggle 下 `fer2013.csv`，放到 `tools/emotion/data/fer2013.csv`，
   或 `set FER2013_CSV=路径`。脚本会优先用本地 CSV。
3. **设备端时间投票**（最划算，第二阶段做）：一个采样窗口里多帧取众数，
   即使单帧 60%，整段情绪判断会准得多——这正是情绪日报需要的。

> 注意：**不要在训练里加设备端没有的预处理**（如直方图均衡），否则训练/部署
> 不一致会让真机更差。归一化必须始终是 `(pixel-127.5)/127.5`。
> 加大模型也能涨点，但会拖慢板子推理 / 占更多 flash，需要时再说。

## 模型与设备端预处理契约

- 输入：`[1,1,48,48]`，单通道，归一化 `(pixel-127.5)/127.5 ∈ [-1,1]`。
  训练、量化校准、设备端 `expression_adapter_esp_dl.cpp` 三处一致。
- 输出：7 个原始 logits（无 softmax），设备端取 argmax。
- 输入尺寸由 `components/lexin_vision/CMakeLists.txt` 的
  `CONFIG_ECHOMATE_EXPRESSION_ESP_DL_INPUT_*` 固定为 48×48 灰度，
  必须与导出模型一致。

## 设备端如何工作（已接好）

- `lexin_vision.c` 每帧抓图 → ESP-WHO 人脸检测 → 裁剪人脸 →
  `expression_adapter_esp_dl.cpp` 跑 ESP-DL 模型 → 7 类 argmax。
- 7 类结果会折叠成旧的三类 `echomate_expression_t`（happy→HAPPY；
  sad/angry/fear/disgust→SAD；neutral/surprise→NEUTRAL）喂给现有 UI 与情绪引擎，
  完整 7 类标签保存在 `expression_esp_dl_status_t.fer_label/fer_scores`，
  并通过 `ESP_LOGI/ESP_LOGD`（TAG=`expression_esp_dl`）打印，便于上板验证。

## 验证（上板后）

串口日志里应出现：
```
expression_esp_dl: ESP-DL expression model ready input=48x48 c=1 ...
expression_esp_dl: ESP-DL expression run input=48x48 c=1 classes=7 ... label=HAPPY conf=.. inf=..us
```
对着摄像头做不同表情，`label=` 应随之变化。

## 第二阶段（情绪日报，尚未实现）

规划：
1. 定时（如每 N 分钟）采样 `fer_label`，写入 NVS 环形日志（时间戳+标签+置信度）。
2. 一天结束做直方图统计，组装上下文文本。
3. 复用现有 DeepSeek 代理（`tools/lexin_proxy.js` + `request_proxy_action`）
   生成情绪日报与建议，显示/保存。

`fer_label` 已经能从适配器拿到，第二阶段只需加「采样+存储+日报」这层。
