"""Quantise the trained FER2013 CNN into main/models/expression.espdl.

Run train_emotion_fer2013.py first to produce tools/emotion/emotion_cnn.pt.
If no checkpoint exists this script will train one automatically.

Uses the same esp-ppq toolchain as tools/export_lexin_advisor_espdl.py.
Calibration data is normalised exactly like the device input
((pixel - 127.5) / 127.5) so the int8 input scale lands on ~pixel-128,
which is what expression_adapter_esp_dl.cpp feeds the model.
"""
import os
from pathlib import Path

import numpy as np
import torch
from torch.utils.data import DataLoader, Dataset
from esp_ppq.api import espdl_quantize_torch

import train_emotion_fer2013 as trainer

ROOT = Path(__file__).resolve().parents[1]
ESPDL_MODEL_PATH = ROOT / "main" / "models" / "expression.espdl"
TARGET = "esp32p4"
NUM_OF_BITS = 8
DEVICE = "cpu"
CALIB_SAMPLES = 512


class CalibDataset(Dataset):
    """Yields normalised [1,48,48] tensors, matching device preprocessing."""

    def __init__(self, images):
        self.images = images

    def __len__(self):
        return len(self.images)

    def __getitem__(self, index):
        img = (self.images[index].astype(np.float32) - 127.5) / 127.5
        return torch.from_numpy(img).unsqueeze(0)


def load_or_train_model():
    model = trainer.EmotionCNN()
    if trainer.CHECKPOINT_PATH.exists():
        ckpt = torch.load(trainer.CHECKPOINT_PATH, map_location=DEVICE)
        model.load_state_dict(ckpt["state_dict"])
        print(f"loaded checkpoint (test acc {ckpt.get('accuracy', 0):.4f})")
    else:
        print("no checkpoint found; training first ...")
        model = trainer.train()
    return model.to(DEVICE).eval()


def calibration_images():
    train_x, _, test_x, _ = trainer.load_fer2013()
    rng = np.random.default_rng(7)

    board_train_x, _, board_test_x, _ = trainer.load_board_dataset()
    board_pools = [x for x in (board_test_x, board_train_x) if len(x)]
    board_pool = np.concatenate(board_pools, axis=0) if board_pools else None

    fer_pool = test_x if len(test_x) else train_x
    if board_pool is None or len(board_pool) == 0:
        idx = rng.permutation(len(fer_pool))[:CALIB_SAMPLES]
        return fer_pool[idx]

    board_take = min(len(board_pool), max(32, CALIB_SAMPLES // 3))
    fer_take = max(0, CALIB_SAMPLES - board_take)
    board_idx = rng.permutation(len(board_pool))[:board_take]
    fer_idx = rng.permutation(len(fer_pool))[:fer_take]
    images = np.concatenate([board_pool[board_idx], fer_pool[fer_idx]], axis=0)
    print(f"calibration mix: board={board_take} fer={len(images) - board_take}")
    return images


def export():
    ESPDL_MODEL_PATH.parent.mkdir(parents=True, exist_ok=True)
    model = load_or_train_model()

    images = calibration_images()
    dataloader = DataLoader(CalibDataset(images), batch_size=1, shuffle=False)
    test_input = next(iter(dataloader)).clone()  # [1,1,48,48]
    print(f"input shape: {list(test_input.shape)}  calib samples: {len(images)}")

    espdl_quantize_torch(
        model=model,
        espdl_export_file=str(ESPDL_MODEL_PATH),
        calib_dataloader=dataloader,
        calib_steps=min(CALIB_SAMPLES, len(dataloader)),
        input_shape=[1, 1, trainer.IMG_SIZE, trainer.IMG_SIZE],
        inputs=[test_input],
        target=TARGET,
        num_of_bits=NUM_OF_BITS,
        device=DEVICE,
        error_report=True,
        skip_export=False,
        export_test_values=True,
        verbose=1,
        dispatching_override=None,
    )
    print(f"exported: {ESPDL_MODEL_PATH}")
    print("Rebuild the firmware -- CMake will auto-embed the model and switch")
    print("the expression backend from HEURISTIC to ESP-DL.")


if __name__ == "__main__":
    os.environ.setdefault("CUDA_VISIBLE_DEVICES", "")
    export()
