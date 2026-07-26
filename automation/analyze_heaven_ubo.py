#!/usr/bin/env python3
"""Summarize WineHuaUbo frame/binding identity without changing runtime state."""

import argparse
import collections
import json
import re
from pathlib import Path


RECORD_RE = re.compile(
    r"WineHuaUbo: winPid=(?P<pid>\d+) recording=(?P<recording>\d+) "
    r"frame=(?P<frame>\d+) pass=(?P<pass_id>\d+) guestCmd=(?P<cmd>0x[0-9a-f]+) "
    r"binding=(?P<binding>\d+) resourceSlot=(?P<slot>\w+) "
    r"descriptorType=(?P<descriptor_type>\d+) stages=(?P<stages>0x[0-9a-f]+) "
    r"writtenHandle=(?P<handle>0x[0-9a-f]+) "
    r"writtenBaseOffset=(?P<base_offset>\d+) writtenRange=(?P<range>\d+) "
    r"dynamicOffsetBound=(?P<dynamic_bound>[01]) dynamicOffset=(?P<dynamic_offset>\d+) "
    r"sliceHandle=(?P<slice_handle>0x[0-9a-f]+) "
    r"sliceOffset=(?P<slice_offset>\d+) sliceLength=(?P<slice_length>\d+) "
    r"bytes=(?P<bytes>\d+) hash=(?P<hash>0x[0-9a-f]+)"
)


def parse_record(line):
    match = RECORD_RE.search(line)
    if not match:
        return None
    record = match.groupdict()
    for key in (
        "pid", "recording", "frame", "pass_id", "binding",
        "descriptor_type", "base_offset", "range", "dynamic_bound",
        "dynamic_offset", "slice_offset", "slice_length", "bytes",
    ):
        record[key] = int(record[key])
    return record


def summarize_binding(records):
    records.sort(key=lambda item: item["frame"])
    hash_frames = collections.defaultdict(list)
    slice_frames = collections.defaultdict(list)
    immediate_repeats = 0
    historical_replays = []

    previous_hash = None
    seen_hashes = set()
    for record in records:
        content_hash = record["hash"]
        if content_hash == previous_hash:
            immediate_repeats += 1
        elif content_hash in seen_hashes:
            historical_replays.append(record["frame"])
        seen_hashes.add(content_hash)
        previous_hash = content_hash
        hash_frames[content_hash].append(record["frame"])
        slice_key = (
            record["slice_handle"], record["slice_offset"],
            record["slice_length"],
        )
        slice_frames[slice_key].append(record["frame"])

    reuse_gaps = []
    for frames in slice_frames.values():
        reuse_gaps.extend(right - left for left, right in zip(frames, frames[1:]))

    frames = [record["frame"] for record in records]
    return {
        "recordCount": len(records),
        "frameFirst": min(frames),
        "frameLast": max(frames),
        "uniqueHashes": len(hash_frames),
        "immediateHashRepeats": immediate_repeats,
        "historicalHashReplayCount": len(historical_replays),
        "historicalHashReplayFramesFirst20": historical_replays[:20],
        "distinctWrittenHandles": len({record["handle"] for record in records}),
        "distinctPhysicalSlices": len(slice_frames),
        "minimumPhysicalSliceReuseGapFrames": min(reuse_gaps) if reuse_gaps else None,
        "sliceReuseGapAtMost2Count": sum(gap <= 2 for gap in reuse_gaps),
        "writtenRanges": sorted({record["range"] for record in records}),
        "sliceLengths": sorted({record["slice_length"] for record in records}),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    records = []
    malformed = 0
    with args.log.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            if "WineHuaUbo:" not in line:
                continue
            record = parse_record(line)
            if record is None:
                malformed += 1
            else:
                records.append(record)

    by_binding = collections.defaultdict(list)
    by_frame = collections.defaultdict(list)
    for record in records:
        by_binding[record["binding"]].append(record)
        by_frame[record["frame"]].append(record)

    frame_signatures = collections.defaultdict(list)
    for frame, frame_records in by_frame.items():
        signature = tuple(sorted(
            (record["binding"], record["hash"], record["slice_handle"],
             record["slice_offset"])
            for record in frame_records
        ))
        frame_signatures[signature].append(frame)

    duplicate_binding_frames = []
    for frame, frame_records in by_frame.items():
        counts = collections.Counter(record["binding"] for record in frame_records)
        if any(count > 1 for count in counts.values()):
            duplicate_binding_frames.append(frame)

    frames = sorted(by_frame)
    report = {
        "schemaVersion": 1,
        "source": str(args.log),
        "recordCount": len(records),
        "malformedRecordCount": malformed,
        "frameCount": len(frames),
        "frameFirst": frames[0] if frames else None,
        "frameLast": frames[-1] if frames else None,
        "missingFrames": [
            frame for frame in range(frames[0], frames[-1] + 1)
            if frame not in by_frame
        ] if frames else [],
        "duplicateBindingFrames": duplicate_binding_frames,
        "repeatedWholeFrameSignatureCount": sum(
            len(signature_frames) - 1
            for signature_frames in frame_signatures.values()
            if len(signature_frames) > 1
        ),
        "bindings": {
            str(binding): summarize_binding(binding_records)
            for binding, binding_records in sorted(by_binding.items())
        },
    }

    output = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
