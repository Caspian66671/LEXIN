const http = require("http");

const PORT = Number(process.env.PORT || 8787);
const XIAN_LAT = 34.3416;
const XIAN_LON = 108.9398;

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

async function weatherText() {
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

    return [
      `TEMP: ${Number.isFinite(temp) ? `${temp}C` : "UNKNOWN"}`,
      `WEATHER: ${weather}`,
      `RAIN: ${rain}`,
      `ADVICE: ${advice}`,
    ].join("\n");
  } catch (error) {
    console.warn(`weather fallback: ${error.message}`);
    return [
      "TEMP: UNKNOWN",
      "WEATHER: UNKNOWN",
      "RAIN: UNKNOWN",
      "ADVICE: CHECK_NETWORK",
    ].join("\n");
  }
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

function timeText() {
  const now = beijingNow();
  const time = `${pad2(now.getHours())}:${pad2(now.getMinutes())}`;
  const date = `${now.getFullYear()}-${pad2(now.getMonth() + 1)}-${pad2(now.getDate())}`;
  return [
    `TIME: ${time}`,
    `DATE: ${date}`,
    `LUNAR: ${lunarText(now)}`,
    `HOLIDAY: ${holidayFor(now)}`,
  ].join("\n");
}

const server = http.createServer(async (req, res) => {
  try {
    if (req.url === "/weather") {
      send(res, 200, await weatherText());
      return;
    }
    if (req.url === "/time") {
      send(res, 200, timeText());
      return;
    }
    if (req.url === "/health") {
      send(res, 200, "OK");
      return;
    }
    send(res, 404, "NOT FOUND");
  } catch (error) {
    send(res, 502, `ERROR: ${error.message}`);
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`WorkBuddy quick proxy listening on http://0.0.0.0:${PORT}`);
  console.log("Endpoints: /weather /time /health");
});
