#!/usr/bin/env python3
"""Decode a WineHua DXVK frame capture into metrics and a contact sheet."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


VK_FORMAT_R8G8B8A8_UNORM = 37
VK_FORMAT_R16G16B16A16_SFLOAT = 97
VK_FORMAT_D24_UNORM_S8_UINT = 129


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path, help="directory containing frame JSONL and .bin files")
    parser.add_argument("--frame", type=int, default=120)
    parser.add_argument("--columns", type=int, default=4)
    return parser.parse_args()


def percentile(values: np.ndarray, value: float) -> float:
    if not values.size:
        return 0.0
    return float(np.percentile(values, value))


def decode(record: dict, path: Path) -> tuple[np.ndarray, np.ndarray, dict]:
    width, height, _ = record["extent"]
    layer_count = max(1, int(record.get("layerCount", 1)))
    fmt = int(record["viewFormat"])
    data = path.read_bytes()

    if fmt == VK_FORMAT_R8G8B8A8_UNORM:
        rgba = np.frombuffer(data, dtype=np.uint8).reshape(layer_count, height, width, 4)
        linear = rgba[0, :, :, :3].astype(np.float32) / 255.0
        preview = rgba[0, :, :, :3]
        format_name = "RGBA8"
    elif fmt == VK_FORMAT_R16G16B16A16_SFLOAT:
        rgba = np.frombuffer(data, dtype="<f2").reshape(layer_count, height, width, 4)
        linear = rgba[0, :, :, :3].astype(np.float32)
        finite = np.nan_to_num(linear, nan=0.0, posinf=65504.0, neginf=0.0)
        mapped = np.maximum(finite, 0.0) / (1.0 + np.maximum(finite, 0.0))
        preview = np.clip(np.power(mapped, 1.0 / 2.2) * 255.0, 0, 255).astype(np.uint8)
        format_name = "RGBA16F"
    elif fmt == VK_FORMAT_D24_UNORM_S8_UINT:
        packed = np.frombuffer(data, dtype="<u4").reshape(layer_count, height, width)
        depth = (packed[0] & 0x00FFFFFF).astype(np.float32) / 16777215.0
        linear = depth[:, :, None]
        non_clear = depth[depth < 0.999999]
        if non_clear.size:
            low = percentile(non_clear, 1.0)
            high = percentile(non_clear, 99.0)
            scale = max(high - low, 1e-8)
            visual = 1.0 - np.clip((depth - low) / scale, 0.0, 1.0)
        else:
            visual = np.zeros_like(depth)
        preview = np.repeat((visual[:, :, None] * 255.0).astype(np.uint8), 3, axis=2)
        format_name = "D24S8"
    else:
        raise ValueError(f"unsupported Vulkan format {fmt}")

    finite = linear[np.isfinite(linear)]
    positive = finite[finite > 0.0]
    metrics = {
        "formatName": format_name,
        "finiteRatio": float(np.isfinite(linear).sum() / linear.size),
        "nonzeroRatio": float(np.count_nonzero(linear) / linear.size),
        "negativeRatio": float(np.count_nonzero(linear < 0.0) / linear.size),
        "min": float(np.min(finite)) if finite.size else 0.0,
        "p50": percentile(finite, 50.0),
        "p95": percentile(finite, 95.0),
        "p99": percentile(finite, 99.0),
        "max": float(np.max(finite)) if finite.size else 0.0,
        "positiveP50": percentile(positive, 50.0),
        "overOneRatio": float(np.count_nonzero(linear > 1.0) / linear.size),
    }
    return linear, preview, metrics


def main() -> int:
    args = parse_args()
    metadata = args.capture / f"frame-{args.frame}.jsonl"
    records = [json.loads(line) for line in metadata.read_text(encoding="utf-8").splitlines() if line]
    decoded: list[tuple[dict, Image.Image]] = []
    summaries: list[dict] = []

    for record in records:
        file_name = record.get("file")
        if not file_name:
            continue
        path = args.capture / file_name
        linear, preview, metrics = decode(record, path)
        image = Image.fromarray(preview, "RGB")
        image.thumbnail((320, 180), Image.Resampling.LANCZOS)
        decoded.append((record | metrics, image))
        summaries.append({
            "pass": int(record["pass"]),
            "attachment": int(record["attachment"]),
            "kind": record["kind"],
            "file": file_name,
            "extent": record["extent"],
            "viewFormat": int(record["viewFormat"]),
            "vertexShader": record.get("vertexShader", ""),
            "fragmentShader": record.get("fragmentShader", ""),
            **metrics,
        })

    columns = max(1, args.columns)
    tile_width, tile_height = 340, 226
    rows = math.ceil(len(decoded) / columns)
    sheet = Image.new("RGB", (columns * tile_width, rows * tile_height), (28, 30, 34))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()

    for index, (record, image) in enumerate(decoded):
        x = (index % columns) * tile_width
        y = (index // columns) * tile_height
        image_x = x + (tile_width - image.width) // 2
        sheet.paste(image, (image_x, y + 6))
        extent = record["extent"]
        label = (
            f"P{record['pass']:02d} A{record['attachment']} {record['kind']} "
            f"{record['formatName']} {extent[0]}x{extent[1]}\n"
            f"nz={record['nonzeroRatio']:.3f} p50={record['p50']:.4g} "
            f"p99={record['p99']:.4g} max={record['max']:.4g}"
        )
        draw.multiline_text((x + 6, y + 190), label, fill=(235, 238, 242), font=font, spacing=2)

    summary_path = args.capture / f"frame-{args.frame}-analysis.json"
    sheet_path = args.capture / f"frame-{args.frame}-contact-sheet.png"
    summary_path.write_text(json.dumps({
        "schemaVersion": 1,
        "frame": args.frame,
        "attachmentCount": len(summaries),
        "attachments": summaries,
    }, indent=2), encoding="utf-8")
    sheet.save(sheet_path)
    print(f"attachments={len(summaries)}")
    print(f"summary={summary_path}")
    print(f"contactSheet={sheet_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
