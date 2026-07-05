"""Train a small facial-expression CNN on FER2013 for the ESP32-P4 edge model.

Output: a PyTorch checkpoint at tools/emotion/emotion_cnn.pt that
tools/export_emotion_espdl.py quantizes into main/models/expression.espdl.

Data sources (tried in order):
  1. A local Kaggle-format CSV (columns: emotion, pixels, Usage).
     Point to it with the FER2013_CSV env var, or drop it at
     tools/emotion/data/fer2013.csv
  2. A HuggingFace image dataset (no Kaggle login needed). Override the
     repo id with FER2013_HF_REPO; a few known public mirrors are tried.

The model input is 48x48 single-channel, normalised to [-1, 1] via
(pixel - 127.5) / 127.5 -- this MUST match the on-device preprocessing in
components/lexin_vision/port/expression_adapter_esp_dl.cpp.

The output is 7 raw logits in this fixed CANONICAL order, chosen so that
indices 0/1/2 keep the meaning the firmware already used:
    0 neutral  1 happy  2 sad  3 angry  4 surprise  5 fear  6 disgust
"""
import os
from pathlib import Path

import numpy as np
import torch
from torch import nn
from torch.utils.data import Dataset, DataLoader
from PIL import Image, ImageFilter, ImageOps

ROOT = Path(__file__).resolve().parents[1]
EMOTION_DIR = ROOT / "tools" / "emotion"
CHECKPOINT_PATH = EMOTION_DIR / "emotion_cnn.pt"
REPORT_PATH = EMOTION_DIR / "emotion_eval_report.txt"
LOCAL_CSV_DEFAULT = EMOTION_DIR / "data" / "fer2013.csv"
BOARD_DATA_DEFAULT = EMOTION_DIR / "board_data"

IMG_SIZE = 48
NUM_CLASSES = 7

# Firmware-facing canonical order (index -> name).
CANONICAL_LABELS = ["neutral", "happy", "sad", "angry", "surprise", "fear", "disgust"]
CANONICAL_INDEX = {name: i for i, name in enumerate(CANONICAL_LABELS)}

# Kaggle fer2013.csv native emotion column ordering.
KAGGLE_ORDER = ["angry", "disgust", "fear", "happy", "sad", "surprise", "neutral"]

# Known public HuggingFace mirrors (image-folder style, label feature with names).
HF_REPOS = [
    os.environ.get("FER2013_HF_REPO", ""),
    "mehmet-3emin/fer2013-cleaned",
    "abhilash88/fer2013-enhanced",
]

IMAGE_SUFFIXES = {".bmp", ".jpeg", ".jpg", ".png", ".ppm", ".webp"}


def _normalise_label_name(name) -> str:
    """Map a dataset's label string/alias onto a CANONICAL_LABELS name."""
    text = str(name).strip().lower()
    aliases = {
        "anger": "angry",
        "angry": "angry",
        "disgust": "disgust",
        "disgusted": "disgust",
        "fear": "fear",
        "fearful": "fear",
        "afraid": "fear",
        "happy": "happy",
        "happiness": "happy",
        "joy": "happy",
        "sad": "sad",
        "sadness": "sad",
        "surprise": "surprise",
        "surprised": "surprise",
        "neutral": "neutral",
        "calm": "neutral",
    }
    return aliases.get(text, text)


