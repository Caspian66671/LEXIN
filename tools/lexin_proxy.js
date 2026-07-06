const http = require("http");
const dgram = require("dgram");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawn } = require("child_process");

function portFromArgs() {
  const index = process.argv.indexOf("--port");
  if (index >= 0 && process.argv[index + 1]) {
    return process.argv[index + 1];
  }
  return "";
}

function firstExistingPath(paths, fallback) {
  for (const candidate of paths) {
    if (candidate && fs.existsSync(candidate)) return candidate;
  }
  return fallback;
}

const PORT = Number(process.env.PORT || portFromArgs() || 8787);
const DISCOVERY_PORT = PORT + 1;
const ROOT_DIR = path.resolve(__dirname, "..");
const CAPTURE_DIR = path.join(ROOT_DIR, "tools", "emotion", "board_data", "_inbox");
const EMOTION_REPORT_DIR = path.join(ROOT_DIR, "tools", "emotion_reports", "users");
const VOICE_DEBUG_DIR = path.join(ROOT_DIR, "tools", "voice_debug");
/** Daily plan storage: tools/plans/<user_id>/<date>/plan.json */
const PLAN_DIR = path.join(ROOT_DIR, "tools", "plans");
/** Face registration data: one JSON object per user, stored in a single
 *  file. For 2-8 desktop users this is simple and fast enough. */
const FACE_DB_FILE = path.join(ROOT_DIR, "tools", "face_users.json");
const LOCAL_FACE_PYTHON = path.join(ROOT_DIR, ".face_venv", "Scripts", "python.exe");
const DEFAULT_IDF_PYTHON = "D:\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe";
const FACE_EMBED_SCRIPT = path.join(ROOT_DIR, "tools", "face_embedding.py");
const FACE_ONNX_MODEL = process.env.LEXIN_FACE_ONNX_MODEL || path.join(ROOT_DIR, "tools", "face_models", "mobilefacenet.onnx");
const FACE_PYTHON = process.env.LEXIN_FACE_PYTHON ||
  firstExistingPath([LOCAL_FACE_PYTHON, DEFAULT_IDF_PYTHON], "python");
const FACE_COSINE_THRESHOLD = Number(process.env.LEXIN_FACE_COSINE_THRESHOLD || 0.38);
const FACE_COSINE_MARGIN = Number(process.env.LEXIN_FACE_COSINE_MARGIN || 0.06);
const FACE_REGISTER_COSINE_THRESHOLD = Number(process.env.LEXIN_FACE_REGISTER_COSINE_THRESHOLD || 0.50);
const FACE_REGISTER_COSINE_MARGIN = Number(process.env.LEXIN_FACE_REGISTER_COSINE_MARGIN || 0.05);
const FACE_EMBED_TIMEOUT_MS = Number(process.env.LEXIN_FACE_EMBED_TIMEOUT_MS || 12000);
const FACE_HAMMING_THRESHOLD = Number(process.env.LEXIN_FACE_HAMMING_THRESHOLD || 36);
const FACE_MAX_SAMPLES_PER_USER = Number(process.env.LEXIN_FACE_MAX_SAMPLES || 8);
const XIAN_LAT = 34.3416;
const XIAN_LON = 108.9398;
const DEEPSEEK_BASE_URL = (process.env.DEEPSEEK_BASE_URL || "https://api.deepseek.com").replace(/\/$/, "");
const DEEPSEEK_MODEL = process.env.DEEPSEEK_MODEL || "deepseek-chat";
const DEEPSEEK_API_KEY = process.env.DEEPSEEK_API_KEY || "";
const WEATHER_CACHE_MS = 5 * 60 * 1000;
const LIVE_INFO_CACHE_MS = Number(process.env.LEXIN_LIVE_INFO_CACHE_MS || 60 * 1000);
const LIVE_INFO_TIMEOUT_MS = Number(process.env.LEXIN_LIVE_INFO_TIMEOUT_MS || 7000);
const LIVE_INFO_LIMIT = Number(process.env.LEXIN_LIVE_INFO_LIMIT || 5);
const LEXIN_LIVE_INFO_ENABLED = process.env.LEXIN_LIVE_INFO !== "0";
const EMOTION_MOOD_SCORE = {
  happy: 2,
  surprised: 1,
  focused: 0,
  tired: -1,
  stressed: -2,
};
const EMOTION_MOODS = Object.keys(EMOTION_MOOD_SCORE);

/* Voice / ASR configuration. The voice pipeline is intentionally
 * lazy: we never *require* an external ASR model, the on-board
 * WakeNet-equivalent detection and rule-based intent are enough to
 * keep the demo flowing. Hook a real FunASR / Whisper / Paraformer
 * command via LEXIN_ASR_CMD when you are ready. */
const LEXIN_ASR_CMD = process.env.LEXIN_ASR_CMD || "";
const LEXIN_ASR_LANG = process.env.LEXIN_ASR_LANG || "zh";
/* Allow voice traffic to use the same DeepSeek key as /insight, but
 * gated by a separate switch so the demo does not silently burn
 * tokens on background noise. */
/* Voice conversation forwards to DeepSeek by default when a key is set.
 * Set LEXIN_DEEPSEEK_VOICE=0 to force the offline rule engine. */
const LEXIN_DEEPSEEK_VOICE = process.env.LEXIN_DEEPSEEK_VOICE !== "0";
/* Wake keywords. Pinyin or simplified Chinese, both are matched case
 * insensitively. The literal text "乐鑫" and a few common variants
 * cover the "lexin / 乐鑫 / 乐心" speech shapes. */
const LEXIN_WAKE_KEYWORDS = (process.env.LEXIN_WAKE_KEYWORDS
  || "乐鑫,乐心,乐新,乐星,乐信,lexin,lex i n,lexinlexin")
  .split(/[,，\s]+/).map((s) => s.trim().toLowerCase()).filter(Boolean);
const LEXIN_EXTRA_WAKE_KEYWORDS = [
  "\u4e50\u946b",
  "\u4e50\u5fc3",
  "\u4e50\u65b0",
  "\u4e50\u661f",
  "\u4e50\u4fe1",
  "lexin",
  "le xin",
  "lex in",
  "lexinlexin",
];
for (const kw of LEXIN_EXTRA_WAKE_KEYWORDS) {
  const normalized = kw.trim().toLowerCase();
  if (normalized && !LEXIN_WAKE_KEYWORDS.includes(normalized)) {
    LEXIN_WAKE_KEYWORDS.push(normalized);
  }
}
for (const weak of ["i", "n"]) {
  let index = LEXIN_WAKE_KEYWORDS.indexOf(weak);
  while (index >= 0) {
    LEXIN_WAKE_KEYWORDS.splice(index, 1);
    index = LEXIN_WAKE_KEYWORDS.indexOf(weak);
  }
}

/* After a wake word is heard we open a short conversation window. Any
 * speech that arrives inside the window is treated as a continuation of
 * the same conversation and does not need the wake word again. */
const LEXIN_VOICE_SESSION_MS = Number(process.env.LEXIN_VOICE_SESSION_MS || 60000);
/* How many recent (user, assistant) turns to send to DeepSeek as context
 * so the reply feels like a real conversation. */
const LEXIN_VOICE_HISTORY_TURNS = Number(process.env.LEXIN_VOICE_HISTORY_TURNS || 6);
/* Hard-coded replies for common short intents. Anything not matched
 * here goes to the local rule engine or to DeepSeek (if enabled). */
const LEXIN_VOICE_FAQ = {
  "你好": "你好呀，我在这里。",
  "你好呀": "你好呀，今天想做点什么？",
  "在吗": "我在的，你说。",
  "你是谁": "我是乐鑫 ESP32-P4 桌宠。",
  "你叫什么": "我叫乐鑫桌宠。",
  "谢谢": "不客气。",
  "再见": "好的，回见。",
  "晚安": "早点休息，明天见。",
};
/* Patterns that should be forwarded to DeepSeek even if a FAQ match
 * exists. Mostly things that need a real model. */
const LEXIN_VOICE_FORWARD_HINTS = [
  "为什么", "怎么", "如何", "推荐", "解释", "总结", "代码", "公式",
  "写一个", "帮我", "介绍", "什么是", "区别", "why", "how", "what",
  "explain", "summarize", "recommend",
];

let weatherCache = null;
let weatherCacheExpiresAt = 0;
let weatherRefreshPromise = null;

/* Per-client voice conversation state. Keyed by the board-supplied user
 * id (x-lexin-user-id) or "device" for the single-board demo. */
const voiceSessions = new Map();   // key -> session expiry (ms epoch)
const voiceHistory = new Map();    // key -> [{ role, content }, ...]

function voiceSessionActive(key) {
  const expiry = voiceSessions.get(key) || 0;
  return Date.now() < expiry;
}

function voiceSessionTouch(key) {
  voiceSessions.set(key, Date.now() + LEXIN_VOICE_SESSION_MS);
}

function voiceHistoryGet(key) {
  return voiceHistory.get(key) || [];
}

function voiceHistoryPush(key, userText, assistantText) {
  const list = voiceHistory.get(key) || [];
  if (userText) list.push({ role: "user", content: userText });
  if (assistantText) list.push({ role: "assistant", content: assistantText });
  // Keep only the most recent N turns (2 messages per turn).
  const maxMessages = LEXIN_VOICE_HISTORY_TURNS * 2;
  while (list.length > maxMessages) list.shift();
  voiceHistory.set(key, list);
}

function send(res, status, body) {
  res.writeHead(status, {
    "content-type": "text/plain; charset=utf-8",
    "cache-control": "no-store",
    "access-control-allow-origin": "*",
  });
  res.end(body);
}

function readRequestBody(req, maxBytes) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let length = 0;
    req.on("data", (chunk) => {
      length += chunk.length;
      if (length > maxBytes) {
        reject(new Error("capture body too large"));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => resolve(Buffer.concat(chunks, length)));
    req.on("error", reject);
  });
}

function timestampForFile() {
  const now = beijingNow();
  const ms = String(now.getMilliseconds()).padStart(3, "0");
  return [
    now.getFullYear(),
    pad2(now.getMonth() + 1),
    pad2(now.getDate()),
    "_",
    pad2(now.getHours()),
    pad2(now.getMinutes()),
    pad2(now.getSeconds()),
    "_",
    ms,
  ].join("");
}

function rgb565leToPpm(width, height, body) {
  const pixelCount = width * height;
  if (body.length < pixelCount * 2) {
    throw new Error(`short RGB565 body: ${body.length}/${pixelCount * 2}`);
  }
  const header = Buffer.from(`P6\n${width} ${height}\n255\n`, "ascii");
  const rgb = Buffer.alloc(pixelCount * 3);
  for (let i = 0, j = 0; i < pixelCount; i++, j += 3) {
    const value = body[i * 2] | (body[i * 2 + 1] << 8);
    const r5 = (value >> 11) & 0x1f;
    const g6 = (value >> 5) & 0x3f;
    const b5 = value & 0x1f;
    rgb[j] = (r5 << 3) | (r5 >> 2);
    rgb[j + 1] = (g6 << 2) | (g6 >> 4);
    rgb[j + 2] = (b5 << 3) | (b5 >> 2);
  }
  return Buffer.concat([header, rgb]);
}

