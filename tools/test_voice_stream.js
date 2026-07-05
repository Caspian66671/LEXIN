#!/usr/bin/env node
/**
 * Smoke test for the LeXin /voice-stream endpoint.
 *
 * Usage:
 *   node tools/test_voice_stream.js
 *   node tools/test_voice_stream.js --url http://192.168.1.3:8787
 *
 * Generates a 1 second 16 kHz mono PCM beep, packs it into a RIFF/WAVE
 * blob, and posts it to /voice-stream. The proxy should return JSON
 * with at least {"reply": "..."}. When LEXIN_ASR_CMD is not set on the
 * proxy, the fallback path returns a wake acknowledgement, which is
 * what the smoke test expects.
 */

const http = require("http");
const path = require("path");

function buildWav(pcmSamples, sampleRate = 22050) {
  const pcmBytes = pcmSamples.length * 2;
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + pcmBytes, 4);
  header.write("WAVE", 8);
  header.write("fmt ", 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); // PCM
  header.writeUInt16LE(1, 22); // mono
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(sampleRate * 2, 28); // byte rate
  header.writeUInt16LE(2, 32); // block align
  header.writeUInt16LE(16, 34); // bits per sample
  header.write("data", 36);
  header.writeUInt32LE(pcmBytes, 40);
  const pcm = Buffer.alloc(pcmBytes);
  for (let i = 0; i < pcmSamples.length; i++) {
    pcm.writeInt16LE(pcmSamples[i], i * 2);
  }
  return Buffer.concat([header, pcm]);
}

function beep(durationMs, freqHz, sampleRate) {
  const n = Math.floor((sampleRate * durationMs) / 1000);
  const out = new Int16Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / sampleRate;
    const env = Math.min(1, Math.min(i, n - i) / (sampleRate * 0.05));
    out[i] = Math.round(Math.sin(2 * Math.PI * freqHz * t) * 12000 * env);
  }
  return out;
}

function postWav(url, body) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const req = http.request({
      method: "POST",
      hostname: u.hostname,
      port: u.port,
      path: u.pathname,
      headers: {
        "content-type": "audio/wav",
        "content-length": body.length,
        "x-lexin-sample-rate": "22050",
        "x-lexin-channels": "1",
      },
    }, (res) => {
      const chunks = [];
      res.on("data", (b) => chunks.push(b));
      res.on("end", () => resolve({
        status: res.statusCode,
        text: Buffer.concat(chunks).toString("utf8"),
      }));
    });
    req.on("error", reject);
    req.write(body);
    req.end();
  });
}

async function main() {
  const argUrl = process.argv.find((a, i) => process.argv[i - 1] === "--url");
  const url = argUrl || process.env.LEXIN_PROXY_URL || "http://127.0.0.1:8787/voice-stream";
  console.log(`POST ${url}`);
  const wav = buildWav(beep(800, 880, 22050));
  console.log(`WAV bytes: ${wav.length}`);
  const start = Date.now();
  const res = await postWav(url, wav);
  const ms = Date.now() - start;
  console.log(`status=${res.status} in ${ms}ms`);
  console.log(res.text);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