def _from_kaggle_csv(csv_path: Path):
    """Parse a Kaggle fer2013.csv into (train_x, train_y, test_x, test_y).

    Tolerant of header variants: emotion/pixels/Usage in any case, with or
    without leading spaces (e.g. icml_face_data.csv uses ' pixels')."""
    print(f"Loading FER2013 from local CSV: {csv_path}")
    import csv

    train_x, train_y, test_x, test_y = [], [], [], []
    with open(csv_path, "r", newline="") as fh:
        reader = csv.DictReader(fh)
        # Map normalised header -> actual field name.
        keymap = {k.strip().lower(): k for k in (reader.fieldnames or [])}
        emo_k = keymap.get("emotion")
        pix_k = keymap.get("pixels")
        use_k = keymap.get("usage")
        if emo_k is None or pix_k is None:
            raise RuntimeError(
                f"CSV {csv_path.name} is missing 'emotion'/'pixels' columns "
                f"(found {reader.fieldnames}). This is not the classic "
                "fer2013.csv (emotion, pixels, Usage)."
            )
        for row in reader:
            emotion = int(str(row[emo_k]).strip())
            pixels = np.fromstring(row[pix_k], dtype=np.uint8, sep=" ")
            if pixels.size != IMG_SIZE * IMG_SIZE:
                continue
            image = pixels.reshape(IMG_SIZE, IMG_SIZE)
            canonical = CANONICAL_INDEX[KAGGLE_ORDER[emotion]]
            usage = (row.get(use_k, "Training") if use_k else "Training") or "Training"
            if usage.strip().lower().startswith("train"):
                train_x.append(image)
                train_y.append(canonical)
            else:  # PublicTest + PrivateTest -> evaluation
                test_x.append(image)
                test_y.append(canonical)
    print(f"  -> parsed {len(train_x)} train / {len(test_x)} test from CSV")
    return (
        np.asarray(train_x, dtype=np.uint8),
        np.asarray(train_y, dtype=np.int64),
        np.asarray(test_x, dtype=np.uint8),
        np.asarray(test_y, dtype=np.int64),
    )


def _to_gray48(value, Image):
    """Coerce one dataset 'image' value into a 48x48 uint8 array.

    Handles PIL images, HF encoded-image dicts ({'bytes'/'path'}), and raw
    arrays/lists of any dtype or shape (some mirrors store int64 2-D pixel
    arrays, which PIL.fromarray rejects until cast to uint8)."""
    import io

    if isinstance(value, Image.Image):
        img = value
    elif isinstance(value, dict) and value.get("bytes"):
        img = Image.open(io.BytesIO(value["bytes"]))
    elif isinstance(value, dict) and value.get("path"):
        img = Image.open(value["path"])
    else:
        arr = np.squeeze(np.asarray(value))
        if arr.ndim == 1 and arr.size == IMG_SIZE * IMG_SIZE:
            arr = arr.reshape(IMG_SIZE, IMG_SIZE)
        if arr.dtype != np.uint8:
            arr = np.clip(arr, 0, 255).astype(np.uint8)
        img = Image.fromarray(arr)
    img = img.convert("L").resize((IMG_SIZE, IMG_SIZE))
    return np.asarray(img, dtype=np.uint8)