async function saveCapture(req) {
  const width = Number(req.headers["x-lexin-width"] || 0);
  const height = Number(req.headers["x-lexin-height"] || 0);
  const format = String(req.headers["x-lexin-format"] || "").toLowerCase();
  const frameId = String(req.headers["x-lexin-frame-id"] || "0").replace(/[^0-9]/g, "") || "0";
  if (width !== 288 || height !== 216 || format !== "rgb565le") {
    throw new Error(`unsupported capture ${width}x${height} ${format}`);
  }

  const body = await readRequestBody(req, width * height * 2 + 1024);
  fs.mkdirSync(CAPTURE_DIR, { recursive: true });
  const filename = `capture_${timestampForFile()}_frame${frameId}.ppm`;
  const outPath = path.join(CAPTURE_DIR, filename);
  fs.writeFileSync(outPath, rgb565leToPpm(width, height, body));
  console.log(`Saved board capture: ${outPath}`);
  return outPath;
}

function beijingNow() {
  return new Date(new Date().toLocaleString("en-US", { timeZone: "Asia/Shanghai" }));
}

function pad2(value) {
  return String(value).padStart(2, "0");
}

function weatherName(code) {
  if ([0, 1].includes(code)) return "SUNNY";
  if ([2, 3, 45, 48].includes(code)) return "CLOUDY";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || [95, 96, 99].includes(code)) return "RAIN";
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "SNOW";
  return "CLOUDY";
}

function adviceFor(temp, rain, weather) {
  if (weather === "RAIN" || rain >= 50) return "UMBRELLA";
  if (temp >= 30) return "HOT";
  if (temp <= 10) return "COLD";
  return "GOOD";
}

function nearestRainProbability(data) {
  const times = data.hourly?.time || [];
  const probs = data.hourly?.precipitation_probability || [];
  if (times.length === 0 || probs.length === 0) return "UNKNOWN";

  const now = beijingNow();
  let bestIndex = 0;
  let bestDelta = Number.MAX_SAFE_INTEGER;
  for (let i = 0; i < times.length; i++) {
    const delta = Math.abs(new Date(times[i]).getTime() - now.getTime());
    if (delta < bestDelta) {
      bestDelta = delta;
      bestIndex = i;
    }
  }
  return Number.isFinite(probs[bestIndex]) ? `${probs[bestIndex]}%` : "UNKNOWN";
}

async function fetchWeatherData() {
  try {
    const url = new URL("https://api.open-meteo.com/v1/forecast");
    url.searchParams.set("latitude", String(XIAN_LAT));
    url.searchParams.set("longitude", String(XIAN_LON));
    url.searchParams.set("current_weather", "true");
    url.searchParams.set("hourly", "precipitation_probability");
    url.searchParams.set("forecast_days", "1");
    url.searchParams.set("timezone", "Asia/Shanghai");

    const response = await fetch(url, { signal: AbortSignal.timeout(4500) });
    if (!response.ok) throw new Error(`weather status ${response.status}`);
    const data = await response.json();
    const current = data.current_weather || {};
    const temp = Math.round(Number(current.temperature));
    const weather = weatherName(Number(current.weathercode));
    const rain = nearestRainProbability(data);
    const rainNumber = Number(String(rain).replace("%", ""));
    const advice = adviceFor(temp, Number.isFinite(rainNumber) ? rainNumber : 0, weather);
    return {
      temp: Number.isFinite(temp) ? `${temp}C` : "UNKNOWN",
      weather,
      rain,
      advice,
    };
  } catch (error) {
    console.warn(`weather fallback: ${error.message}`);
    return {
      temp: "UNKNOWN",
      weather: "UNKNOWN",
      rain: "UNKNOWN",
      advice: "CHECK_NETWORK",
    };
  }
}

async function weatherData() {
  const now = Date.now();
  if (weatherCache && now < weatherCacheExpiresAt) return weatherCache;
  if (weatherRefreshPromise) return weatherRefreshPromise;

  weatherRefreshPromise = fetchWeatherData()
    .then((data) => {
      if (data.weather !== "UNKNOWN") {
        weatherCache = data;
        weatherCacheExpiresAt = Date.now() + WEATHER_CACHE_MS;
      } else if (weatherCache) {
        return weatherCache;
      }
      return data;
    })
    .finally(() => {
      weatherRefreshPromise = null;
    });
  return weatherRefreshPromise;
}

function weatherText(data) {
  return [
    `TEMP: ${data.temp}`,
    `WEATHER: ${data.weather}`,
    `RAIN: ${data.rain}`,
    `ADVICE: ${data.advice}`,
  ].join("\n");
}

function holidayFor(date) {
  const key = `${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
  const fixed = {
    "01-01": "New Year",
    "05-01": "Labor Day",
    "10-01": "National Day",
  };
  return fixed[key] || "NONE";
}

function lunarText(date) {
  try {
    const formatted = new Intl.DateTimeFormat("zh-CN-u-ca-chinese", {
      month: "long",
      day: "numeric",
    }).format(date);
    if (!formatted) return "UNKNOWN";
    const digits = {
      "正": "zheng",
      "一": "yi",
      "二": "er",
      "三": "san",
      "四": "si",
      "五": "wu",
      "六": "liu",
      "七": "qi",
      "八": "ba",
      "九": "jiu",
      "十": "shi",
      "冬": "dong",
      "腊": "la",
    };
    const monthMatch = formatted.match(/([闰正一二三四五六七八九十冬腊]+)月/);
    const dayMatch = formatted.match(/(初|十|廿|卅)([一二三四五六七八九十]?)/);
    const numericDayMatch = formatted.match(/(\d{1,2})日/);
    if (!monthMatch || (!dayMatch && !numericDayMatch)) return "UNKNOWN";

    const month = Array.from(monthMatch[1])
      .map((ch) => ch === "闰" ? "run" : digits[ch])
      .filter(Boolean)
      .join(" ");
    let day = "";
    if (numericDayMatch) {
      const value = Number(numericDayMatch[1]);
      const ones = ["", "yi", "er", "san", "si", "wu", "liu", "qi", "ba", "jiu"];
      if (value < 10) day = ones[value];
      else if (value === 10) day = "shi";
      else if (value < 20) day = `shi ${ones[value - 10]}`;
      else if (value === 20) day = "er shi";
      else if (value < 30) day = `er shi ${ones[value - 20]}`;
      else if (value === 30) day = "san shi";
    } else if (dayMatch[1] === "初") {
      day = digits[dayMatch[2]];
    } else if (dayMatch[1] === "十") {
      day = dayMatch[2] ? `shi ${digits[dayMatch[2]]}` : "shi";
    } else if (dayMatch[1] === "廿") {
      day = dayMatch[2] ? `er shi ${digits[dayMatch[2]]}` : "er shi";
    } else if (dayMatch[1] === "卅") {
      day = dayMatch[2] ? `san shi ${digits[dayMatch[2]]}` : "san shi";
    }
    return month && day ? `${month} yue ${day}` : "UNKNOWN";
  } catch {
    return "UNKNOWN";
  }
}

function timeData() {
  const now = beijingNow();
  const weekday = now.getDay();
  const holiday = holidayFor(now);
  const isWeekend = weekday === 0 || weekday === 6;
  const isHoliday = holiday !== "NONE";
  return {
    time: `${pad2(now.getHours())}:${pad2(now.getMinutes())}`,
    date: `${now.getFullYear()}-${pad2(now.getMonth() + 1)}-${pad2(now.getDate())}`,
    lunar: lunarText(now),
    holiday,
    weekday,
    isWeekend,
    isHoliday,
    dayType: isHoliday ? "HOLIDAY" : isWeekend ? "WEEKEND" : "WORKDAY",
    hour: now.getHours(),
  };
}

function timeText(data) {
  return [
    `TIME: ${data.time}`,
    `DATE: ${data.date}`,
    `LUNAR: ${data.lunar}`,
    `HOLIDAY: ${data.holiday}`,
    `DAY_TYPE: ${data.dayType}`,
  ].join("\n");
}

function riskFromWeather(weather, rain) {
  const rainNumber = Number(String(rain).replace("%", ""));
  if (weather === "RAIN" || Number.isFinite(rainNumber) && rainNumber >= 50) return "HIGH";
  if (weather === "UNKNOWN") return "MEDIUM";
  return "LOW";
}

function localInsight(weather, time) {
  const risk = riskFromWeather(weather.weather, weather.rain);
  const isRestDay = time.isHoliday || time.isWeekend || time.dayType === "HOLIDAY" || time.dayType === "WEEKEND";
  if (risk === "HIGH") {
    return {
      model: "LOCAL",
      insight: "UMBRELLA",
      risk,
      basis: `${time.dayType} WEATHER_RAIN LOCAL_RULE`,
    };
  }

  if (isRestDay) {
    if (time.hour >= 7 && time.hour < 11) {
      return {
        model: "LOCAL",
        insight: "EXERCISE",
        risk,
        basis: `${time.dayType} MORNING_EXERCISE LOCAL_RULE`,
      };
    }
    if (time.hour >= 11 && time.hour < 14) {
      return {
        model: "LOCAL",
        insight: "LUNCH",
        risk,
        basis: `${time.dayType} LUNCH_TIME LOCAL_RULE`,
      };
    }
    if (time.hour >= 14 && time.hour < 18) {
      return {
        model: "LOCAL",
        insight: "REST",
        risk,
        basis: `${time.dayType} AFTERNOON_REST LOCAL_RULE`,
      };
    }
    if (time.hour >= 18 && time.hour < 22) {
      return {
        model: "LOCAL",
        insight: "EXERCISE",
        risk,
        basis: `${time.dayType} EVENING_EXERCISE LOCAL_RULE`,
      };
    }
    return {
      model: "LOCAL",
      insight: "SLEEP",
      risk,
      basis: `${time.dayType} SLEEP_TIME LOCAL_RULE`,
    };
  }

  if (time.hour >= 6 && time.hour < 9) {
    return {
      model: "LOCAL",
      insight: "BREAKFAST",
      risk,
      basis: "WORKDAY BREAKFAST_TIME LOCAL_RULE",
    };
  }
  if (time.hour >= 9 && time.hour < 11) {
    return {
      model: "LOCAL",
      insight: "RESEARCH_FOCUS",
      risk,
      basis: "WORKDAY FOCUS_STUDY LOCAL_RULE",
    };
  }
  if (time.hour >= 11 && time.hour < 14) {
    return {
      model: "LOCAL",
      insight: "LUNCH",
      risk,
      basis: "WORKDAY LUNCH_TIME LOCAL_RULE",
    };
  }
  if (time.hour >= 14 && time.hour < 17) {
    return {
      model: "LOCAL",
      insight: "PAPER_READING",
      risk,
      basis: "WORKDAY PAPER_NOTE LOCAL_RULE",
    };
  }
  if (time.hour >= 17 && time.hour < 19) {
    return {
      model: "LOCAL",
      insight: "DINNER",
      risk,
      basis: "WORKDAY DINNER_TIME LOCAL_RULE",
    };
  }
  if (time.hour >= 19 && time.hour < 22) {
    return {
      model: "LOCAL",
      insight: "PLAN",
      risk,
      basis: "WORKDAY EVENING_REVIEW LOCAL_RULE",
    };
  }
  if (time.hour >= 22 || time.hour < 6) {
    return {
      model: "LOCAL",
      insight: "SLEEP",
      risk,
      basis: "WORKDAY SLEEP_TIME LOCAL_RULE",
    };
  }

  if (weather.advice === "HOT") {
    return {
      model: "LOCAL",
      insight: "HYDRATE",
      risk,
      basis: "WORKDAY WEATHER_HOT LOCAL_RULE",
    };
  }

  return {
    model: "LOCAL",
    insight: "PLAN",
    risk,
    basis: "WORKDAY TIME_STATUS LOCAL_RULE",
  };
}

const INSIGHT_CHOICES = [
  "BREAKFAST",
  "LUNCH",
  "DINNER",
  "RESEARCH_FOCUS",
  "PAPER_READING",
  "WRITE_THESIS",
  "EXERCISE",
  "REST",
  "SLEEP",
  "UMBRELLA",
  "HYDRATE",
  "PLAN",
  "SUN",
  "STABLE",
];

const DEEPSEEK_SYSTEM_PROMPT = [
  "You are the DeepSeek model powering a smart desktop pet for an electronics-information master's student.",
  "Return JSON only, with no explanation.",
  `insight must be one of: ${INSIGHT_CHOICES.join("/")}.`,
  "risk must be LOW, MEDIUM, or HIGH.",
  "basis must be short English tags.",
  "Do not assume the student is doing experiments unless explicit experiment data is provided.",
  "On workdays, prioritize health, meals, hydration, focus study, paper reading, note taking, short review, exercise, and sleep.",
  "On weekends or holidays, prioritize rest, exercise, meals, light reading, and gentle planning.",
].join(" ");

function normalizeChoice(value, allowed, fallback) {
  const upper = String(value || "").toUpperCase();
  return allowed.includes(upper) ? upper : fallback;
}

function parseJsonContent(content) {
  const text = String(content || "").trim();
  if (!text) return {};
  try {
    return JSON.parse(text);
  } catch {
    const start = text.indexOf("{");
    const end = text.lastIndexOf("}");
    if (start >= 0 && end > start) {
      return JSON.parse(text.slice(start, end + 1));
    }
    return {};
  }
}

function sanitizePathSegment(value, fallback = "anonymous") {
  const cleaned = String(value || "")
    .trim()
    .replace(/[^a-zA-Z0-9_-]/g, "_")
    .replace(/_+/g, "_")
    .slice(0, 48);
  return cleaned || fallback;
}

function dateKey(date = beijingNow()) {
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
}

function monthKey(date = beijingNow()) {
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}`;
}

