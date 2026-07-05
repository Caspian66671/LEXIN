#!/usr/bin/env python3
"""LeXin face embedding helper.

The ESP32-P4 uploads a cropped RGB565 face. This helper converts that raw
image into a normalized embedding vector.

Preferred backend:
  onnxruntime + numpy + MobileFaceNet/ArcFace ONNX model.

Fallback backend:
  a deterministic low-resolution grayscale embedding, used only so the
  proxy still runs before the model/dependencies are installed.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from typing import List, Sequence, Tuple


def rgb565le_to_rgb(raw: bytes, width: int, height: int) -> List[Tuple[int, int, int]]:
    count = width * height
    if len(raw) < count * 2:
        raise ValueError(f"short RGB565 body: {len(raw)}/{count * 2}")
    out: List[Tuple[int, int, int]] = []
    for i in range(count):
        value = raw[i * 2] | (raw[i * 2 + 1] << 8)
        r5 = (value >> 11) & 0x1F
        g6 = (value >> 5) & 0x3F
        b5 = value & 0x1F
        r = (r5 << 3) | (r5 >> 2)
        g = (g6 << 2) | (g6 >> 4)
        b = (b5 << 3) | (b5 >> 2)
        out.append((r, g, b))
    return out


def resize_nearest(
    pixels: Sequence[Tuple[int, int, int]],
    width: int,
    height: int,
    out_w: int,
    out_h: int,
) -> List[Tuple[int, int, int]]:
    resized: List[Tuple[int, int, int]] = []
    for y in range(out_h):
        sy = min(height - 1, int(y * height / out_h))
        row = sy * width
        for x in range(out_w):
            sx = min(width - 1, int(x * width / out_w))
            resized.append(pixels[row + sx])
    return resized


def l2_normalize(values: Sequence[float]) -> List[float]:
    norm = math.sqrt(sum(v * v for v in values))
    if norm <= 1e-12:
        return [0.0 for _ in values]
    return [float(v / norm) for v in values]


def fallback_embedding(pixels: Sequence[Tuple[int, int, int]], width: int, height: int) -> List[float]:
    """Small deterministic embedding used before ONNX is installed.

    This is intentionally better than a single 64-bit hash, but it is not a
    real face recognition model. It keeps the demo runnable while the model
    file/dependencies are being prepared.
    """
    out_w, out_h = 16, 16
    small = resize_nearest(pixels, width, height, out_w, out_h)
    gray = [(0.299 * r + 0.587 * g + 0.114 * b) / 255.0 for r, g, b in small]
    mean = sum(gray) / len(gray)
    centered = [g - mean for g in gray]
    # Add coarse row/column statistics so lighting shifts hurt less.
    row_stats = [sum(centered[r * out_w:(r + 1) * out_w]) / out_w for r in range(out_h)]
    col_stats = [sum(centered[r * out_w + c] for r in range(out_h)) / out_h for c in range(out_w)]
    return l2_normalize(centered + row_stats + col_stats)


def infer_model_size(shape: Sequence[object]) -> Tuple[int, int, bool]:
    """Return width, height, channels_last."""
    dims = [int(x) if isinstance(x, int) and x > 0 else 0 for x in shape]
    if len(dims) != 4:
        return 112, 112, False
    # NCHW: [1, 3, H, W]
    if dims[1] == 3:
        return dims[3] or 112, dims[2] or 112, False
    # NHWC: [1, H, W, 3]
    if dims[3] == 3:
        return dims[2] or 112, dims[1] or 112, True
    return 112, 112, False


def onnx_embedding(
    pixels: Sequence[Tuple[int, int, int]],
    width: int,
    height: int,
    model_path: str,
) -> Tuple[List[float], str]:
    try:
        import numpy as np  # type: ignore
        import onnxruntime as ort  # type: ignore
    except Exception as exc:  # pragma: no cover - depends on local env
        raise RuntimeError(f"onnxruntime/numpy not installed: {exc}") from exc

    if not os.path.exists(model_path):
        raise RuntimeError(f"ONNX model not found: {model_path}")

    providers = ["CPUExecutionProvider"]
    sess = ort.InferenceSession(model_path, providers=providers)
    inp = sess.get_inputs()[0]
    out_w, out_h, channels_last = infer_model_size(inp.shape)
    small = resize_nearest(pixels, width, height, out_w, out_h)
    arr = np.asarray(small, dtype=np.float32).reshape((out_h, out_w, 3))
    # ArcFace/MobileFaceNet common preprocessing: RGB, [-1, 1].
    arr = (arr - 127.5) / 128.0
    if channels_last:
        tensor = arr.reshape((1, out_h, out_w, 3))
    else:
        tensor = np.transpose(arr, (2, 0, 1)).reshape((1, 3, out_h, out_w))
    outputs = sess.run(None, {inp.name: tensor})
    emb = np.asarray(outputs[0], dtype=np.float32).reshape(-1)
    norm = float(np.linalg.norm(emb))
    if norm > 1e-12:
        emb = emb / norm
    return [float(x) for x in emb.tolist()], os.path.basename(model_path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--height", required=True, type=int)
    parser.add_argument("--model", default="")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        raw = f.read()
    pixels = rgb565le_to_rgb(raw, args.width, args.height)

    model_path = args.model or os.environ.get("LEXIN_FACE_ONNX_MODEL", "")
    try:
        if model_path:
            emb, model_name = onnx_embedding(pixels, args.width, args.height, model_path)
            result = {
                "ok": True,
                "backend": "onnx",
                "model": model_name,
                "dim": len(emb),
                "embedding": emb,
            }
        else:
            raise RuntimeError("LEXIN_FACE_ONNX_MODEL is not configured")
    except Exception as exc:
        emb = fallback_embedding(pixels, args.width, args.height)
        result = {
            "ok": True,
            "backend": "fallback",
            "model": "grayscale-fallback",
            "dim": len(emb),
            "warning": str(exc),
            "embedding": emb,
        }

    print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        raise SystemExit(2)