def _from_huggingface():
    """Download FER2013 from a public HuggingFace mirror."""
    from datasets import load_dataset
    from PIL import Image

    last_err = None
    for repo in HF_REPOS:
        if not repo:
            continue
        try:
            print(f"Trying HuggingFace dataset: {repo}")
            ds = load_dataset(repo)
        except Exception as exc:  # noqa: BLE001 - try the next mirror
            print(f"  -> failed: {exc}")
            last_err = exc
            continue

        # Resolve split names (train + a test/validation split).
        splits = list(ds.keys())
        train_split = "train" if "train" in splits else splits[0]
        test_split = next(
            (s for s in ("test", "validation", "valid", "public_test") if s in splits),
            None,
        )

        # Detect the image and label columns (names vary between mirrors).
        cols = list(ds[train_split].features.keys())
        img_col = next((c for c in ("image", "img", "pixels", "pixel_values") if c in cols), None)
        lbl_col = next((c for c in ("label", "labels", "emotion", "class") if c in cols), None)
        if img_col is None or lbl_col is None:
            print(f"  -> repo {repo} has unexpected columns {cols}; skipping")
            last_err = RuntimeError(f"unexpected columns {cols}")
            continue

        def label_names(split):
            feat = ds[split].features.get(lbl_col)
            return getattr(feat, "names", None)

        def convert(split):
            names = label_names(split)
            xs, ys = [], []
            for ex in ds[split]:
                xs.append(_to_gray48(ex[img_col], Image))
                raw_label = ex[lbl_col]
                # int label without ClassLabel names -> assume Kaggle ordering.
                if names is not None:
                    name = names[raw_label]
                elif isinstance(raw_label, int):
                    name = KAGGLE_ORDER[raw_label]
                else:
                    name = raw_label
                ys.append(CANONICAL_INDEX[_normalise_label_name(name)])
            return np.asarray(xs, dtype=np.uint8), np.asarray(ys, dtype=np.int64)

        train_x, train_y = convert(train_split)
        if test_split is not None:
            test_x, test_y = convert(test_split)
        else:  # carve a 10% holdout out of train
            rng = np.random.default_rng(7)
            idx = rng.permutation(len(train_x))
            cut = int(len(idx) * 0.9)
            test_x, test_y = train_x[idx[cut:]], train_y[idx[cut:]]
            train_x, train_y = train_x[idx[:cut]], train_y[idx[:cut]]
        print(f"  -> using repo {repo}: {len(train_x)} train / {len(test_x)} test")
        return train_x, train_y, test_x, test_y

    raise RuntimeError(
        "Could not load FER2013 from any HuggingFace mirror. "
        "Set FER2013_CSV to a local fer2013.csv, or FER2013_HF_REPO to a working "
        f"dataset repo. Last error: {last_err}"
    )


def load_fer2013():
    csv_env = os.environ.get("FER2013_CSV", "")
    if csv_env and Path(csv_env).exists():
        return _from_kaggle_csv(Path(csv_env))
    if LOCAL_CSV_DEFAULT.exists():
        return _from_kaggle_csv(LOCAL_CSV_DEFAULT)
    # Convenience: pick up any *.csv dropped into tools/emotion/data/ so the
    # exact filename (fer2013.csv / icml_face_data.csv / ...) doesn't matter.
    data_dir = LOCAL_CSV_DEFAULT.parent
    if data_dir.exists():
        found = sorted(data_dir.glob("*.csv"))
        if found:
            return _from_kaggle_csv(found[0])
    return _from_huggingface()


def load_image_folder(root: Path):
    """Load board-captured images from class subfolders.

    Expected layout examples:
      tools/emotion/board_data/neutral/*.jpg
      tools/emotion/board_data/train/happy/*.png
      tools/emotion/board_data/test/surprise/*.jpg
    """
    root = Path(root)
    if not root.exists():
        return np.empty((0, IMG_SIZE, IMG_SIZE), dtype=np.uint8), np.empty((0,), dtype=np.int64)

    xs, ys = [], []
    for label_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        label_name = _normalise_label_name(label_dir.name)
        if label_name not in CANONICAL_INDEX:
            continue
        label = CANONICAL_INDEX[label_name]
        for path in sorted(label_dir.rglob("*")):
            if path.suffix.lower() not in IMAGE_SUFFIXES:
                continue
            try:
                with Image.open(path) as img:
                    xs.append(_to_gray48(img, Image))
                    ys.append(label)
            except Exception as exc:  # noqa: BLE001 - skip bad captures
                print(f"  -> skip bad board image {path}: {exc}")
    return np.asarray(xs, dtype=np.uint8), np.asarray(ys, dtype=np.int64)


def load_board_dataset():
    root = Path(os.environ.get("EMOTION_BOARD_DATA", BOARD_DATA_DEFAULT))
    if not root.exists():
        return (
            np.empty((0, IMG_SIZE, IMG_SIZE), dtype=np.uint8),
            np.empty((0,), dtype=np.int64),
            np.empty((0, IMG_SIZE, IMG_SIZE), dtype=np.uint8),
            np.empty((0,), dtype=np.int64),
        )

    if (root / "train").exists() or (root / "test").exists():
        train_x, train_y = load_image_folder(root / "train")
        test_x, test_y = load_image_folder(root / "test")
    else:
        train_x, train_y = load_image_folder(root)
        test_x = np.empty((0, IMG_SIZE, IMG_SIZE), dtype=np.uint8)
        test_y = np.empty((0,), dtype=np.int64)

    print(f"Loaded board captures from {root}: {len(train_x)} train / {len(test_x)} test")
    for i, name in enumerate(CANONICAL_LABELS):
        if len(train_y) or len(test_y):
            print(f"  board {i} {name:9s} train={(train_y == i).sum():4d} test={(test_y == i).sum():4d}")
    return train_x, train_y, test_x, test_y


