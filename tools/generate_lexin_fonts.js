const { spawnSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const sourceFiles = [
  path.join(root, "main", "lexin_display_test.c"),
  path.join(root, "components", "lexin_voice", "lexin_voice.c"),
  path.join(root, "tools", "lexin_proxy.js"),
];
const displaySource = sourceFiles
  .filter((file) => fs.existsSync(file))
  .map((file) => fs.readFileSync(file, "utf8"))
  .join("\n");

// Punctuation and a few glyph seeds that must always be present.
const extraSymbols = "，。；：、（）【】《》！？“”‘’…—·～％℃「」";
const glyphSeed = [
  "乐鑫语音对话唤醒词未检测到请先说正在识别等待回复麦克风输入",
  "语音识别电脑代理本地规则云端建议今日计划天气日历情绪研伴",
  "维轮触摸交互专注计时本地输入云端润色桌宠反馈研伴节奏",
].join("");

// Any Han characters that appear literally in the UI source.
const hanSymbols = [...new Set(displaySource.match(/[\u3400-\u9fff]/g) || [])].join("");

// Full GB2312 hanzi set (level 1 + level 2 = 6763 chars). Runtime text such as
// DeepSeek advice, ASR transcripts, weather/lunar API results is arbitrary
// modern Chinese, so the font must cover the whole common charset instead of a
// hand-picked subset (which caused missing-glyph "tofu" boxes).
function buildGb2312Charset() {
  const decoder = new TextDecoder("gb2312", { fatal: false });
  const chars = [];
  for (let hi = 0xb0; hi <= 0xf7; hi++) {
    for (let lo = 0xa1; lo <= 0xfe; lo++) {
      const ch = decoder.decode(new Uint8Array([hi, lo]));
      if (ch && ch.length === 1 && ch !== "\uFFFD") {
        const code = ch.codePointAt(0);
        if (code >= 0x3400 && code <= 0x9fff) {
          chars.push(ch);
        }
      }
    }
  }
  return chars.join("");
}

const gb2312 = buildGb2312Charset();
const symbols = [...new Set([...`${gb2312}${hanSymbols}${extraSymbols}${glyphSeed}`])].join("");

console.log(`[generate_lexin_fonts] GB2312 hanzi: ${[...gb2312].length}, total unique symbols: ${[...symbols].length}`);

const fontCandidates = [
  process.env.LEXIN_FONT_PATH,
  "C:\\Windows\\Fonts\\simhei.ttf",
  "C:\\Windows\\Fonts\\msyh.ttc",
  path.join(root, "managed_components", "lvgl__lvgl", "scripts", "built_in_font", "SourceHanSansSC-Normal.otf"),
  path.join(root, "managed_components", "lvgl__lvgl", "tests", "src", "test_files", "fonts", "noto", "NotoSansSC-Regular.ttf"),
].filter(Boolean);
const font = fontCandidates.find((candidate) => fs.existsSync(candidate));
if (!font) {
  console.error("[generate_lexin_fonts] No Chinese font found. Set LEXIN_FONT_PATH to a .ttf/.otf/.ttc file.");
  process.exit(1);
}
console.log(`[generate_lexin_fonts] font: ${font}`);
const outputs = [
  { size: "20", name: "lexin_cn_20", out: "main\\lexin_cn_20.c" },
  { size: "28", name: "lexin_cn_28", out: "main\\lexin_cn_28.c" },
];

for (const item of outputs) {
  const args = [
    "lv_font_conv",
    "--font",
    font,
    "--size",
    item.size,
    "--bpp",
    "3",
    "--format",
    "lvgl",
    "--lv-include",
    "lvgl.h",
    "--lv-font-name",
    item.name,
    "--range",
    "0x20-0x7E",
    "--symbols",
    symbols,
    "-o",
    item.out,
  ];
  const result = spawnSync("npx", args, {
    cwd: root,
    stdio: "inherit",
    shell: process.platform === "win32",
  });
  if (result.status !== 0) {
    process.exit(result.status || 1);
  }
}