function beijingIsoLike(date = beijingNow()) {
  return `${dateKey(date)}T${pad2(date.getHours())}:${pad2(date.getMinutes())}:${pad2(date.getSeconds())}+08:00`;
}

function emotionUserDir(userId) {
  return path.join(EMOTION_REPORT_DIR, sanitizePathSegment(userId));
}

function emotionDayDir(userId, date) {
  return path.join(emotionUserDir(userId), sanitizePathSegment(date, dateKey()));
}

function sampleFileFor(userId, date) {
  return path.join(emotionDayDir(userId, date), "samples.jsonl");
}

function reportFileFor(userId, date) {
  return path.join(emotionDayDir(userId, date), "report.json");
}

function normalizeMood(value) {
  const mood = String(value || "").toLowerCase();
  return EMOTION_MOODS.includes(mood) ? mood : "";
}

function moodCn(mood) {
  return {
    happy: "开心",
    surprised: "惊讶",
    focused: "专注",
    tired: "疲惫",
    stressed: "压力",
  }[mood] || "未知";
}

async function handleEmotionLog(req) {
  const body = await readRequestBody(req, 8192);
  const parsed = JSON.parse(body.toString("utf8") || "{}");
  const mood = normalizeMood(parsed.mood);
  if (!mood) {
    return { status: 400, body: { ok: false, error: "invalid mood" } };
  }
  if (parsed.face_detected === false) {
    return { status: 202, body: { ok: true, skipped: "no face" } };
  }

  const now = beijingNow();
  const userId = sanitizePathSegment(parsed.user_id, "anonymous");
  const date = dateKey(now);
  const sample = {
    user_id: userId,
    user_name: String(parsed.user_name || "").slice(0, 64),
    source: String(parsed.source || "board").slice(0, 24),
    mood,
    mood_confidence: clampNumber(parsed.mood_confidence, 0, 100, 0),
    face_detected: true,
    frame_id: clampNumber(parsed.frame_id, 0, Number.MAX_SAFE_INTEGER, 0),
    inference_ms: clampNumber(parsed.inference_ms, 0, 60000, 0),
    camera_fps_x10: clampNumber(parsed.camera_fps_x10, 0, 1000, 0),
    device_ms: clampNumber(parsed.device_ms, 0, Number.MAX_SAFE_INTEGER, 0),
    server_time: beijingIsoLike(now),
  };

  const dir = emotionDayDir(userId, date);
  fs.mkdirSync(dir, { recursive: true });
  fs.appendFileSync(sampleFileFor(userId, date), `${JSON.stringify(sample)}\n`, "utf8");
  return { status: 200, body: { ok: true, date, path: sampleFileFor(userId, date) } };
}

function clampNumber(value, min, max, fallback) {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  return Math.max(min, Math.min(max, Math.round(n)));
}

function readEmotionSamples(userId, date) {
  const file = sampleFileFor(userId, date);
  if (!fs.existsSync(file)) return [];
  return fs.readFileSync(file, "utf8")
    .split(/\r?\n/)
    .filter(Boolean)
    .map((line) => {
      try { return JSON.parse(line); } catch { return null; }
    })
    .filter((sample) => sample && normalizeMood(sample.mood));
}

function summarizeEmotionSamples(samples) {
  const counts = Object.fromEntries(EMOTION_MOODS.map((mood) => [mood, 0]));
  const hourlySum = Array(24).fill(0);
  const hourlyCount = Array(24).fill(0);
  let totalScore = 0;

  for (const sample of samples) {
    const mood = normalizeMood(sample.mood);
    if (!mood) continue;
    const score = EMOTION_MOOD_SCORE[mood];
    counts[mood]++;
    totalScore += score;
    const hour = Number(String(sample.server_time || "").slice(11, 13));
    if (Number.isInteger(hour) && hour >= 0 && hour < 24) {
      hourlySum[hour] += score;
      hourlyCount[hour]++;
    }
  }

  let dominant = "focused";
  for (const mood of EMOTION_MOODS) {
    if (counts[mood] > counts[dominant]) dominant = mood;
  }
  const count = samples.length;
  const avgScore = count ? totalScore / count : 0;
  const hourlyScores = hourlySum.map((sum, i) =>
    hourlyCount[i] ? Math.round(sum / hourlyCount[i]) : 0);
  return {
    count,
    counts,
    dominant,
    avgScore,
    stressTiredRatio: count ? (counts.stressed + counts.tired) / count : 0,
    happyFocusedRatio: count ? (counts.happy + counts.focused) / count : 0,
    hourlyScores,
  };
}

function localEmotionAdvice(summary, periodName) {
  if (summary.count === 0) return "今天还没有足够的情绪记录，先保持自然使用，稍后再查看报告。";
  if (summary.stressTiredRatio >= 0.45) {
    return `${periodName}压力或疲惫占比较高，建议安排一次短休息、补水，并把接下来的任务拆成更小步骤。`;
  }
  if (summary.counts.happy >= summary.counts.stressed + summary.counts.tired &&
      summary.counts.happy > 0) {
    return `${periodName}积极情绪比较明显，可以把高专注任务放在这段状态较好的时间继续推进。`;
  }
  if (summary.counts.focused >= summary.count / 2) {
    return `${periodName}整体偏专注，建议保持当前节奏，每 45-60 分钟主动休息一次。`;
  }
  return `${periodName}情绪波动不算极端，建议记录触发情绪变化的场景，逐步找到适合自己的学习节奏。`;
}

async function deepseekEmotionAdvice(kind, payload, fallback) {
  if (!DEEPSEEK_API_KEY) return { model: "LOCAL", advice: fallback };
  try {
    const answer = await deepseekChat([
      {
        role: "system",
        content: "你是一个温和、克制的学习陪伴助手。根据情绪统计给中文建议，80字以内，不做医学诊断，不夸大风险。",
      },
      {
        role: "user",
        content: JSON.stringify({ kind, payload }),
      },
    ]);
    return { model: "DEEPSEEK", advice: answer.replace(/[\r\n]+/g, " ").slice(0, 180) };
  } catch (error) {
    console.warn(`emotion report deepseek fallback: ${error.message}`);
    return { model: "LOCAL", advice: fallback };
  }
}

function emotionReportText(report) {
  return [
    `MODEL: ${report.model}`,
    `TYPE: ${report.type}`,
    `DATE: ${report.date || ""}`,
    `MONTH: ${report.month || ""}`,
    `SAMPLES: ${report.samples}`,
    `DOMINANT: ${report.dominant}`,
    `DOMINANT_CN: ${moodCn(report.dominant)}`,
    `AVG_SCORE: ${report.avg_score}`,
    `STRESS_TIRED: ${report.stress_tired_percent}%`,
    `HAPPY_FOCUSED: ${report.happy_focused_percent}%`,
    `SCORES: ${report.scores.join(",")}`,
    `ADVICE: ${report.advice}`,
  ].join("\n");
}

async function handleEmotionReport(req) {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const userId = sanitizePathSegment(url.searchParams.get("user_id"), "anonymous");
  const date = sanitizePathSegment(url.searchParams.get("date"), dateKey());
  const samples = readEmotionSamples(userId, date);
  const summary = summarizeEmotionSamples(samples);
  const fallback = localEmotionAdvice(summary, "今天");
  const cloud = await deepseekEmotionAdvice("daily", { userId, date, summary }, fallback);
  const report = {
    type: "daily",
    date,
    user_id: userId,
    model: cloud.model,
    samples: summary.count,
    dominant: summary.dominant,
    avg_score: Number(summary.avgScore.toFixed(2)),
    stress_tired_percent: Math.round(summary.stressTiredRatio * 100),
    happy_focused_percent: Math.round(summary.happyFocusedRatio * 100),
    counts: summary.counts,
    scores: summary.hourlyScores,
    advice: cloud.advice,
    generated_at: beijingIsoLike(),
  };
  fs.mkdirSync(emotionDayDir(userId, date), { recursive: true });
  fs.writeFileSync(reportFileFor(userId, date), JSON.stringify(report, null, 2), "utf8");
  return emotionReportText(report);
}

