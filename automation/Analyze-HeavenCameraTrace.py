#!/usr/bin/env python3
"""Analyze the bounded DXVK Heaven camera UBO trace.

The trace is intentionally read-only evidence.  It cannot prove what the Host
GPU consumed, but it can decide whether the camera state already regressed at
the DXVK draw boundary before a renderer or present investigation continues.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
from pathlib import Path


UBO_RE = re.compile(
    r"WineHuaUbo: frame=(\d+) pass=0 binding=(\d+).*?"
    r"sliceHandle=0x([0-9a-fA-F]+) sliceOffset=(\d+).*?"
    r"hash=0x([0-9a-fA-F]+) words=\[([^]]*)\]"
)


def u32_to_float(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value))[0]


def distance(left: list[float], right: list[float]) -> float:
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(left, right)))


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[round((len(ordered) - 1) * fraction)]


def parse_sessions(path: Path) -> list[list[dict[str, object]]]:
    sessions: list[list[dict[str, object]]] = []
    current: list[dict[str, object]] = []
    previous_frame: int | None = None

    with path.open("r", encoding="utf-8", errors="ignore") as stream:
        for line in stream:
            match = UBO_RE.search(line)
            if not match:
                continue

            frame = int(match.group(1))
            if previous_frame is not None and frame + 5 < previous_frame:
                if current:
                    sessions.append(current)
                current = []
            previous_frame = frame

            words = [
                int(value, 16)
                for value in match.group(6).split(",")
                if value
            ]
            current.append(
                {
                    "frame": frame,
                    "binding": int(match.group(2)),
                    "sliceHandle": match.group(3).lower(),
                    "sliceOffset": int(match.group(4)),
                    "hash": match.group(5).lower(),
                    "values": [u32_to_float(value) for value in words],
                }
            )

    if current:
        sessions.append(current)
    return sessions


def analyze(path: Path, history: int, old_ratio: float) -> dict[str, object]:
    sessions = parse_sessions(path)
    if not sessions:
        raise RuntimeError("no WineHuaUbo records found")

    session = sessions[-1]
    by_binding: dict[int, dict[int, dict[str, object]]] = {}
    for record in session:
        binding = int(record["binding"])
        frame = int(record["frame"])
        by_binding.setdefault(binding, {})[frame] = record

    camera = by_binding.get(3, {})
    frames = sorted(camera)
    if len(frames) < 3:
        raise RuntimeError("fewer than three binding-3 camera records")

    steps: list[dict[str, object]] = []
    old_candidates: list[dict[str, object]] = []
    hash_repeats: list[dict[str, object]] = []
    phase_sign_flips: list[dict[str, object]] = []
    hash_history: dict[str, int] = {}
    previous_phase_delta: float | None = None

    for index, frame in enumerate(frames):
        current = camera[frame]
        current_hash = str(current["hash"])
        if current_hash in hash_history:
            hash_repeats.append(
                {"frame": frame, "previousFrame": hash_history[current_hash]}
            )
        hash_history[current_hash] = frame

        if index == 0:
            continue

        previous_frame = frames[index - 1]
        previous = camera[previous_frame]
        current_values = list(current["values"])
        previous_values = list(previous["values"])
        step = distance(previous_values, current_values)
        steps.append({"frame": frame, "distance": step})

        older_frames = frames[max(0, index - history) : index - 1]
        if older_frames and step > 0.01:
            older_distance, older_frame = min(
                (distance(list(camera[item]["values"]), current_values), item)
                for item in older_frames
            )
            if older_distance < step * old_ratio:
                old_candidates.append(
                    {
                        "frame": frame,
                        "previousDistance": step,
                        "olderFrame": older_frame,
                        "olderDistance": older_distance,
                    }
                )

        if len(current_values) >= 2 and len(previous_values) >= 2:
            phase = math.atan2(current_values[1], current_values[0])
            previous_phase = math.atan2(previous_values[1], previous_values[0])
            phase_delta = (phase - previous_phase + math.pi) % (2 * math.pi) - math.pi
            if (
                previous_phase_delta is not None
                and phase_delta * previous_phase_delta < 0
                and abs(phase_delta) > 0.005
                and abs(previous_phase_delta) > 0.005
            ):
                phase_sign_flips.append(
                    {
                        "frame": frame,
                        "previousDelta": previous_phase_delta,
                        "delta": phase_delta,
                    }
                )
            previous_phase_delta = phase_delta

    distances = [float(item["distance"]) for item in steps]
    largest_steps = sorted(
        steps, key=lambda item: float(item["distance"]), reverse=True
    )[:20]
    binding_counts = {
        str(binding): len(records) for binding, records in sorted(by_binding.items())
    }
    binding3_and_4 = sorted(set(camera).intersection(by_binding.get(4, {})))

    return {
        "schemaVersion": 1,
        "source": str(path),
        "sessionCount": len(sessions),
        "selectedSession": len(sessions) - 1,
        "selectedRecordCount": len(session),
        "frameRange": [frames[0], frames[-1]],
        "cameraFrameCount": len(frames),
        "bindingCounts": binding_counts,
        "binding3And4FrameCount": len(binding3_and_4),
        "stepDistance": {
            "p50": percentile(distances, 0.50),
            "p95": percentile(distances, 0.95),
            "p99": percentile(distances, 0.99),
            "max": max(distances),
        },
        "largestSteps": largest_steps,
        "olderFrameCandidates": old_candidates,
        "hashRepeats": hash_repeats,
        "phaseSignFlips": phase_sign_flips,
        "guestCameraVerdict": (
            "REGRESSION-CANDIDATES"
            if old_candidates or hash_repeats
            else "NO-OLD-CAMERA-GENERATION-DETECTED"
        ),
        "scope": (
            "DXVK draw-boundary values only; Host consumption and source-image "
            "lifetime still require renderer/present correlation"
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--history", type=int, default=12)
    parser.add_argument("--old-ratio", type=float, default=0.35)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    result = analyze(args.trace, args.history, args.old_ratio)
    encoded = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
