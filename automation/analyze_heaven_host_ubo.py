#!/usr/bin/env python3
"""Join Heaven DXVK UBO hashes to the Host private upload for each submit."""

import argparse
import collections
import json
import re
from pathlib import Path

from analyze_heaven_ubo import parse_record


TIMESTAMP_RE = re.compile(r"\[(\d+)\]")
FIELD_RE = re.compile(r"([A-Za-z][A-Za-z0-9]*)=([^\s]+)")
WINE_MAP_RE = re.compile(
    r"WineHuaWineFrameAssoc: unixPid=(\d+) command-buffer "
    r"clientCmd=(0x[0-9a-f]+) guestCmd=(0x[0-9a-f]+)"
)
GUEST_SUBMIT_RE = re.compile(
    r"WineHuaGuestFrameAssoc: unixPid=(\d+) queue-submit "
    r"guestCmd=(0x[0-9a-f]+) cmdId=(\d+)"
)
DXVK_SUBMIT_RE = re.compile(
    r"WineHuaDxvkSubmit: winPid=(\d+) recording=(\d+) "
    r"execCmd=(0x[0-9a-f]+) frameCount=(\d+) frames=\[([^]]*)\]"
)
def integer(value):
    return int(value, 16) if value.startswith("0x") else int(value)


def parse_fields(line):
    return {key: value.rstrip(",") for key, value in FIELD_RE.findall(line)}


def parse_wine_identity(path):
    client_to_guest = {}
    pending = collections.defaultdict(collections.deque)
    guest_submits_by_pid = collections.defaultdict(list)
    frame_guest_index = {}
    unjoined_dxvk = []

    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            match = WINE_MAP_RE.search(line)
            if match:
                pid, client, guest = match.groups()
                client_to_guest[client] = (int(pid), guest)
                continue

            match = DXVK_SUBMIT_RE.search(line)
            if match:
                _, recording, client, _, frames_text = match.groups()
                mapped = client_to_guest.get(client)
                frames = [int(value) for value in frames_text.split(",") if value]
                if mapped:
                    pid, guest = mapped
                    pending[(pid, guest)].append((int(recording), frames, client))
                else:
                    unjoined_dxvk.append({
                        "recording": int(recording), "frames": frames,
                        "clientCmd": client, "reason": "missing-client-map",
                    })
                continue

            match = GUEST_SUBMIT_RE.search(line)
            if not match:
                continue
            pid_text, guest, cmd_id_text = match.groups()
            pid = int(pid_text)
            entry = {"guestCmd": guest, "cmdId": int(cmd_id_text)}
            index = len(guest_submits_by_pid[pid])
            guest_submits_by_pid[pid].append(entry)
            queue = pending[(pid, guest)]
            if queue:
                recording, frames, client = queue.popleft()
                for frame in frames:
                    frame_guest_index[frame] = {
                        "recording": recording,
                        "clientCmd": client,
                        "guestCmd": guest,
                        "cmdId": entry["cmdId"],
                        "guestIndex": index,
                        "unixPid": pid,
                    }

    selected_pid = max(guest_submits_by_pid, key=lambda pid: len(guest_submits_by_pid[pid]))
    return {
        "selectedPid": selected_pid,
        "guestSubmits": guest_submits_by_pid[selected_pid],
        "frameGuestIndex": {
            frame: value for frame, value in frame_guest_index.items()
            if value["unixPid"] == selected_pid
        },
        "unjoinedDxvk": unjoined_dxvk,
    }


def parse_host_submits(path):
    sessions = collections.defaultdict(list)
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            if "WineHuaFrameAssoc: queue-submit" not in line:
                continue
            fields = parse_fields(line)
            if not {"ctx", "submit", "cmdId"}.issubset(fields):
                continue
            timestamp = int(TIMESTAMP_RE.search(line).group(1))
            sessions[int(fields["ctx"])].append({
                "timestamp": timestamp,
                "submit": int(fields["submit"]),
                "cmdId": int(fields["cmdId"]),
            })
    selected_ctx = max(sessions, key=lambda ctx: max(item["submit"] for item in sessions[ctx]))
    return selected_ctx, sessions[selected_ctx]