async function handleEmotionMonth(req) {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const userId = sanitizePathSegment(url.searchParams.get("user_id"), "anonymous");
  const month = sanitizePathSegment(url.searchParams.get("month"), monthKey());
  const base = emotionUserDir(userId);
  const days = fs.existsSync(base)
    ? fs.readdirSync(base).filter((name) => name.startsWith(month)).sort()
    : [];
  const daily = days.map((day) => {
    const summary = summarizeEmotionSamples(readEmotionSamples(userId, day));
    return { day, ...summary };
  }).filter((item) => item.count > 0);
  const samples = daily.reduce((sum, item) => sum + item.count, 0);
  const avgScore = daily.length
    ? daily.reduce((sum, item) => sum + item.avgScore, 0) / daily.length
    : 0;
  const scores = daily.map((item) => Math.round(item.avgScore));
  const stressDays = daily.filter((item) => item.stressTiredRatio >= 0.45).length;
  const merged = summarizeEmotionSamples(days.flatMap((day) => readEmotionSamples(userId, day)));
  const fallback = localEmotionAdvice(merged, "本月");
  const cloud = await deepseekEmotionAdvice("monthly", { userId, month, daily }, fallback);
  const report = {
    type: "monthly",
    month,
    user_id: userId,
    model: cloud.model,
    samples,
    days: daily.map((item) => item.day),
    dominant: merged.dominant,
    avg_score: Number(avgScore.toFixed(2)),
    stress_days: stressDays,
    stress_tired_percent: Math.round(merged.stressTiredRatio * 100),
    happy_focused_percent: Math.round(merged.happyFocusedRatio * 100),
    scores,
    advice: cloud.advice,
    generated_at: beijingIsoLike(),
  };
  fs.mkdirSync(path.join(base, month), { recursive: true });
  fs.writeFileSync(path.join(base, month, "month_report.json"),
                   JSON.stringify(report, null, 2), "utf8");
  return emotionReportText(report);
}

async function deepseekInsight(weather, time) {
  if (!DEEPSEEK_API_KEY) {
    return localInsight(weather, time);
  }

  try {
    const response = await fetch(`${DEEPSEEK_BASE_URL}/chat/completions`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        authorization: `Bearer ${DEEPSEEK_API_KEY}`,
      },
      body: JSON.stringify({
        model: DEEPSEEK_MODEL,
        temperature: 0.2,
        max_tokens: 120,
        thinking: { type: "disabled" },
        response_format: { type: "json_object" },
        messages: [
          {
            role: "system",
            content: DEEPSEEK_SYSTEM_PROMPT,
          },
          {
            role: "user",
            content: JSON.stringify({
              weather,
              time,
              user_profile: "electronics_information_master_student",
              task: "Pick the best daily companion category from weather and calendar only. Prefer healthy routine, focused study, paper reading, notes, review, exercise, meals, or rest.",
            }),
          },
        ],
      }),
      signal: AbortSignal.timeout(9000),
    });
    if (!response.ok) throw new Error(`deepseek status ${response.status}`);
    const data = await response.json();
    const content = data.choices?.[0]?.message?.content || "";
    const parsed = parseJsonContent(content);
    return {
      model: "DEEPSEEK",
      insight: normalizeChoice(parsed.insight, INSIGHT_CHOICES, "STABLE"),
      risk: normalizeChoice(parsed.risk, ["LOW", "MEDIUM", "HIGH"], "LOW"),
      basis: String(parsed.basis || "DEEPSEEK WEATHER TIME").replace(/[\r\n:]/g, " ").slice(0, 80),
    };
  } catch (error) {
    console.warn(`deepseek fallback: ${error.message}`);
    const fallback = localInsight(weather, time);
    return {
      ...fallback,
      basis: `${fallback.basis} DEEPSEEK_FALLBACK`,
    };
  }
}

function insightText(data) {
  return [
    `MODEL: ${data.model}`,
    `INSIGHT: ${data.insight}`,
    `RISK: ${data.risk}`,
    `BASIS: ${data.basis}`,
  ].join("\n");
}

async function edgeContextText(weather, time) {
  const cloud = await deepseekInsight(weather, time);
  return [
    `WEATHER: ${weather.weather}`,
    `TEMP: ${weather.temp}`,
    `RAIN: ${weather.rain}`,
    `ADVICE: ${weather.advice}`,
    `TIME: ${time.time}`,
    `DATE: ${time.date}`,
    `LUNAR: ${time.lunar}`,
    `HOLIDAY: ${time.holiday}`,
    `HOUR: ${time.hour}`,
    `DAY_TYPE: ${time.dayType}`,
    `CLOUD_MODEL: ${cloud.model}`,
    `CLOUD_INSIGHT: ${cloud.insight}`,
    `CLOUD_RISK: ${cloud.risk}`,
    `CLOUD_BASIS: ${cloud.basis}`,
  ].join("\n");
}

/* ----------------------------------------------------------------
 * Face recognition pipeline
 *
 * The board uploads a cropped face (RGB565 raw pixels, with width &
 * height in HTTP headers). The proxy computes a 64-bit average hash
 * and compares it against previously registered users.  When no
 * match is found the board shows a registration screen where the
 * user types a name; the name is then posted to /face-register.
 * --------------------------------------------------------------- */

function loadFaceDb() {
  try {
    if (fs.existsSync(FACE_DB_FILE)) {
      return JSON.parse(fs.readFileSync(FACE_DB_FILE, "utf8"));
    }
  } catch (_) { /* corrupted file → start fresh */ }
  return {};
}

function saveFaceDb(db) {
  fs.writeFileSync(FACE_DB_FILE, JSON.stringify(db, null, 2), "utf8");
}

function ensureFaceDbFile() {
  if (!fs.existsSync(FACE_DB_FILE)) {
    saveFaceDb({});
  }
}

/**
 * Compute a 64-bit average hash from raw RGB565 pixel data.
 * Steps: convert to grayscale → downsample to 8×8 → compare each
 * pixel to the mean → pack into a 64-bit integer.
 */
function rgb565ToGray(r, g, b) {
  return Math.round(0.299 * r + 0.587 * g + 0.114 * b);
}

function averageHash64(rgb565, width, height) {
  // Downsample to 8×8
  const gray = new Array(64);
  const stepX = width / 8;
  const stepY = height / 8;
  let sum = 0;
  for (let row = 0; row < 8; row++) {
    for (let col = 0; col < 8; col++) {
      const sx = Math.floor(col * stepX);
      const sy = Math.floor(row * stepY);
      const idx = (sy * width + sx) * 2;
      if (idx + 1 < rgb565.length) {
        const val = rgb565[idx] | (rgb565[idx + 1] << 8);
        const r = ((val >> 11) & 0x1f) << 3;
        const g = ((val >> 5) & 0x3f) << 2;
        const b = (val & 0x1f) << 3;
        const gv = rgb565ToGray(r, g, b);
        gray[row * 8 + col] = gv;
        sum += gv;
      }
    }
  }
  const mean = sum / 64;
  let hash = 0n;
  for (let i = 0; i < 64; i++) {
    if (gray[i] >= mean) hash |= (1n << BigInt(63 - i));
  }
  return hash;
}

function hammingDistance(a, b) {
  let xor = a ^ b;
  let dist = 0;
  while (xor > 0n) {
    dist += Number(xor & 1n);
    xor >>= 1n;
  }
  return dist;
}

function faceHashes(entry) {
  const hashes = [];
  if (entry && Array.isArray(entry.hashes)) {
    for (const h of entry.hashes) {
      if (typeof h === "string" || typeof h === "bigint") hashes.push(BigInt(h));
    }
  }
  if (entry && (typeof entry.hash === "string" || typeof entry.hash === "bigint")) {
    hashes.push(BigInt(entry.hash));
  }
  return hashes;
}

function findBestMatch(hash, db) {
  let bestUser = null;
  let bestDist = 65;
  for (const id of Object.keys(db)) {
    const entry = db[id];
    for (const storedHash of faceHashes(entry)) {
      const dist = hammingDistance(hash, storedHash);
      if (dist < bestDist) {
        bestDist = dist;
        bestUser = { id, name: entry.name, dist };
      }
    }
  }
  if (bestUser) {
    console.log(`Face best match: ${bestUser.name || bestUser.id} dist=${bestDist} threshold=${FACE_HAMMING_THRESHOLD}`);
  } else {
    console.log("Face best match: no users in database");
  }
  if (bestUser && bestDist <= FACE_HAMMING_THRESHOLD) {
    return bestUser;
  }
  return null;
}

function cosineSimilarity(a, b) {
  if (!Array.isArray(a) || !Array.isArray(b) || a.length === 0 || a.length !== b.length) {
    return -1;
  }
  let dot = 0;
  let na = 0;
  let nb = 0;
  for (let i = 0; i < a.length; i++) {
    const av = Number(a[i]);
    const bv = Number(b[i]);
    dot += av * bv;
    na += av * av;
    nb += bv * bv;
  }
  if (na <= 1e-12 || nb <= 1e-12) return -1;
  return dot / Math.sqrt(na * nb);
}

function faceEmbeddings(entry) {
  if (!entry || !Array.isArray(entry.embeddings)) return [];
  return entry.embeddings
    .map((e) => {
      if (Array.isArray(e)) return e;
      if (e && Array.isArray(e.value)) return e.value;
      return null;
    })
    .filter((e) => Array.isArray(e) && e.length > 0);
}

function findBestEmbeddingMatch(embedding, db, options = {}) {
  const threshold = Number(options.threshold ?? FACE_COSINE_THRESHOLD);
  const margin = Number(options.margin ?? FACE_COSINE_MARGIN);
  let bestUser = null;
  let bestScore = -1;
  let secondUser = null;
  let secondScore = -1;
  for (const id of Object.keys(db)) {
    const entry = db[id];
    for (const stored of faceEmbeddings(entry)) {
      const score = cosineSimilarity(embedding, stored);
      if (score > bestScore) {
        if (!bestUser || bestUser.id !== id) {
          secondUser = bestUser;
          secondScore = bestScore;
        }
        bestScore = score;
        bestUser = { id, name: entry.name, score };
      } else if (id !== (bestUser && bestUser.id) && score > secondScore) {
        secondScore = score;
        secondUser = { id, name: entry.name, score };
      }
    }
  }
  if (bestUser) {
    const runnerUp = secondUser
      ? `${secondUser.name || secondUser.id} score=${secondScore.toFixed(3)}`
      : "none";
    console.log(`Face embedding best match: ${bestUser.name || bestUser.id} score=${bestScore.toFixed(3)} threshold=${threshold} margin=${margin} second=${runnerUp}`);
  } else {
    console.log("Face embedding best match: no embedding users in database");
  }
  if (bestUser &&
      bestScore >= threshold &&
      (!secondUser || bestScore - secondScore >= margin)) {
    return bestUser;
  }
  return null;
}

function faceDbHasEmbeddings(db) {
  return Object.values(db).some((entry) => faceEmbeddings(entry).length > 0);
}

