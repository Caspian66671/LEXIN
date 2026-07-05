# LeXin Face Embedding Backend

The board still posts cropped RGB565 faces to:

- `POST /face-register`
- `POST /face-recognize`

The PC proxy now calls `tools/face_embedding.py` to produce a normalized face
embedding. The preferred backend is:

```text
Python + onnxruntime + MobileFaceNet/ArcFace ONNX
```

## Model Path

Put the ONNX model here by default:

```text
tools/face_models/mobilefacenet.onnx
```

Or override it before starting the proxy:

```powershell
$env:LEXIN_FACE_ONNX_MODEL="D:\path\to\your\model.onnx"
.\start_proxy.bat
```

## Python Path

The proxy automatically prefers:

```text
D:\esp32p4\classmate_code\.face_venv\Scripts\python.exe
```

If that file does not exist, it falls back to ESP-IDF's Python:

```text
D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe
```

You can use a dedicated venv instead:

```powershell
$env:LEXIN_FACE_PYTHON="D:\esp32p4\classmate_code\.face_venv\Scripts\python.exe"
.\start_proxy.bat
```

## Install Dependencies

Use a separate virtual environment if possible:

```powershell
cd D:\esp32p4\classmate_code
py -3.11 -m venv .face_venv
.\.face_venv\Scripts\python.exe -m pip install --upgrade pip
.\.face_venv\Scripts\python.exe -m pip install numpy onnxruntime
.\start_proxy.bat
```

If `onnxruntime`, `numpy`, or the model file is missing, the helper uses a
fallback grayscale embedding. That fallback keeps the demo running but is not
a real face recognition model.

## Useful Tuning

Cosine threshold:

```powershell
$env:LEXIN_FACE_COSINE_THRESHOLD="0.42"
```

Typical ranges:

- fallback embedding: `0.40 - 0.65`
- real ArcFace/MobileFaceNet: tune with board photos; often `0.45 - 0.60`

After switching from hash to embedding, re-register each real user once so
`tools/face_users.json` gets `embeddings` samples.