def merge_board_data(train_x, train_y, test_x, test_y):
    board_train_x, board_train_y, board_test_x, board_test_y = load_board_dataset()
    if len(board_train_x):
        repeat = max(1, int(os.environ.get("EMOTION_BOARD_REPEAT", "4")))
        train_x = np.concatenate([train_x] + [board_train_x] * repeat, axis=0)
        train_y = np.concatenate([train_y] + [board_train_y] * repeat, axis=0)
        print(f"Mixed board train captures into training set x{repeat}: train={len(train_x)}")
    if len(board_test_x):
        test_x = np.concatenate([test_x, board_test_x], axis=0)
        test_y = np.concatenate([test_y, board_test_y], axis=0)
        print(f"Mixed board test captures into eval set: test={len(test_x)}")
    return train_x, train_y, test_x, test_y


class FerDataset(Dataset):
    def __init__(self, images, labels, augment=False):
        self.images = images  # uint8 [N,48,48]
        self.labels = labels
        self.augment = augment

    def __len__(self):
        return len(self.images)

    def __getitem__(self, index):
        if self.augment:
            img = self._augment(self.images[index])
        else:
            img = self.images[index].astype(np.float32)
        img = ((img - 127.5) / 127.5).astype(np.float32)  # -> [-1, 1], matches firmware
        tensor = torch.from_numpy(np.ascontiguousarray(img)).unsqueeze(0)  # [1,48,48]
        return tensor, int(self.labels[index])

    @staticmethod
    def _augment(src_uint8):
        """Train-time only augmentation. Does NOT touch the device contract:
        the result is still a 48x48 grayscale image, normalised the same way."""
        pil = Image.fromarray(src_uint8)
        # geometric: horizontal flip + random crop + small rotation/translation.
        if np.random.rand() < 0.5:
            pil = ImageOps.mirror(pil)
        if np.random.rand() < 0.65:
            scale = float(np.random.uniform(0.84, 1.0))
            crop = max(36, int(IMG_SIZE * scale))
            x0 = int(np.random.randint(0, IMG_SIZE - crop + 1))
            y0 = int(np.random.randint(0, IMG_SIZE - crop + 1))
            pil = pil.crop((x0, y0, x0 + crop, y0 + crop)).resize((IMG_SIZE, IMG_SIZE))
        angle = float(np.random.uniform(-15.0, 15.0))
        tx, ty = (int(v) for v in np.random.randint(-5, 6, size=2))
        fill = int(np.clip(np.asarray(src_uint8).mean(), 0, 255))
        pil = pil.rotate(angle, resample=Image.BILINEAR, translate=(tx, ty), fillcolor=fill)
        if np.random.rand() < 0.20:
            pil = pil.filter(ImageFilter.GaussianBlur(radius=float(np.random.uniform(0.2, 0.9))))
        img = np.asarray(pil, dtype=np.float32)
        # photometric: strong board-camera lighting variation. This teaches the
        # model not to confuse "bright/washed out" with SURPRISE or "dim" with no expression.
        if np.random.rand() < 0.40:
            gamma = float(np.random.uniform(0.55, 1.85))
            img = 255.0 * np.power(np.clip(img / 255.0, 0.0, 1.0), gamma)
        img = (img - img.mean()) * float(np.random.uniform(0.45, 1.65)) + img.mean()
        img = img * float(np.random.uniform(0.45, 1.85))
        img = img + float(np.random.uniform(-38.0, 38.0))
        if np.random.rand() < 0.18:  # low-light frame
            img = img * float(np.random.uniform(0.28, 0.72))
        if np.random.rand() < 0.18:  # flashlight / overexposure frame
            img = img * float(np.random.uniform(1.35, 2.15)) + float(np.random.uniform(8.0, 32.0))
        if np.random.rand() < 0.25:
            noise = np.random.normal(0.0, float(np.random.uniform(2.0, 9.0)), img.shape)
            img = img + noise
        img = np.clip(img, 0.0, 255.0)
        # random local glare/shadow patch.
        if np.random.rand() < 0.25:
            h = int(np.random.randint(8, 20))
            w = int(np.random.randint(8, 20))
            y0 = int(np.random.randint(0, IMG_SIZE - h + 1))
            x0 = int(np.random.randint(0, IMG_SIZE - w + 1))
            delta = float(np.random.choice([-1, 1]) * np.random.uniform(28.0, 90.0))
            img[y0:y0 + h, x0:x0 + w] = np.clip(img[y0:y0 + h, x0:x0 + w] + delta, 0.0, 255.0)
        # cutout: blank a small random patch to fight overfitting
        if np.random.rand() < 0.3:
            s = np.random.randint(6, 13)
            y0 = np.random.randint(0, IMG_SIZE - s + 1)
            x0 = np.random.randint(0, IMG_SIZE - s + 1)
            img[y0:y0 + s, x0:x0 + s] = float(img.mean())
        return img.astype(np.float32)