function computeFaceEmbedding(body, width, height) {
  return new Promise((resolve, reject) => {
    if (!fs.existsSync(FACE_EMBED_SCRIPT)) {
      reject(new Error(`face embedding script not found: ${FACE_EMBED_SCRIPT}`));
      return;
    }
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "lexin-face-"));
    const rawPath = path.join(tmpDir, "face.rgb565");
    fs.writeFileSync(rawPath, body);
    const args = [
      FACE_EMBED_SCRIPT,
      "--input", rawPath,
      "--width", String(width),
      "--height", String(height),
      "--model", FACE_ONNX_MODEL,
    ];
    const child = spawn(FACE_PYTHON, args, {
      cwd: ROOT_DIR,
      windowsHide: true,
      env: { ...process.env, LEXIN_FACE_ONNX_MODEL: FACE_ONNX_MODEL },
    });
    const out = [];
    const err = [];
    let done = false;
    const cleanup = () => {
      fs.rm(tmpDir, { recursive: true, force: true }, () => {});
    };
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      child.kill();
      cleanup();
      reject(new Error("face embedding timed out"));
    }, FACE_EMBED_TIMEOUT_MS);
    child.stdout.on("data", (b) => out.push(b));
    child.stderr.on("data", (b) => err.push(b));
    child.on("error", (e) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      cleanup();
      reject(e);
    });
    child.on("close", (code) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      cleanup();
      if (code !== 0) {
        reject(new Error(`face embedding exit ${code}: ${Buffer.concat(err).toString("utf8").trim()}`));
        return;
      }
      try {
        const parsed = JSON.parse(Buffer.concat(out).toString("utf8"));
        if (!parsed.ok || !Array.isArray(parsed.embedding)) {
          reject(new Error(parsed.error || "face embedding returned no vector"));
          return;
        }
        if (parsed.warning) {
          console.warn(`Face embedding fallback: ${parsed.warning}`);
        }
        resolve(parsed);
      } catch (e) {
        reject(new Error(`face embedding invalid JSON: ${e.message}`));
      }
    });
  });
}

async function handleFaceRecognize(req) {
  const width = Number(req.headers["x-face-width"] || 64);
  const height = Number(req.headers["x-face-height"] || 64);
  if (width < 16 || height < 16 || width > 320 || height > 320) {
    return { status: 400, body: { error: 1, reply: "invalid face size" } };
  }
  const body = await readWavFromRequest(req, 640 * 640 * 2 + 1024);
  if (body.length < width * height * 2) {
    return { status: 400, body: { error: 1, reply: "face data too short" } };
  }
  const db = loadFaceDb();
  const desc = await computeFaceEmbedding(body, width, height);
  const match = findBestEmbeddingMatch(desc.embedding, db);
  if (match) {
    return {
      status: 200,
      body: { recognized: true, user_id: match.id, user_name: match.name,
              score: Number(match.score.toFixed(4)), backend: desc.backend,
      model: desc.model, status: "OK" },
    };
  }
  if (!faceDbHasEmbeddings(db)) {
    const legacyHash = averageHash64(body, width, height);
    const legacyMatch = findBestMatch(legacyHash, db);
    if (legacyMatch) {
      return {
        status: 200,
        body: { recognized: true, user_id: legacyMatch.id, user_name: legacyMatch.name,
                distance: legacyMatch.dist, backend: `${desc.backend}+legacy_hash`,
                model: desc.model, status: "OK" },
      };
    }
  }
  return {
    status: 200,
    body: { recognized: false, status: "UNKNOWN", backend: desc.backend,
            model: desc.model,
            hint: "register by posting name + face to /face-register" },
  };
}

async function handleFaceRegister(req) {
  const width = Number(req.headers["x-face-width"] || 64);
  const height = Number(req.headers["x-face-height"] || 64);
  const userName = String(req.headers["x-face-name"] || "").trim();
  console.log(`Face register request: name="${userName}" size=${width}x${height}`);
  if (!userName || userName.length > 32) {
    return { status: 400, body: { error: 1, reply: "need x-face-name header" } };
  }
  const body = await readWavFromRequest(req, 640 * 640 * 2 + 1024);
  if (body.length < width * height * 2) {
    return { status: 400, body: { error: 1, reply: "face data too short" } };
  }
  const hash = averageHash64(body, width, height);
  const desc = await computeFaceEmbedding(body, width, height);
  const db = loadFaceDb();
  // Reuse existing id if the same name exists, or if the face is already
  // close enough to a saved user. The board camera is noisy, so storing
  // several samples per user is much more stable than a single hash.
  let userId = null;
  for (const id of Object.keys(db)) {
    if (db[id].name === userName) { userId = id; break; }
  }
  if (!userId) {
    const match = findBestEmbeddingMatch(desc.embedding, db, {
      threshold: FACE_REGISTER_COSINE_THRESHOLD,
      margin: FACE_REGISTER_COSINE_MARGIN,
    });
    if (match) {
      userId = match.id;
      console.log(`Face register merged with existing user ${match.name} (${match.id}), score=${match.score.toFixed(3)}`);
    }
  }
  if (!userId) {
    userId = `u${Date.now().toString(36)}${Math.floor(Math.random() * 1000).toString(36)}`;
  }
  const now = new Date().toISOString();
  const old = db[userId] || {};
  const hashes = faceHashes(old).map((h) => String(h));
  const hashText = String(hash);
  if (!hashes.includes(hashText)) hashes.unshift(hashText);
  const embeddings = faceEmbeddings(old).map((e) => e.map(Number));
  embeddings.unshift(desc.embedding.map((v) => Number(Number(v).toFixed(6))));
  db[userId] = {
    ...old,
    name: old.name || userName,
    hash: hashText,
    hashes: hashes.slice(0, FACE_MAX_SAMPLES_PER_USER),
    embeddings: embeddings.slice(0, FACE_MAX_SAMPLES_PER_USER),
    embedding_backend: desc.backend,
    embedding_model: desc.model,
    created: old.created || now,
    updated: now,
  };
  saveFaceDb(db);
  console.log(`Face registered: ${db[userId].name} -> ${userId} embedding_samples=${db[userId].embeddings.length} backend=${desc.backend}`);
  return {
    status: 200,
    body: { registered: true, user_id: userId, user_name: db[userId].name,
            backend: desc.backend, model: desc.model, status: "OK" },
  };
}

function handleFaceUsers() {
  const db = loadFaceDb();
  const users = Object.entries(db).map(([id, e]) => ({
    id,
    name: e.name,
    created: e.created,
    embedding_samples: faceEmbeddings(e).length,
    embedding_backend: e.embedding_backend || "",
    embedding_model: e.embedding_model || "",
  }));
  return { status: 200, body: { users } };
}

/* ----------------------------------------------------------------
 * Daily plan module
 *
 * The board records the day's plan by voice (x-lexin-voice-mode: plan),
 * then checks items off one by one. Each check grows a plant on screen.
 * The calendar colours each day by its completion percentage.
 *
 * Storage: tools/plans/<user_id>/<date>/plan.json
 *   { user_id, date, created_at, updated_at, items: [{text, done, done_at}] }
 * --------------------------------------------------------------- */

function planUserDir(userId) {
  return path.join(PLAN_DIR, sanitizePathSegment(userId));
}

function planDayDir(userId, date) {
  return path.join(planUserDir(userId), sanitizePathSegment(date, dateKey()));
}

function planFileFor(userId, date) {
  return path.join(planDayDir(userId, date), "plan.json");
}

function loadPlan(userId, date) {
  const file = planFileFor(userId, date);
  if (!fs.existsSync(file)) {
    return { user_id: userId, date, items: [] };
  }
  try {
    const parsed = JSON.parse(fs.readFileSync(file, "utf8"));
    if (!Array.isArray(parsed.items)) parsed.items = [];
    return parsed;
  } catch {
    return { user_id: userId, date, items: [] };
  }
}

function savePlan(userId, date, plan) {
  fs.mkdirSync(planDayDir(userId, date), { recursive: true });
  plan.updated_at = beijingIsoLike();
  if (!plan.created_at) plan.created_at = plan.updated_at;
  fs.writeFileSync(planFileFor(userId, date), JSON.stringify(plan, null, 2), "utf8");
}

function planPercent(plan) {
  const items = plan.items || [];
  if (items.length === 0) return 0;
  const done = items.filter((it) => it.done).length;
  return Math.round((done / items.length) * 100);
}

/* Board-friendly text: KEY: value lines plus one ITEM<n>: done|text line
 * per to-do. The board parses these with its existing field parser. */
function planToText(plan) {
  const items = plan.items || [];
  const done = items.filter((it) => it.done).length;
  const lines = [
    `DATE: ${plan.date || ""}`,
    `COUNT: ${items.length}`,
    `DONE: ${done}`,
    `PERCENT: ${planPercent(plan)}`,
  ];
  items.forEach((it, i) => {
    const text = String(it.text || "").replace(/[\r\n|]+/g, " ").slice(0, 40);
    lines.push(`ITEM${i}: ${it.done ? 1 : 0}|${text}`);
  });
  return lines.join("\n");
}

/* Turn a spoken sentence into short to-do items. Prefers DeepSeek for a
 * clean split; falls back to a local delimiter split. */
async function parsePlanItems(transcript) {
  const raw = String(transcript || "").trim();
  if (!raw) return [];
  if (voiceDeepseekEnabled()) {
    try {
      const answer = await deepseekChat([
        {
          role: "system",
          content: "把用户口述的一天计划拆成简短的待办事项。每条不超过15个字，只输出一个 JSON 字符串数组，不要输出其它任何文字。例如：[\"完成数学作业\",\"跑步30分钟\"]",
        },
        { role: "user", content: raw },
      ]);
      const start = answer.indexOf("[");
      const end = answer.lastIndexOf("]");
      if (start >= 0 && end > start) {
        const arr = JSON.parse(answer.slice(start, end + 1));
        const items = arr.map((s) => String(s).trim()).filter(Boolean).slice(0, 12);
        if (items.length) return items;
      }
    } catch (error) {
      console.warn(`plan deepseek split fallback: ${error.message}`);
    }
  }
  // Local fallback: split on common Chinese/English list delimiters.
  return raw
    .replace(/然后|接着|还有|以及|再|,|，|、|;|；|。|\.|\n/g, "\u0001")
    .split("\u0001")
    .map((s) => s.trim())
    .filter((s) => s.length > 0)
    .slice(0, 12);
}

async function handlePlanGet(req) {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const userId = sanitizePathSegment(url.searchParams.get("user_id"), "device");
  const date = sanitizePathSegment(url.searchParams.get("date"), dateKey());
  const plan = loadPlan(userId, date);
  return planToText(plan);
}

async function handlePlanDone(req) {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const userId = sanitizePathSegment(url.searchParams.get("user_id"), "device");
  const date = sanitizePathSegment(url.searchParams.get("date"), dateKey());
  const index = Number(url.searchParams.get("index"));
  const doneParam = url.searchParams.get("done");
  const plan = loadPlan(userId, date);
  if (Number.isInteger(index) && index >= 0 && index < plan.items.length) {
    const done = doneParam === null ? !plan.items[index].done : doneParam === "1";
    plan.items[index].done = done;
    plan.items[index].done_at = done ? beijingIsoLike() : "";
    savePlan(userId, date, plan);
  }
  return planToText(plan);
}

