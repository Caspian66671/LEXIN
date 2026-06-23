const { spawnSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const displaySource = fs.readFileSync(path.join(root, "main", "lexin_display_test.c"), "utf8");
const extraSymbols = "，。；：、（）【】《》！？“”‘’";
const hanSymbols = [...new Set(displaySource.match(/[\u3400-\u9fff]/g) || [])].join("");
const glyphSeed = "维轮触摸交互专注计时本地输入云端润色桌宠反馈研伴节奏";
const symbols = `${hanSymbols}${extraSymbols}${glyphSeed}`;

const font = "C:\\Windows\\Fonts\\simhei.ttf";
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
    "4",
    "--no-compress",
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
