#!/usr/bin/env python3
"""FunASR Mandarin speech-to-text helper for the LeXin proxy.

The proxy invokes this script as an external ASR command:

    python asr_funasr.py <wav_path> [lang]

It prints a single JSON object to stdout, e.g. {"text": "今天的天气"}.
On any failure it still prints JSON with an "error" field and a non-zero
exit code so the proxy can fall back to its rule engine.

Loading the Paraformer model takes 10-30 s. To keep conversation latency
low, the same script can run as a tiny local daemon that loads the model
once and answers many requests:

    python asr_funasr.py --serve [--port 8799]

When a daemon is reachable on 127.0.0.1:<port>, the one-shot mode simply
forwards the request to it (fast path). If no daemon is running the
one-shot mode loads the model in-process (slow, but works).
"""

import argparse
import json
import os
import sys
import urllib.request

DEFAULT_DAEMON_PORT = int(os.environ.get("LEXIN_ASR_DAEMON_PORT", "8799"))
DEFAULT_MODEL = os.environ.get("LEXIN_ASR_MODEL", "paraformer-zh")
DEFAULT_VAD_MODEL = os.environ.get("LEXIN_ASR_VAD_MODEL", "fsmn-vad")
DEFAULT_PUNC_MODEL = os.environ.get("LEXIN_ASR_PUNC_MODEL", "ct-punc")

_MODEL = None


def _eprint(*args):
    print(*args, file=sys.stderr, flush=True)


def load_model():
    """Load (and cache) the FunASR AutoModel. Heavy: call once."""
    global _MODEL
    if _MODEL is not None:
        return _MODEL
    from funasr import AutoModel  # imported lazily so --help stays fast

    kwargs = {"model": DEFAULT_MODEL, "disable_update": True}
    # VAD + punctuation make free-form conversation transcripts far more
    # usable. They are optional; if the names are unavailable FunASR will
    # raise and we retry with the bare acoustic model.
    if DEFAULT_VAD_MODEL:
        kwargs["vad_model"] = DEFAULT_VAD_MODEL
    if DEFAULT_PUNC_MODEL:
        kwargs["punc_model"] = DEFAULT_PUNC_MODEL
    try:
        _MODEL = AutoModel(**kwargs)
    except Exception as exc:  # noqa: BLE001 - degrade gracefully
        _eprint(f"asr: full pipeline load failed ({exc}); retry acoustic-only")
        _MODEL = AutoModel(model=DEFAULT_MODEL, disable_update=True)
    return _MODEL


def clean_text(text):
    """FunASR returns Chinese tokens separated by spaces; join them."""
    if not text:
        return ""
    # Remove spaces between CJK characters while keeping spaces that
    # separate latin words.
    out = []
    prev_cjk = False
    for ch in str(text):
        is_cjk = "\u4e00" <= ch <= "\u9fff"
        if ch == " " and prev_cjk:
            prev_cjk = False
            continue
        out.append(ch)
        prev_cjk = is_cjk
    return "".join(out).strip()


def transcribe_file(wav_path):
    model = load_model()
    res = model.generate(input=wav_path)
    text = ""
    if isinstance(res, list) and res:
        text = res[0].get("text", "") if isinstance(res[0], dict) else str(res[0])
    elif isinstance(res, dict):
        text = res.get("text", "")
    return clean_text(text)


# ----------------------------------------------------------------------
# Daemon mode
# ----------------------------------------------------------------------

def run_daemon(port):
    from http.server import BaseHTTPRequestHandler, HTTPServer

    _eprint(f"asr daemon: loading model '{DEFAULT_MODEL}' ...")
    load_model()
    _eprint(f"asr daemon: ready on 127.0.0.1:{port}")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # keep the console quiet

        def _send(self, code, obj):
            payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("content-type", "application/json; charset=utf-8")
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            if self.path == "/health":
                self._send(200, {"ok": True})
            else:
                self._send(404, {"ok": False})

        def do_POST(self):
            length = int(self.headers.get("content-length", 0))
            raw = self.rfile.read(length) if length else b"{}"
            try:
                req = json.loads(raw.decode("utf-8") or "{}")
                wav = req.get("wav")
                if not wav or not os.path.exists(wav):
                    self._send(400, {"error": "wav path missing"})
                    return
                text = transcribe_file(wav)
                self._send(200, {"text": text})
            except Exception as exc:  # noqa: BLE001
                self._send(500, {"error": str(exc)})

    HTTPServer(("127.0.0.1", port), Handler).serve_forever()


def try_daemon(wav_path, port):
    """Return transcript via a running daemon, or None if unreachable."""
    url = f"http://127.0.0.1:{port}/transcribe"
    data = json.dumps({"wav": wav_path}).encode("utf-8")
    req = urllib.request.Request(url, data=data,
                                 headers={"content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            out = json.loads(resp.read().decode("utf-8"))
            if "text" in out:
                return out["text"]
    except Exception:  # noqa: BLE001 - daemon simply not available
        return None
    return None


def main():
    parser = argparse.ArgumentParser(description="LeXin FunASR helper")
    parser.add_argument("wav", nargs="?", help="path to a WAV file")
    parser.add_argument("lang", nargs="?", default="zh")
    parser.add_argument("--serve", action="store_true", help="run as daemon")
    parser.add_argument("--port", type=int, default=DEFAULT_DAEMON_PORT)
    args = parser.parse_args()

    if args.serve:
        run_daemon(args.port)
        return

    if not args.wav:
        print(json.dumps({"error": "no wav path"}))
        sys.exit(2)

    # Fast path: an already-warm daemon.
    text = try_daemon(args.wav, args.port)
    if text is None:
        # Slow path: load the model in this process.
        try:
            text = transcribe_file(args.wav)
        except Exception as exc:  # noqa: BLE001
            print(json.dumps({"error": str(exc)}, ensure_ascii=True))
            sys.exit(1)

    # Keep stdout ASCII-only. On Windows, Python may encode pipe stdout
    # with the active ANSI code page; escaping CJK avoids Node decoding
    # GBK bytes as UTF-8 and turning transcripts into mojibake.
    print(json.dumps({"text": text}, ensure_ascii=True))


if __name__ == "__main__":
    main()