async function handlePlanDelete(req) {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const userId = sanitizePathSegment(url.searchParams.get("user_id"), "device");
  const date = sanitizePathSegment(url.searchParams.get("date"), dateKey());
  const index = Number(url.searchParams.get("index"));
  const plan = loadPlan(userId, date);
  if (Number.isInteger(index) && index >= 0 && index < plan.items.length) {
    plan.items.splice(index, 1);
    savePlan(userId, date, plan);
  }
  return planToText(plan);
}

async function handlePlanMonth(req) {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const userId = sanitizePathSegment(url.searchParams.get("user_id"), "device");
  const month = sanitizePathSegment(url.searchParams.get("month"), monthKey());
  const base = planUserDir(userId);
  const pairs = [];
  if (fs.existsSync(base)) {
    const days = fs.readdirSync(base).filter((name) => name.startsWith(month)).sort();
    for (const day of days) {
      const plan = loadPlan(userId, day);
      if ((plan.items || []).length === 0) continue;
      const dayNum = Number(day.slice(8, 10));
      if (Number.isInteger(dayNum)) {
        pairs.push(`${dayNum}:${planPercent(plan)}`);
      }
    }
  }
  return [`MONTH: ${month}`, `PERCENTS: ${pairs.join(",")}`].join("\n");
}

/* The board's plan list shows at most this many rows (LEXIN_PLAN_MAX_ITEMS
 * in the firmware). Items beyond it would be stored but never visible, so
 * cap the day's plan at the same number and tell the user when it's full. */
const MAX_PLAN_ITEMS = 12;

/* Append voice-captured items to today's plan. Returns a short reply. */
async function capturePlanFromVoice(userId, transcript) {
  const date = dateKey();
  const items = await parsePlanItems(transcript);
  if (items.length === 0) {
    return { reply: "没听清今天的计划，请再说一次。", count: 0 };
  }
  const plan = loadPlan(userId, date);
  const room = MAX_PLAN_ITEMS - plan.items.length;
  if (room <= 0) {
    return { reply: `今日计划已满（最多${MAX_PLAN_ITEMS}条），删除一些再录吧。`,
             count: 0, total: plan.items.length };
  }
  const accepted = items.slice(0, room);
  for (const text of accepted) {
    plan.items.push({ text, done: false, done_at: "" });
  }
  savePlan(userId, date, plan);
  const dropped = items.length - accepted.length;
  const reply = dropped > 0
    ? `已记录 ${accepted.length} 项，${dropped} 项超出上限（最多${MAX_PLAN_ITEMS}条）未记录。`
    : `已记录 ${accepted.length} 项计划，一起加油。`;
  return { reply, count: accepted.length, total: plan.items.length };
}

/* ----------------------------------------------------------------
 * Voice conversation pipeline
 *
 * The board uploads a 16 kHz mono WAV captured between two silence
 * edges. The proxy first runs an external ASR command if one is
 * configured (LEXIN_ASR_CMD). The command receives the WAV file path
 * as its last argument and is expected to print a JSON object on
 * stdout with at least {"text": "..."}. The same JSON object may
 * contain a "wake": true|false field.
 *
 * When no external ASR is configured the proxy falls back to a
 * hand-written wake + FAQ rule that runs entirely in Node, so the
 * demo works the moment tools/lexin_proxy.js is started.
 * --------------------------------------------------------------- */

function sendJson(res, status, body) {
  const text = JSON.stringify(body);
  res.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store",
    "access-control-allow-origin": "*",
  });
  res.end(text);
}

function readWavFromRequest(req, maxBytes) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let length = 0;
    req.on("data", (chunk) => {
      length += chunk.length;
      if (length > maxBytes) {
        reject(new Error("voice body too large"));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => resolve(Buffer.concat(chunks, length)));
    req.on("error", reject);
  });
}

function parseAsrStdout(stdout) {
  const text = String(stdout || "").trim();
  if (!text) {
    throw new Error("asr produced no output");
  }
  try {
    return JSON.parse(text);
  } catch (_) {
    /* Some ASR dependencies print notices to stdout before the JSON line.
     * Keep the on-screen transcript clean by taking the final JSON object
     * when one is present. */
    const lines = text.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
    for (let i = lines.length - 1; i >= 0; i--) {
      const line = lines[i];
      if (!line.startsWith("{") || !line.endsWith("}")) continue;
      try {
        return JSON.parse(line);
      } catch (_) {
        /* Try the previous line. */
      }
    }
    return { text };
  }
}

function runExternalAsr(wavPath) {
  return new Promise((resolve, reject) => {
    if (!LEXIN_ASR_CMD) {
      reject(new Error("LEXIN_ASR_CMD not set"));
      return;
    }
    const parts = LEXIN_ASR_CMD.split(/\s+/);
    const cmd = parts[0];
    const args = parts.slice(1).concat([wavPath, LEXIN_ASR_LANG]);
    const child = spawn(cmd, args, {
      stdio: ["ignore", "pipe", "pipe"],
      windowsHide: true,
      env: {
        ...process.env,
        PYTHONIOENCODING: "utf-8",
        PYTHONUTF8: "1",
      },
    });
    const out = [];
    const err = [];
    child.stdout.on("data", (b) => out.push(b));
    child.stderr.on("data", (b) => err.push(b));
    child.on("error", (e) => reject(e));
    child.on("close", (code) => {
      if (code !== 0) {
        reject(new Error(`asr exit ${code}: ${Buffer.concat(err).toString()}`));
        return;
      }
      const stdout = Buffer.concat(out).toString("utf8").trim();
      try {
        resolve(parseAsrStdout(stdout));
      } catch (e) {
        reject(e);
      }
    });
  });
}

function containsWake(text) {
  if (!text) return false;
  const lower = String(text).toLowerCase();
  for (const kw of LEXIN_WAKE_KEYWORDS) {
    if (!kw) continue;
    if (lower.includes(kw)) return true;
    /* Match Chinese characters exactly. */
    if (/[一-鿿]/.test(kw) && text.includes(kw)) return true;
  }
  return false;
}

function stripWakeWord(text) {
  if (!text) return "";
  let out = String(text);
  for (const kw of LEXIN_WAKE_KEYWORDS) {
    if (!kw) continue;
    const re = new RegExp(kw.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi");
    out = out.replace(re, "");
  }
  return out.replace(/[,，\s]+/g, " ").trim();
}

async function deepseekChat(messages) {
  if (!DEEPSEEK_API_KEY) {
    throw new Error("DeepSeek API key not set");
  }
  const response = await fetch(`${DEEPSEEK_BASE_URL}/chat/completions`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      authorization: `Bearer ${DEEPSEEK_API_KEY}`,
    },
    body: JSON.stringify({
      model: DEEPSEEK_MODEL,
      temperature: 0.5,
      max_tokens: 240,
      messages,
    }),
    signal: AbortSignal.timeout(9000),
  });
  if (!response.ok) {
    const body = await response.text();
    throw new Error(`deepseek status ${response.status}: ${body.slice(0, 200)}`);
  }
  const data = await response.json();
  return (data.choices?.[0]?.message?.content || "").trim();
}

function voiceDeepseekEnabled() {
  /* Forward every open-ended question to DeepSeek whenever a key is
   * configured. Set LEXIN_DEEPSEEK_VOICE=0 to force the local rules. */
  return Boolean(DEEPSEEK_API_KEY) && LEXIN_DEEPSEEK_VOICE;
}

const liveInfoCache = new Map();

