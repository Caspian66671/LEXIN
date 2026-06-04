const http = require("http");

function portFromArgs() {
  const index = process.argv.indexOf("--port");
  if (index >= 0 && process.argv[index + 1]) {
    return process.argv[index + 1];
  }
  return "";
}

const PORT = Number(process.env.PORT || portFromArgs() || 8787);
const XIAN_LAT = 34.3416;
const XIAN_LON = 108.9398;
const DEEPSEEK_BASE_URL = (process.env.DEEPSEEK_BASE_URL || "https://api.deepseek.com").replace(/\/$/, "");
const DEEPSEEK_MODEL = process.env.DEEPSEEK_MODEL || "deepseek-chat";
const DEEPSEEK_API_KEY = process.env.DEEPSEEK_API_KEY || "";

function send(res, status, body) {
  res.writeHead(status, {
    "content-type": "text/plain; charset=utf-8",
    "cache-control": "no-store",
    "access-control-allow-origin": "*",
  });
  res.end(body);
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

async function weatherData() {
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

const server = http.createServer(async (req, res) => {
  try {
    if (req.url === "/weather") {
      send(res, 200, weatherText(await weatherData()));
      return;
    }
    if (req.url === "/time") {
      send(res, 200, timeText(timeData()));
      return;
    }
    if (req.url === "/insight") {
      const weather = await weatherData();
      const time = timeData();
      send(res, 200, insightText(await deepseekInsight(weather, time)));
      return;
    }
    if (req.url === "/edge-context") {
      const weather = await weatherData();
      const time = timeData();
      send(res, 200, await edgeContextText(weather, time));
      return;
    }
    if (req.url === "/health") {
      send(res, 200, "OK PET");
      return;
    }
    send(res, 404, "NOT FOUND");
  } catch (error) {
    send(res, 502, `ERROR: ${error.message}`);
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`WorkBuddy DeepSeek pet proxy listening on http://0.0.0.0:${PORT}`);
  console.log("Endpoints: /weather /time /edge-context /insight /health");
  console.log(DEEPSEEK_API_KEY ? `DeepSeek enabled: ${DEEPSEEK_MODEL}` : "DeepSeek not configured, using local pet fallback.");
});