def parse_host_binds(path):
    binds = collections.defaultdict(list)
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for order, line in enumerate(source):
            if "WineHuaCapture: bind-descriptor" not in line:
                continue
            fields = parse_fields(line)
            timestamp_match = TIMESTAMP_RE.search(line)
            if not timestamp_match or not {"cmdId", "setId"}.issubset(fields):
                continue
            cmd_id = integer(fields["cmdId"])
            binds[cmd_id].append({
                "timestamp": int(timestamp_match.group(1)),
                "order": order,
                "setId": integer(fields["setId"]),
                "firstSet": integer(fields.get("firstSet", "0")),
                "setIndex": integer(fields.get("setIndex", "0")),
            })
    return binds


def align_submit_sequences(guest, host):
    mapping = {}
    guest_index = 0
    host_index = 0
    mismatches = []
    while guest_index < len(guest) and host_index < len(host):
        if guest[guest_index]["cmdId"] == host[host_index]["cmdId"]:
            mapping[guest_index] = host[host_index]
            guest_index += 1
            host_index += 1
            continue

        guest_id = guest[guest_index]["cmdId"]
        host_id = host[host_index]["cmdId"]
        next_host = next((offset for offset in range(1, 17)
                          if host_index + offset < len(host)
                          and host[host_index + offset]["cmdId"] == guest_id), None)
        next_guest = next((offset for offset in range(1, 17)
                           if guest_index + offset < len(guest)
                           and guest[guest_index + offset]["cmdId"] == host_id), None)
        mismatches.append({
            "guestIndex": guest_index, "hostIndex": host_index,
            "guestCmdId": guest_id, "hostCmdId": host_id,
            "nextHost": next_host, "nextGuest": next_guest,
        })
        if next_host is not None and (next_guest is None or next_host <= next_guest):
            host_index += next_host
        elif next_guest is not None:
            guest_index += next_guest
        else:
            guest_index += 1
            host_index += 1
    return mapping, mismatches


def parse_host_ubo(path, minimum_timestamp):
    phases = collections.defaultdict(list)
    limits = []
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            timestamp_match = TIMESTAMP_RE.search(line)
            if not timestamp_match:
                continue
            timestamp = int(timestamp_match.group(1))
            if timestamp < minimum_timestamp:
                continue
            fields = parse_fields(line)
            phase = fields.get("phase")
            if not phase:
                continue
            if "trace limit reached" in line:
                limits.append({"phase": phase, "timestamp": timestamp})
                continue
            fields["timestamp"] = timestamp
            for key in (
                "submit", "submitGeneration", "binding", "bufferId",
                "memoryId", "absoluteOffset", "bytes", "hashBytes",
                "descriptorRange", "descriptorOffset", "bufferMemoryOffset",
                "descriptorSequence", "setId", "arrayElement", "type",
                "updateOffset", "updateBytes", "capacity",
            ):
                if key in fields:
                    fields[key] = integer(fields[key])
            phases[phase].append(fields)
    return phases, limits


def parse_wine_ubo(path):
    result = collections.defaultdict(dict)
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line in source:
            record = parse_record(line)
            if record and record["binding"] in (3, 4):
                result[record["frame"]][record["binding"]] = record
    return result


def covers(record, offset, size):
    return (record["absoluteOffset"] <= offset and
            record["absoluteOffset"] + record["bytes"] >= offset + size)


def build_host_indices(phases):
    descriptors = collections.defaultdict(list)
    flushes = collections.defaultdict(list)
    ranges = collections.defaultdict(list)
    updates = collections.defaultdict(list)
    for item in phases["descriptor"]:
        descriptors[(item.get("binding"), item.get("shadowHash"),
                     item.get("hashBytes"))].append(item)
    for item in phases["flush"]:
        flushes[(item.get("memoryId"), item.get("absoluteOffset"),
                 item.get("bytes"))].append(item)
    for item in phases["upload-range"]:
        ranges[item.get("bufferId")].append(item)
    for item in phases["update"]:
        updates[item.get("bufferId")].append(item)
    return descriptors, flushes, ranges, updates