function decodeHtmlEntities(text) {
  return String(text || "")
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1")
    .replace(/&#x([0-9a-fA-F]+);/g, (_, hex) => String.fromCodePoint(parseInt(hex, 16)))
    .replace(/&#(\d+);/g, (_, dec) => String.fromCodePoint(parseInt(dec, 10)))
    .replace(/&quot;/g, "\"")
    .replace(/&apos;/g, "'")
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/\s+/g, " ")
    .trim();
}

function stripHtml(text) {
  return decodeHtmlEntities(String(text || "").replace(/<[^>]+>/g, " "));
}

function xmlTag(block, tag) {
  const re = new RegExp(`<${tag}(?:\\s[^>]*)?>([\\s\\S]*?)<\\/${tag}>`, "i");
  const match = String(block || "").match(re);
  return match ? decodeHtmlEntities(match[1]) : "";
}

function parseRssItems(xml, source, limit) {
  const items = [];
  const blocks = String(xml || "").match(/<item\b[\s\S]*?<\/item>/gi) || [];
  for (const block of blocks) {
    const title = stripHtml(xmlTag(block, "title"));
    const link = stripHtml(xmlTag(block, "link"));
    const pubDate = stripHtml(xmlTag(block, "pubDate"));
    const description = stripHtml(xmlTag(block, "description"));
    if (!title) continue;
    items.push({
      title,
      link,
      pubDate,
      source,
      summary: description.slice(0, 160),
    });
    if (items.length >= limit) break;
  }
  return items;
}

async function fetchText(url) {
  const response = await fetch(url, {
    headers: {
      "user-agent": "LeXin-ESP32P4-Proxy/1.0",
      accept: "application/rss+xml,text/xml,text/html;q=0.8,*/*;q=0.5",
    },
    signal: AbortSignal.timeout(LIVE_INFO_TIMEOUT_MS),
  });
  if (!response.ok) throw new Error(`live info status ${response.status}`);
  return response.text();
}

function liveInfoIntent(text) {
  return /新闻|热点|最新|发生了什么|实时|世界杯|比分|赛果|赛程|战报|排名|冠军|比赛|足球|篮球|NBA|英超|中超|欧冠|亚冠|奥运|赛事/i.test(text);
}

function liveInfoQuery(text) {
  const clean = String(text || "")
    .replace(/[?？!！。，“”"']/g, " ")
    .replace(/\s+/g, " ")
    .trim();
  if (/世界杯|比分|赛果|赛程|战报|比赛|足球|篮球|NBA|英超|中超|欧冠|亚冠|奥运|赛事/i.test(clean)) {
    return `${clean} 最新 比分 赛果`;
  }
  if (/新闻|热点|发生了什么|实时|最新/i.test(clean)) {
    return clean;
  }
  return `${clean} 最新消息`;
}

function dedupeLiveItems(items) {
  const seen = new Set();
  const out = [];
  for (const item of items) {
    const key = item.title.replace(/\s+/g, "").slice(0, 40);
    if (!key || seen.has(key)) continue;
    seen.add(key);
    out.push(item);
    if (out.length >= LIVE_INFO_LIMIT) break;
  }
  return out;
}

async function fetchLiveInfo(query) {
  if (!LEXIN_LIVE_INFO_ENABLED) return { query, items: [], error: "LIVE_INFO_DISABLED" };
  const cacheKey = query.toLowerCase();
  const cached = liveInfoCache.get(cacheKey);
  if (cached && Date.now() - cached.at < LIVE_INFO_CACHE_MS) return cached.data;

  const urls = [];
  const google = new URL("https://news.google.com/rss/search");
  google.searchParams.set("q", query);
  google.searchParams.set("hl", "zh-CN");
  google.searchParams.set("gl", "CN");
  google.searchParams.set("ceid", "CN:zh-Hans");
  urls.push({ source: "Google News", url: google.toString() });

  const bing = new URL("https://www.bing.com/news/search");
  bing.searchParams.set("q", query);
  bing.searchParams.set("format", "RSS");
  bing.searchParams.set("mkt", "zh-CN");
  bing.searchParams.set("cc", "CN");
  urls.push({ source: "Bing News", url: bing.toString() });

  const errors = [];
  const items = [];
  for (const candidate of urls) {
    try {
      const xml = await fetchText(candidate.url);
      items.push(...parseRssItems(xml, candidate.source, LIVE_INFO_LIMIT));
      if (items.length >= LIVE_INFO_LIMIT) break;
    } catch (error) {
      errors.push(`${candidate.source}: ${error.message}`);
    }
  }

  const data = {
    query,
    items: dedupeLiveItems(items),
    error: errors.join("; "),
    fetched_at: beijingIsoLike(),
  };
  liveInfoCache.set(cacheKey, { at: Date.now(), data });
  console.log(`Live info query="${query}" items=${data.items.length}${data.error ? ` errors=${data.error}` : ""}`);
  return data;
}

function liveInfoFallbackReply(data) {
  if (!data || !Array.isArray(data.items) || data.items.length === 0) {
    return "我暂时没有查到可靠的最新信息，可以换个更具体的关键词。";
  }
  const top = data.items.slice(0, 3).map((item, index) => `${index + 1}. ${item.title}`).join("；");
  return `我查到几条最新结果：${top}`;
}

async function composeLiveInfoReply(spoken, cleanText) {
  const query = liveInfoQuery(cleanText || spoken);
  const data = await fetchLiveInfo(query).catch((error) => ({
    query,
    items: [],
    error: error.message,
    fetched_at: beijingIsoLike(),
  }));
  if (!data.items || data.items.length === 0) {
    return { reply: liveInfoFallbackReply(data), backend: 1, model: "LIVE_WEB_EMPTY" };
  }
  if (voiceDeepseekEnabled()) {
    try {
      const sources = data.items.map((item, index) => [
        `${index + 1}. ${item.title}`,
        item.pubDate ? `时间：${item.pubDate}` : "",
        item.summary ? `摘要：${item.summary}` : "",
        item.source ? `来源：${item.source}` : "",
      ].filter(Boolean).join("\n")).join("\n\n");
      const answer = await deepseekChat([
        {
          role: "system",
          content: [
            "你是乐鑫桌宠的中文实时信息整理助手。",
            "只能依据 PC 代理刚刚联网拿到的候选结果回答，不要编造候选结果里没有的事实。",
            "如果候选结果没有明确比分或结论，就直接说暂时没有查到明确结果。",
            "回答控制在 80 个汉字以内，适合小屏显示。",
          ].join(""),
        },
        {
          role: "user",
          content: `用户问题：${spoken}\n查询词：${query}\n抓取时间：${data.fetched_at}\n候选结果：\n${sources}`,
        },
      ]);
      if (answer) return { reply: answer, backend: 2, model: "DEEPSEEK_LIVE_WEB" };
    } catch (error) {
      console.warn(`live info deepseek fallback: ${error.message}`);
    }
  }
  return { reply: liveInfoFallbackReply(data), backend: 1, model: "LIVE_WEB" };
}

const VOICE_SYSTEM_PROMPT = [
  "你是名叫“乐鑫”的中文桌面语音助手，运行在一块 ESP32-P4 开发板上，主人是一名电子信息方向的研究生。",
  "请用自然、口语化的简体中文回答，像朋友聊天一样，语气温和。",
  "回答尽量简短，控制在 60 个字以内，因为答案会显示在一块小屏幕上，之后也会用扬声器读出来。",
  "如果用户询问天气、时间或日历，系统会优先用本地数据回答，不要声称自己没联网。",
  "不要使用 Markdown、列表符号或表情符号，直接说话即可。",
].join("");

function voiceWeatherNameCn(weather) {
  const key = String(weather || "").toUpperCase();
  if (key === "SUNNY") return "晴";
  if (key === "CLOUDY") return "多云";
  if (key === "RAIN") return "有雨";
  if (key === "SNOW") return "有雪";
  return "天气状态暂时未知";
}

function voiceWeatherAdviceCn(advice) {
  const key = String(advice || "").toUpperCase();
  if (key === "UMBRELLA") return "出门记得带伞。";
  if (key === "HOT") return "天气偏热，注意防暑，多喝水。";
  if (key === "COLD") return "天气偏冷，注意保暖。";
  if (key === "CHECK_NETWORK") return "天气接口暂时不稳定，可以稍后再问我。";
  return "整体适合学习和出门。";
}

function voiceTemperatureCn(temp) {
  const value = String(temp || "").trim();
  if (!value) return "";
  return value.replace(/C$/i, "℃");
}

function buildVoiceWeatherReply(weather) {
  if (!weather || String(weather.weather || "").toUpperCase() === "UNKNOWN") {
    return "天气接口暂时不可用，可以稍后再问我。";
  }
  const temp = voiceTemperatureCn(weather.temp);
  const rain = String(weather.rain || "0%").trim();
  const weatherName = voiceWeatherNameCn(weather.weather);
  const advice = voiceWeatherAdviceCn(weather.advice);
  return `西安现在${weatherName}${temp ? `，气温${temp}` : ""}，降雨概率${rain}。${advice}`;
}

function buildVoiceTimeReply(time) {
  return `现在是北京时间 ${time.time}。`;
}

function buildVoiceCalendarReply(time) {
  const dayName = time.dayType === "WEEKEND" ? "周末" : time.dayType === "HOLIDAY" ? "节假日" : "工作日";
  return `今天是 ${time.date}，${dayName}，农历${time.lunar}。`;
}

async function composeReply(spoken, raw, historyMessages) {
  if (!spoken) {
    return { reply: "我在，你说。", backend: 1 };
  }
  const cleanText = spoken.replace(/[,，。.\s!?！？]+/g, " ").trim();
  /* 1. Deterministic local intents should not go through DeepSeek. This
   *    keeps weather/time/calendar answers grounded in the proxy data. */
  if (/天气|下雨|降雨|雨|温度|气温|冷|热|晴|阴|多云/.test(cleanText)) {
    const weather = await weatherData().catch(() => null);
    return { reply: buildVoiceWeatherReply(weather), backend: 1, model: "LOCAL_WEATHER" };
  }
  const time = timeData();
  if (/几点|时间|现在几点/.test(cleanText)) {
    return { reply: buildVoiceTimeReply(time), backend: 1, model: "LOCAL_TIME" };
  }
  if (/日历|星期|周几|几号|农历|日期/.test(cleanText)) {
    return { reply: buildVoiceCalendarReply(time), backend: 1, model: "LOCAL_CALENDAR" };
  }
  if (liveInfoIntent(cleanText)) {
    return composeLiveInfoReply(spoken, cleanText);
  }

  /* 2. Exact FAQ match (after trimming) keeps very common short intents
   *    instant and free even when DeepSeek is enabled. */
  for (const key of Object.keys(LEXIN_VOICE_FAQ)) {
    if (cleanText.includes(key)) {
      return { reply: LEXIN_VOICE_FAQ[key], backend: 1 };
    }
  }
  /* 3. DeepSeek handles everyday conversation, with recent turns as
   *    context so it can follow along. */
  if (voiceDeepseekEnabled()) {
    try {
      const messages = [{ role: "system", content: VOICE_SYSTEM_PROMPT }];
      if (Array.isArray(historyMessages) && historyMessages.length) {
        messages.push(...historyMessages);
      }
      messages.push({ role: "user", content: spoken });
      const answer = await deepseekChat(messages);
      if (answer) {
        return { reply: answer, backend: 2, model: DEEPSEEK_MODEL };
      }
    } catch (error) {
      console.warn(`voice deepseek fallback: ${error.message}`);
    }
  }
  /* 4. Local rule engine: a tiny weather/time sensitive template so the
   *    demo still answers common questions without a key. */
  const weather = await weatherData().catch(() => null);
  if (/天气|下雨|温度|冷|热/.test(cleanText) && weather) {
    return {
      reply: buildVoiceWeatherReply(weather),
      backend: 1,
    };
  }
  if (/几点|时间|现在/.test(cleanText)) {
    return { reply: buildVoiceTimeReply(time), backend: 1 };
  }
  if (/日历|星期|周几|几号|农历/.test(cleanText)) {
    return { reply: buildVoiceCalendarReply(time), backend: 1 };
  }
  /* 5. Generic acknowledgement so the user always gets something. */
  if (raw && raw.length > 0 && raw.length <= 12) {
    return { reply: `我听到「${cleanText}」，可以再说详细点吗？`, backend: 1 };
  }
  return {
    reply: "我在听，继续说或者换个问法。",
    backend: 1,
  };
}

async function handleVoiceUpload(req) {
  const sampleRate = Number(req.headers["x-lexin-sample-rate"] || 16000);
  const channels = Number(req.headers["x-lexin-channels"] || 1);
  if (![16000, 22050].includes(sampleRate) || channels !== 1) {
    return {
      status: 400,
      body: {
        error: 1,
        reply: `暂不支持 ${sampleRate}Hz / ${channels}ch 音频`,
        status: "BAD_AUDIO",
      },
    };
  }
  const clientKey = sanitizePathSegment(req.headers["x-lexin-user-id"], "device");
  const voiceMode = String(req.headers["x-lexin-voice-mode"] || "chat").toLowerCase();
  const pageChatMode = voiceMode === "voice" || voiceMode === "chat-open";
  const body = await readWavFromRequest(req, 2 * 1024 * 1024);
  const tmpDir = path.join(os.tmpdir(), "lexin-voice");
  fs.mkdirSync(tmpDir, { recursive: true });
  const filename = `voice_${Date.now()}_${Math.floor(Math.random() * 1000)}.wav`;
  const wavPath = path.join(tmpDir, filename);
  fs.writeFileSync(wavPath, body);
  fs.mkdirSync(VOICE_DEBUG_DIR, { recursive: true });
  const debugPath = path.join(VOICE_DEBUG_DIR, filename);
  fs.copyFileSync(wavPath, debugPath);
  console.log(`Voice upload: ${body.length} bytes, ${sampleRate}Hz, ${channels}ch, user=${clientKey}, saved=${debugPath}`);

  let transcript = "";
  let asrSource = "fallback";

  if (LEXIN_ASR_CMD) {
    try {
      const out = await runExternalAsr(wavPath);
      transcript = String(out.text || "").trim();
      asrSource = "external";
    } catch (error) {
      console.warn(`voice ASR failed (${error.message}); falling back to rules`);
      asrSource = "fallback";
    }
  }

  console.log(`Voice ASR: ${JSON.stringify(transcript)} source=${asrSource}`);
  fs.writeFileSync(debugPath.replace(/\.wav$/i, ".json"), JSON.stringify({
    time: new Date().toISOString(),
    user: clientKey,
    mode: voiceMode,
    sampleRate,
    channels,
    bytes: body.length,
    asr: asrSource,
    transcript,
  }, null, 2), "utf8");

  if (asrSource === "fallback") {
    /* No ASR model configured (or it failed): we cannot read the words,
     * so we cannot converse. Tell the user how to enable it instead of
     * pretending. */
    fs.unlink(wavPath, () => {});
    console.log("Voice reply: ASR not configured");
    return {
      status: 200,
      body: {
        wake: true,
        transcript: "",
        asr: asrSource,
        reply: "语音识别还没配置，请在电脑上运行 setup_asr.ps1。",
        backend: 1,
        model: "LOCAL_RULE",
        status: "NO_ASR",
        session: voiceSessionActive(clientKey),
      },
    };
  }

  /* Plan capture mode: the user is dictating today's to-do list from the
   * plan page. No wake word required (the page already opted in). */
  if (voiceMode === "plan") {
    fs.unlink(wavPath, () => {});
    if (!transcript) {
      return {
        status: 200,
        body: { transcript: "", asr: asrSource, reply: "没听清今天的计划，请再说一次。",
                backend: 1, model: "LOCAL_RULE", status: "PLAN_EMPTY", mode: "plan" },
      };
    }
    const captured = await capturePlanFromVoice(clientKey, transcript);
    return {
      status: 200,
      body: {
        transcript,
        asr: asrSource,
        reply: captured.reply,
        backend: voiceDeepseekEnabled() ? 2 : 1,
        model: voiceDeepseekEnabled() ? DEEPSEEK_MODEL : "LOCAL_RULE",
        status: captured.count > 0 ? "PLAN_OK" : "PLAN_EMPTY",
        mode: "plan",
        plan_count: captured.count,
        plan_total: captured.total || 0,
      },
    };
  }

  /* Voice page mode: the user has already opened the conversation page,
   * so every utterance is treated as an intentional chat turn. This keeps
   * the page useful even when ASR misses the wake word. */
  if (pageChatMode) {
    fs.unlink(wavPath, () => {});
    if (!transcript) {
      return {
        status: 200,
        body: {
          wake: true,
          transcript: "",
          asr: asrSource,
          reply: "没听清，请再说一次。",
          backend: 1,
          model: "LOCAL_RULE",
          status: "VOICE_EMPTY",
          mode: voiceMode,
          session: voiceSessionActive(clientKey),
        },
      };
    }
    voiceSessionTouch(clientKey);
    const spoken = stripWakeWord(transcript) || transcript;
    const history = voiceHistoryGet(clientKey);
    const composed = await composeReply(spoken, transcript, history);
    voiceHistoryPush(clientKey, spoken, composed.reply);
    return {
      status: 200,
      body: {
        wake: true,
        transcript,
        asr: asrSource,
        reply: composed.reply,
        backend: composed.backend,
        model: composed.model || (composed.backend === 2 ? DEEPSEEK_MODEL : "LOCAL_RULE"),
        status: "OK",
        mode: voiceMode,
        session: true,
      },
    };
  }

  const hasWakeWord = containsWake(transcript);
  const sessionActive = voiceSessionActive(clientKey);
  const wake = hasWakeWord || sessionActive;

  if (!wake) {
    fs.unlink(wavPath, () => {});
    return {
      status: 200,
      body: {
        wake: false,
        transcript,
        reply: "（未检测到唤醒词，先说“乐鑫乐鑫”）",
        backend: 0,
        status: "NO_WAKE",
        asr: asrSource,
        session: false,
      },
    };
  }

  /* Wake word heard or an existing conversation is still open: (re)open
   * the conversation window so the next utterances need no wake word. */
  voiceSessionTouch(clientKey);

  const spoken = stripWakeWord(transcript);

  /* User only said the wake word (no follow-up content yet). */
  if (!spoken) {
    fs.unlink(wavPath, () => {});
    return {
      status: 200,
      body: {
        wake: true,
        transcript,
        asr: asrSource,
        reply: "我在，你说。",
        backend: 1,
        model: "LOCAL_RULE",
        status: hasWakeWord ? "WAKE" : "OK",
        session: true,
      },
    };
  }

  const history = voiceHistoryGet(clientKey);
  const composed = await composeReply(spoken, transcript, history);
  voiceHistoryPush(clientKey, spoken, composed.reply);
  fs.unlink(wavPath, () => {});

  return {
    status: 200,
    body: {
      wake: true,
      transcript,
      asr: asrSource,
      reply: composed.reply,
      backend: composed.backend,
      model: composed.model || (composed.backend === 2 ? DEEPSEEK_MODEL : "LOCAL_RULE"),
      status: "OK",
      session: true,
    },
  };
}

async function handleLiveInfo(req) {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const query = String(url.searchParams.get("q") || "").trim() || "今日新闻";
  const data = await fetchLiveInfo(query);
  const lines = [
    `QUERY: ${data.query}`,
    `FETCHED_AT: ${data.fetched_at || ""}`,
    `COUNT: ${Array.isArray(data.items) ? data.items.length : 0}`,
  ];
  if (data.error) lines.push(`ERRORS: ${data.error}`);
  for (const [index, item] of (data.items || []).entries()) {
    lines.push(`ITEM${index + 1}: ${item.title}`);
    if (item.pubDate) lines.push(`TIME${index + 1}: ${item.pubDate}`);
    if (item.source) lines.push(`SOURCE${index + 1}: ${item.source}`);
    if (item.link) lines.push(`LINK${index + 1}: ${item.link}`);
  }
  return lines.join("\n");
}

function pathOf(req) {
  try {
    const u = new URL(req.url, `http://${req.headers.host || "localhost"}`);
    return u.pathname;
  } catch {
    return req.url.split("?")[0];
  }
}

const server = http.createServer(async (req, res) => {
  try {
    const path = pathOf(req);
    if (req.method === "POST" && path === "/capture") {
      const outPath = await saveCapture(req);
      send(res, 200, `SAVED ${outPath}`);
      return;
    }
    if (req.method === "POST" && path === "/voice-stream") {
      const reply = await handleVoiceUpload(req);
      sendJson(res, reply.status || 200, reply.body);
      return;
    }
    if (req.method === "POST" && path === "/emotion-log") {
      const reply = await handleEmotionLog(req);
      sendJson(res, reply.status || 200, reply.body);
      return;
    }
    if (req.method === "GET" && path === "/plan") {
      send(res, 200, await handlePlanGet(req));
      return;
    }
    if (req.method === "POST" && path === "/plan-done") {
      send(res, 200, await handlePlanDone(req));
      return;
    }
    if (req.method === "POST" && path === "/plan-delete") {
      send(res, 200, await handlePlanDelete(req));
      return;
    }
    if (req.method === "GET" && path === "/plan-month") {
      send(res, 200, await handlePlanMonth(req));
      return;
    }
    if (req.method === "GET" && path === "/emotion-report") {
      send(res, 200, await handleEmotionReport(req));
      return;
    }
    if (req.method === "GET" && path === "/emotion-month") {
      send(res, 200, await handleEmotionMonth(req));
      return;
    }
    if (path === "/weather") {
      send(res, 200, weatherText(await weatherData()));
      return;
    }
    if (path === "/time") {
      send(res, 200, timeText(timeData()));
      return;
    }
    if (path === "/insight") {
      const weather = await weatherData();
      const time = timeData();
      send(res, 200, insightText(await deepseekInsight(weather, time)));
      return;
    }
    if (path === "/edge-context") {
      const weather = await weatherData();
      const time = timeData();
      send(res, 200, await edgeContextText(weather, time));
      return;
    }
    if (path === "/live-info") {
      send(res, 200, await handleLiveInfo(req));
      return;
    }
    if (path === "/health") {
      send(res, 200, "OK PET");
      return;
    }
    /* Face recognition endpoints. The board uploads a cropped
     * RGB565 face region; the proxy hashes it and compares. */
    if (req.method === "POST" && path === "/face-recognize") {
      const reply = await handleFaceRecognize(req);
      sendJson(res, reply.status || 200, reply.body);
      return;
    }
    if (req.method === "POST" && path === "/face-register") {
      const reply = await handleFaceRegister(req);
      sendJson(res, reply.status || 200, reply.body);
      return;
    }
    if (req.method === "GET" && path === "/face-users") {
      const reply = handleFaceUsers();
      sendJson(res, reply.status || 200, reply.body);
      return;
    }
    send(res, 404, "NOT FOUND");
  } catch (error) {
    send(res, 502, `ERROR: ${error.message}`);
  }
});

server.listen(PORT, "0.0.0.0", () => {
  ensureFaceDbFile();
  fs.mkdirSync(EMOTION_REPORT_DIR, { recursive: true });
  console.log(`LeXin DeepSeek pet proxy listening on http://0.0.0.0:${PORT}`);
  console.log("Endpoints: /weather /time /edge-context /insight /live-info /health /capture /voice-stream /emotion-log /emotion-report /emotion-month /face-recognize /face-register /face-users /plan /plan-done /plan-delete /plan-month");
  console.log(`Face users save to ${FACE_DB_FILE}`);
  console.log(`Face embedding python: ${FACE_PYTHON}`);
  console.log(`Face embedding script: ${FACE_EMBED_SCRIPT}`);
  console.log(`Face ONNX model: ${FACE_ONNX_MODEL}`);
  console.log(`Face cosine threshold: ${FACE_COSINE_THRESHOLD}, margin: ${FACE_COSINE_MARGIN}, register threshold: ${FACE_REGISTER_COSINE_THRESHOLD}, register margin: ${FACE_REGISTER_COSINE_MARGIN}, max samples/user: ${FACE_MAX_SAMPLES_PER_USER}`);
  console.log(`Legacy face hash threshold: ${FACE_HAMMING_THRESHOLD}`);
  console.log(`Board captures save to ${CAPTURE_DIR}`);
  console.log(`Emotion reports save to ${EMOTION_REPORT_DIR}`);
  console.log(DEEPSEEK_API_KEY ? `DeepSeek enabled: ${DEEPSEEK_MODEL}` : "DeepSeek not configured, using local pet fallback.");
  if (LEXIN_ASR_CMD) {
    console.log(`Voice ASR command: ${LEXIN_ASR_CMD} (lang=${LEXIN_ASR_LANG})`);
  } else {
    console.log("Voice ASR command: <not configured>, using fallback wake+rules");
  }
  console.log(`Voice wake keywords: ${LEXIN_WAKE_KEYWORDS.join(", ")}`);
  console.log(`Voice DeepSeek route: ${LEXIN_DEEPSEEK_VOICE ? "enabled" : "off"}`);
});

const discovery = dgram.createSocket("udp4");
discovery.on("message", (message, remote) => {
  if (message.toString("utf8").trim() !== "LEXIN_DISCOVER_V1") return;
  const response = Buffer.from("LEXIN_PROXY_V1", "utf8");
  discovery.send(response, remote.port, remote.address);
});
discovery.on("error", (error) => {
  console.error(`LeXin UDP discovery disabled: ${error.message}`);
  discovery.close();
});
discovery.bind(DISCOVERY_PORT, "0.0.0.0", () => {
  console.log(`LeXin discovery listening on UDP ${DISCOVERY_PORT}`);
});