class EmotionCNN(nn.Module):
    """Compact, quantisation-friendly CNN (Conv/BN/ReLU/MaxPool/Linear only)."""

    def __init__(self, num_classes=NUM_CLASSES):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(1, 32, 3, padding=1), nn.BatchNorm2d(32), nn.ReLU(),
            nn.MaxPool2d(2),                                       # 24x24
            nn.Conv2d(32, 64, 3, padding=1), nn.BatchNorm2d(64), nn.ReLU(),
            nn.MaxPool2d(2),                                       # 12x12
            nn.Conv2d(64, 128, 3, padding=1), nn.BatchNorm2d(128), nn.ReLU(),
            nn.MaxPool2d(2),                                       # 6x6
            nn.Conv2d(128, 128, 3, padding=1), nn.BatchNorm2d(128), nn.ReLU(),
            nn.MaxPool2d(2),                                       # 3x3
        )
        self.classifier = nn.Sequential(
            nn.Flatten(),
            nn.Dropout(0.4),
            nn.Linear(128 * 3 * 3, 128), nn.BatchNorm1d(128), nn.ReLU(),
            nn.Dropout(0.3),
            nn.Linear(128, num_classes),
        )

    def forward(self, x):
        return self.classifier(self.features(x))


def class_weights(labels):
    counts = np.bincount(labels, minlength=NUM_CLASSES).astype(np.float32)
    counts[counts == 0] = 1.0
    weights = counts.sum() / (NUM_CLASSES * counts)
    return torch.tensor(weights, dtype=torch.float32)