def build_focused_indices(phases):
    descriptors = collections.defaultdict(list)
    updates = collections.defaultdict(list)
    for item in phases["watched-descriptor"]:
        descriptors[(item.get("setId"), item.get("binding"))].append(item)
    for item in phases["watched-update"]:
        updates[(item.get("bufferId"), item.get("absoluteOffset"),
                 item.get("bytes"))].append(item)
    for items in descriptors.values():
        items.sort(key=lambda item: (
            item.get("submitGeneration", 0),
            item.get("descriptorSequence", 0), item["timestamp"]))
    for items in updates.values():
        items.sort(key=lambda item: (item.get("submit", 0), item["timestamp"]))
    return descriptors, updates


def previous_submit_timestamps(host_submits):
    previous = {}
    last_by_command = {}
    for item in host_submits:
        previous[id(item)] = last_by_command.get(item["cmdId"])
        last_by_command[item["cmdId"]] = item["timestamp"]
    return previous


def bound_sets_for_submit(bind_events, host_item, previous_timestamp):
    events = [item for item in bind_events.get(host_item["cmdId"], [])
              if item["timestamp"] <= host_item["timestamp"] and
              (previous_timestamp is None or
               item["timestamp"] > previous_timestamp)]
    if not events:
        prior = [item for item in bind_events.get(host_item["cmdId"], [])
                 if item["timestamp"] <= host_item["timestamp"]]
        if prior:
            latest_order = prior[-1]["order"]
            events = [item for item in prior
                      if item["order"] >= latest_order - 16]
    result = []
    seen = set()
    for item in reversed(events):
        if item["setId"] not in seen:
            seen.add(item["setId"])
            result.append(item["setId"])
    return result


def join_focused_binding(record, host_item, previous_timestamp, bind_events,
                         focused_descriptors, focused_updates):
    expected_hash = record["hash"][2:]
    size = record["bytes"]
    set_ids = bound_sets_for_submit(bind_events, host_item, previous_timestamp)
    candidates = []
    for set_id in set_ids:
        descriptor_events = [item for item in focused_descriptors.get(
            (set_id, record["binding"]), [])
            if item.get("submitGeneration", 0) <= host_item["submit"]]
        if not descriptor_events:
            continue
        descriptor = descriptor_events[-1]
        update_events = [item for item in focused_updates.get((
            descriptor.get("bufferId"), descriptor.get("absoluteOffset"),
            size), []) if item.get("submit", 0) <= host_item["submit"]]
        latest_update = update_events[-1] if update_events else None
        hash_match = bool(
            latest_update and latest_update.get("sourceHash") == expected_hash)
        candidates.append({
            "setId": set_id,
            "descriptor": descriptor,
            "latestUpdate": latest_update,
            "latestUpdateHashMatch": hash_match,
            "latestUpdateHashMismatch": bool(latest_update and not hash_match),
        })
    candidates.sort(key=lambda item: (
        item["latestUpdateHashMatch"], bool(item["latestUpdate"]),
        item["descriptor"].get("submitGeneration", 0),
    ), reverse=True)
    return {
        "commandId": host_item["cmdId"],
        "boundSetCount": len(set_ids),
        "boundSetIdsFirst64": set_ids[:64],
        "candidateCount": len(candidates),
        "best": candidates[0] if candidates else None,
    }


def join_binding(record, host_submit, descriptors, flushes, ranges, updates):
    expected_hash = record["hash"][2:]
    size = record["bytes"]
    candidates = [item for item in descriptors.get(
                      (record["binding"], expected_hash, size), [])
                  if item.get("submitGeneration", 0) <= host_submit]
    best = None
    for descriptor in candidates:
        offset = descriptor["absoluteOffset"]
        buffer_id = descriptor["bufferId"]
        memory_id = descriptor["memoryId"]
        matching_ranges = [item for item in ranges.get(buffer_id, [])
                           if item.get("submit", 0) <= host_submit
                           and covers(item, offset, size)]
        matching_updates = [item for item in updates.get(buffer_id, [])
                            if item.get("submit", 0) <= host_submit
                            and covers(item, offset, size)]
        matching_flushes = [item for item in flushes.get(
                                (memory_id, offset, size), [])
                            if item.get("submitGeneration", 0) < host_submit]
        latest_range = max(matching_ranges, key=lambda item: item["submit"],
                           default=None)
        latest_update = max(matching_updates, key=lambda item: item["submit"],
                            default=None)
        latest_flush = max(
            matching_flushes, key=lambda item: item["submitGeneration"],
            default=None)
        exact_range_hash_match = bool(
            latest_range and latest_range["absoluteOffset"] == offset
            and latest_range["bytes"] == size
            and latest_range.get("sourceHash") == expected_hash)
        exact_update_hash_match = bool(
            latest_update and latest_update["absoluteOffset"] == offset
            and latest_update["bytes"] == size
            and latest_update.get("sourceHash") == expected_hash)
        exact_range_hash_mismatch = bool(
            latest_range and latest_range["absoluteOffset"] == offset
            and latest_range["bytes"] == size
            and latest_range.get("sourceHash") != expected_hash)
        exact_update_hash_mismatch = bool(
            latest_update and latest_update["absoluteOffset"] == offset
            and latest_update["bytes"] == size
            and latest_update.get("sourceHash") != expected_hash)
        flush_hash_match = bool(
            latest_flush and latest_flush.get("sourceHash") == expected_hash)
        score = (
            bool(latest_range) + bool(latest_update) + flush_hash_match
            + 2 * exact_range_hash_match + 2 * exact_update_hash_match
            - 2 * exact_range_hash_mismatch - 2 * exact_update_hash_mismatch
        )
        joined = {
            "score": score,
            "descriptor": descriptor,
            "flushCount": len(matching_flushes),
            "latestFlushHashMatch": flush_hash_match,
            "rangeCoverage": bool(latest_range),
            "rangeSubmit": latest_range.get("submit") if latest_range else None,
            "updateCoverage": bool(latest_update),
            "updateSubmit": latest_update.get("submit") if latest_update else None,
            "exactRangeHashMatch": exact_range_hash_match,
            "exactRangeHashMismatch": exact_range_hash_mismatch,
            "exactUpdateHashMatch": exact_update_hash_match,
            "exactUpdateHashMismatch": exact_update_hash_mismatch,
        }
        if best is None or joined["score"] > best["score"]:
            best = joined
    return best, len(candidates)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wine-ubo", type=Path, required=True)
    parser.add_argument("--wine-identity", type=Path, required=True)
    parser.add_argument("--host-assoc", type=Path, required=True)
    parser.add_argument("--host-ubo", type=Path, required=True)
    parser.add_argument("--host-bind", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    identity = parse_wine_identity(args.wine_identity)
    selected_ctx, host_submits = parse_host_submits(args.host_assoc)
    bind_events = parse_host_binds(args.host_bind or args.host_ubo)
    alignment, alignment_mismatches = align_submit_sequences(
        identity["guestSubmits"], host_submits)
    frame_host_submit = {}
    frame_host_item = {}
    for frame, item in identity["frameGuestIndex"].items():
        host = alignment.get(item["guestIndex"])
        if host:
            frame_host_submit[frame] = host["submit"]
            frame_host_item[frame] = host

    phases, limits = parse_host_ubo(args.host_ubo, host_submits[0]["timestamp"])
    host_indices = build_host_indices(phases)
    focused_indices = build_focused_indices(phases)
    previous_timestamps = previous_submit_timestamps(host_submits)
    wine_ubo = parse_wine_ubo(args.wine_ubo)
    results = {3: [], 4: []}
    focused_results = {3: [], 4: []}
    failures = []
    for frame, bindings in sorted(wine_ubo.items()):
        host_submit = frame_host_submit.get(frame)
        if host_submit is None:
            continue
        for binding in (3, 4):
            record = bindings.get(binding)
            if not record:
                continue
            joined, candidate_count = join_binding(
                record, host_submit, *host_indices)
            item = {
                "frame": frame, "hostSubmit": host_submit,
                "hash": record["hash"], "candidateCount": candidate_count,
                "joined": joined,
            }
            results[binding].append(item)
            if joined and ((joined["rangeCoverage"] and not joined["updateCoverage"])
                           or joined["exactRangeHashMismatch"]
                           or joined["exactUpdateHashMismatch"]):
                failures.append({"binding": binding, **item})

            host_item = frame_host_item[frame]
            focused = join_focused_binding(
                record, host_item, previous_timestamps[id(host_item)],
                bind_events, *focused_indices)
            focused_item = {
                "frame": frame, "hostSubmit": host_submit,
                "hash": record["hash"], "joined": focused,
            }
            focused_results[binding].append(focused_item)

    binding_summary = {}
    for binding, items in results.items():
        joined = [item["joined"] for item in items if item["joined"]]
        binding_summary[str(binding)] = {
            "framesWithHostSubmit": len(items),
            "framesWithDescriptorCandidate": len(joined),
            "framesWithFlush": sum(item["flushCount"] > 0 for item in joined),
            "framesWithUploadRangeCoverage": sum(item["rangeCoverage"] for item in joined),
            "framesWithUpdateCoverage": sum(item["updateCoverage"] for item in joined),
            "framesWithExactRangeHash": sum(item["exactRangeHashMatch"] for item in joined),
            "framesWithExactUpdateHash": sum(item["exactUpdateHashMatch"] for item in joined),
            "framesWithExactRangeHashMismatch": sum(item["exactRangeHashMismatch"] for item in joined),
            "framesWithExactUpdateHashMismatch": sum(item["exactUpdateHashMismatch"] for item in joined),
            "joinSamplesFirst10": items[:10],
        }

    focused_summary = {}
    for binding, items in focused_results.items():
        joins = [item["joined"] for item in items]
        best = [item["best"] for item in joins if item["best"]]
        focused_summary[str(binding)] = {
            "framesWithHostSubmit": len(items),
            "framesWithBoundSet": sum(bool(item["boundSetCount"])
                                      for item in joins),
            "framesWithDescriptorCandidate": len(best),
            "framesWithWatchedUpdate": sum(bool(item["latestUpdate"])
                                           for item in best),
            "framesWithExpectedLastUpdateHash": sum(
                item["latestUpdateHashMatch"] for item in best),
            "framesWithStaleLastUpdateHash": sum(
                item["latestUpdateHashMismatch"] for item in best),
            "joinSamplesFirst10": items[:10],
        }

    report = {
        "schemaVersion": 1,
        "selectedUnixPid": identity["selectedPid"],
        "selectedHostContext": selected_ctx,
        "guestSubmitCount": len(identity["guestSubmits"]),
        "hostSubmitCommandCount": len(host_submits),
        "alignedSubmitCommandCount": len(alignment),
        "alignmentMismatchCount": len(alignment_mismatches),
        "alignmentMismatchesFirst20": alignment_mismatches[:20],
        "frameToHostSubmitCount": len(frame_host_submit),
        "hostTraceCounts": {key: len(value) for key, value in phases.items()},
        "hostBindCommandBufferCount": len(bind_events),
        "hostBindEventCount": sum(len(items) for items in bind_events.values()),
        "traceLimits": limits,
        "bindings": binding_summary,
        "focusedBindings": focused_summary,
        "suspiciousJoinCount": len(failures),
        "suspiciousJoinsFirst20": failures[:20],
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