def train(epochs=None, batch_size=128, lr=1e-3):
    if epochs is None:
        epochs = int(os.environ.get("EMOTION_EPOCHS", "60"))
    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"device: {device}  epochs: {epochs}")
    torch.manual_seed(7)
    np.random.seed(7)

    train_x, train_y, test_x, test_y = merge_board_data(*load_fer2013())
    print(f"train={len(train_x)} test={len(test_x)} classes={NUM_CLASSES}")
    for i, name in enumerate(CANONICAL_LABELS):
        print(f"  {i} {name:9s} train={(train_y == i).sum():5d} test={(test_y == i).sum():4d}")

    train_loader = DataLoader(
        FerDataset(train_x, train_y, augment=True),
        batch_size=batch_size, shuffle=True, num_workers=0, drop_last=True,
    )
    test_loader = DataLoader(
        FerDataset(test_x, test_y, augment=False),
        batch_size=256, shuffle=False, num_workers=0,
    )

    model = EmotionCNN().to(device)
    # class weights handle FER2013 imbalance (disgust is tiny); label smoothing
    # tempers the noisy FER2013 labels and improves generalisation.
    loss_fn = nn.CrossEntropyLoss(
        weight=class_weights(train_y).to(device), label_smoothing=0.05
    )
    optimizer = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs)

    best_acc = 0.0
    for epoch in range(1, epochs + 1):
        model.train()
        running = 0.0
        for xb, yb in train_loader:
            xb, yb = xb.to(device), yb.to(device)
            optimizer.zero_grad()
            loss = loss_fn(model(xb), yb)
            loss.backward()
            optimizer.step()
            running += loss.item() * xb.size(0)

        acc = evaluate(model, test_loader, device)
        scheduler.step()
        lr_now = optimizer.param_groups[0]["lr"]
        print(f"epoch {epoch:2d}/{epochs}  loss={running / len(train_x):.4f}  "
              f"test_acc={acc:.4f}  lr={lr_now:.5f}")
        if acc > best_acc:
            best_acc = acc
            save_checkpoint(model, best_acc)

    if CHECKPOINT_PATH.exists():
        ckpt = torch.load(CHECKPOINT_PATH, map_location=device)
        model.load_state_dict(ckpt["state_dict"])
    final_acc, conf = evaluate_with_confusion(model, test_loader, device)
    write_eval_report(conf, final_acc)
    print(f"best test accuracy: {best_acc:.4f}")
    print(f"eval report: {REPORT_PATH}")
    print(f"checkpoint: {CHECKPOINT_PATH}")
    return model


@torch.no_grad()
def evaluate(model, loader, device):
    acc, _ = evaluate_with_confusion(model, loader, device)
    return acc


@torch.no_grad()
def evaluate_with_confusion(model, loader, device):
    model.eval()
    correct = total = 0
    conf = np.zeros((NUM_CLASSES, NUM_CLASSES), dtype=np.int64)
    for xb, yb in loader:
        xb, yb = xb.to(device), yb.to(device)
        preds = model(xb).argmax(dim=1)
        correct += (preds == yb).sum().item()
        total += yb.size(0)
        for y, p in zip(yb.cpu().numpy(), preds.cpu().numpy()):
            conf[int(y), int(p)] += 1
    return correct / max(1, total), conf


def write_eval_report(conf, accuracy):
    EMOTION_DIR.mkdir(parents=True, exist_ok=True)
    lines = [
        f"accuracy: {accuracy:.4f}",
        f"labels: {CANONICAL_LABELS}",
        "confusion matrix rows=true cols=pred:",
        str(conf),
        "per-class:",
    ]
    pred_counts = conf.sum(axis=0)
    for i, name in enumerate(CANONICAL_LABELS):
        total = int(conf[i].sum())
        recall = float(conf[i, i] / total) if total else 0.0
        pred_rate = float(pred_counts[i] / max(1, conf.sum()))
        top = int(conf[i].argmax()) if total else 0
        lines.append(
            f"{i} {name:9s} recall={recall:.3f} "
            f"top_pred={CANONICAL_LABELS[top]} {int(conf[i, top])}/{total} "
            f"pred_rate={pred_rate:.3f}"
        )
    text = "\n".join(lines) + "\n"
    REPORT_PATH.write_text(text, encoding="utf-8")
    print(text)


def save_checkpoint(model, accuracy):
    EMOTION_DIR.mkdir(parents=True, exist_ok=True)
    torch.save(
        {
            "state_dict": model.state_dict(),
            "labels": CANONICAL_LABELS,
            "img_size": IMG_SIZE,
            "accuracy": accuracy,
        },
        CHECKPOINT_PATH,
    )


if __name__ == "__main__":
    os.environ.setdefault("CUDA_VISIBLE_DEVICES", os.environ.get("CUDA_VISIBLE_DEVICES", ""))
    train()
