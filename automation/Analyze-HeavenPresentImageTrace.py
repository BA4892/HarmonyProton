#!/usr/bin/env python3
"""Correlate Heaven private-present image identity across all layers.

Matching handles and serials prove that the requested present image reached
the NCP in order. They do not prove that DXVK wrote the expected pixels into
that image before it was presented.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any, Callable


KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
DXVK_RE = re.compile(r"WineHuaPresentImage: layer=dxvk event=(image-map|acquire|present)")
WINE_RE = re.compile(r"WineHuaPresentImage: layer=wine event=(image-map|acquire|present)")
GUEST_RE = re.compile(r"WineHuaPresentImage: layer=guest event=present")
HOST_RE = re.compile(r"WineHuaPresentImage: layer=host event=present")
NCP_RE = re.compile(r"vk_present count=")
ORDER_RE = re.compile(r"\[VENUS-ORDER\]\[NCP\]")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxvk", required=True, type=Path)
    parser.add_argument("--wine", required=True, type=Path)
    parser.add_argument("--host", required=True, type=Path)
    parser.add_argument("--hilog", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def key_values(line: str) -> dict[str, str]:
    return {key: value.rstrip(",") for key, value in KV_RE.findall(line)}


def number(values: dict[str, str], key: str, default: int = -1) -> int:
    value = values.get(key)
    if value is None:
        return default
    try:
        return int(value, 0)
    except ValueError:
        return default


def handle(values: dict[str, str], key: str) -> str:
    return values.get(key, "").lower()


def log_encoding(path: Path) -> str:
    with path.open("rb") as stream:
        prefix = stream.read(4)
    if prefix.startswith((b"\xff\xfe", b"\xfe\xff")):
        return "utf-16"
    return "utf-8"


def new_image_session() -> dict[str, Any]:
    return {
        "maps": {},
        "acquires": {},
        "presents": {},
        "eventSerials": [],
        "lineRange": [0, 0],
    }


def parse_image_sessions(
    path: Path, pattern: re.Pattern[str], serial_key: str
) -> list[dict[str, Any]]:
    sessions: list[dict[str, Any]] = []
    current = new_image_session()

    with path.open(
        "r", encoding=log_encoding(path), errors="ignore"
    ) as stream:
        for line_number, line in enumerate(stream, 1):
            match = pattern.search(line)
            if not match:
                continue
            event = match.group(1)
            values = key_values(line)
            index = number(values, "index")

            if event == "image-map" and index == 0 and current["maps"]:
                current["lineRange"][1] = line_number - 1
                sessions.append(current)
                current = new_image_session()

            if current["lineRange"][0] == 0:
                current["lineRange"][0] = line_number
            current["lineRange"][1] = line_number

            record = {
                "line": line_number,
                "index": index,
                "image": handle(values, "image"),
                "swapchain": handle(values, "swapchain"),
            }
            if event == "image-map":
                current["maps"][index] = record
                continue

            serial = number(values, serial_key)
            if serial < 0:
                continue
            record["serial"] = serial
            record[serial_key] = serial
            record["status"] = values.get("status", "")
            record["result"] = number(values, "result")
            current[f"{event}s"][serial] = record
            if event == "present":
                current["eventSerials"].append(serial)

    if current["maps"] or current["acquires"] or current["presents"]:
        sessions.append(current)
    return sessions


def split_serial_sessions(
    records: list[dict[str, Any]], context_key: str | None = None
) -> list[list[dict[str, Any]]]:
    sessions: list[list[dict[str, Any]]] = []
    current: list[dict[str, Any]] = []
    previous_serial: int | None = None
    previous_context: int | None = None

    for record in records:
        serial = int(record["serial"])
        context = int(record.get(context_key, -1)) if context_key else None
        reset = previous_serial is not None and serial < previous_serial
        if context_key and previous_context is not None and context != previous_context:
            reset = True
        if reset and current:
            sessions.append(current)
            current = []
        current.append(record)
        previous_serial = serial
        previous_context = context

    if current:
        sessions.append(current)
    return sessions


def read_matching(
    path: Path,
    pattern: re.Pattern[str],
    parser: Callable[[dict[str, str], int], dict[str, Any]],
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with path.open(
        "r", encoding=log_encoding(path), errors="ignore"
    ) as stream:
        for line_number, line in enumerate(stream, 1):
            if not pattern.search(line):
                continue
            record = parser(key_values(line), line_number)
            if int(record.get("serial", -1)) >= 0:
                records.append(record)
    return records


def parse_guest(values: dict[str, str], line: int) -> dict[str, Any]:
    return {
        "line": line,
        "serial": number(values, "serial"),
        "rawImage": handle(values, "rawImage"),
        "imageId": number(values, "imageId"),
        "queueId": number(values, "queueId"),
    }


def parse_host(values: dict[str, str], line: int) -> dict[str, Any]:
    return {
        "line": line,
        "serial": number(values, "serial"),
        "ctx": number(values, "ctx"),
        "imageId": number(values, "imageId"),
        "hostImage": handle(values, "hostImage"),
        "queueId": number(values, "queueId"),
    }


def parse_ncp(values: dict[str, str], line: int) -> dict[str, Any]:
    return {
        "line": line,
        "serial": number(values, "serial"),
        "ctx": number(values, "ctx"),
        "pid": number(values, "pid"),
        "imageId": number(values, "image_id"),
        "ret": number(values, "ret"),
        "count": number(values, "count"),
    }


def parse_order(values: dict[str, str], line: int) -> dict[str, Any]:
    return {
        "line": line,
        "serial": number(values, "serial"),
        "frame": number(values, "frame"),
        "source": handle(values, "source"),
        "targetIndex": number(values, "target_index"),
        "target": handle(values, "target"),
        "timestamp": number(values, "timestamp"),
        "reportedRegression": number(values, "serial_regress"),
    }


def unique_by_serial(
    records: list[dict[str, Any]],
) -> tuple[dict[int, dict[str, Any]], int, list[dict[str, Any]]]:
    unique: dict[int, dict[str, Any]] = {}
    retries = 0
    conflicts: list[dict[str, Any]] = []
    for record in records:
        serial = int(record["serial"])
        previous = unique.get(serial)
        if previous is None:
            unique[serial] = record
            continue
        retries += 1
        identity_keys = ("imageId", "hostImage", "rawImage", "source")
        differences = {
            key: [previous.get(key), record.get(key)]
            for key in identity_keys
            if key in previous
            and key in record
            and previous.get(key) != record.get(key)
        }
        if differences:
            conflicts.append({"serial": serial, "differences": differences})
    return unique, retries, conflicts


def serial_regressions(records: list[dict[str, Any]]) -> list[dict[str, int]]:
    result: list[dict[str, int]] = []
    previous: int | None = None
    for record in records:
        current = int(record["serial"])
        if previous is not None and current < previous:
            result.append(
                {
                    "previous": previous,
                    "current": current,
                    "line": int(record["line"]),
                }
            )
        previous = current
    return result


def select_image_session(
    sessions: list[dict[str, Any]],
) -> tuple[int, dict[str, Any]]:
    if not sessions:
        raise RuntimeError("no image sessions found")
    index = max(
        range(len(sessions)),
        key=lambda item: (len(sessions[item]["presents"]), item),
    )
    return index, sessions[index]


def select_serial_session(
    sessions: list[list[dict[str, Any]]],
    score: Callable[[dict[int, dict[str, Any]]], int],
) -> tuple[
    int,
    list[dict[str, Any]],
    dict[int, dict[str, Any]],
    int,
    list[dict[str, Any]],
]:
    if not sessions:
        raise RuntimeError("no serial sessions found")
    candidates = []
    for index, session in enumerate(sessions):
        unique, retries, conflicts = unique_by_serial(session)
        candidates.append(
            (score(unique), len(unique), index, session, unique, retries, conflicts)
        )
    _, _, index, session, unique, retries, conflicts = max(
        candidates, key=lambda item: item[:3]
    )
    return index, session, unique, retries, conflicts


def mismatch(kind: str, serial: int, **values: Any) -> dict[str, Any]:
    return {"kind": kind, "serial": serial, **values}


def analyze(args: argparse.Namespace) -> dict[str, Any]:
    dxvk_sessions = parse_image_sessions(args.dxvk, DXVK_RE, "sequence")
    dxvk_index, dxvk = select_image_session(dxvk_sessions)

    wine_sessions = parse_image_sessions(args.wine, WINE_RE, "serial")
    wine_index, wine = max(
        enumerate(wine_sessions),
        key=lambda item: (
            sum(
                1
                for serial, record in item[1]["presents"].items()
                if serial in dxvk["presents"]
                and record["image"] == dxvk["presents"][serial]["image"]
                and record["index"] == dxvk["presents"][serial]["index"]
            ),
            len(item[1]["presents"]),
            item[0],
        ),
    )

    guest_records = read_matching(args.wine, GUEST_RE, parse_guest)
    guest_sessions = split_serial_sessions(guest_records)
    guest_index, guest_events, guest, guest_retries, guest_conflicts = (
        select_serial_session(
            guest_sessions,
            lambda records: sum(
                1
                for serial, record in records.items()
                if serial in wine["presents"]
                and record["rawImage"] == wine["presents"][serial]["image"]
            ),
        )
    )

    host_records = read_matching(args.host, HOST_RE, parse_host)
    host_sessions = split_serial_sessions(host_records, "ctx")
    host_index, host_events, host, host_retries, host_conflicts = (
        select_serial_session(
            host_sessions,
            lambda records: sum(
                1
                for serial, record in records.items()
                if serial in guest
                and record["imageId"] == guest[serial]["imageId"]
            ),
        )
    )

    order_records = read_matching(args.hilog, ORDER_RE, parse_order)
    order_sessions = split_serial_sessions(order_records)
    order_index, order_events, order, order_retries, order_conflicts = (
        select_serial_session(
            order_sessions,
            lambda records: sum(
                1
                for serial, record in records.items()
                if serial in host
                and record["source"] == host[serial]["hostImage"]
            ),
        )
    )

    ncp_records = read_matching(args.host, NCP_RE, parse_ncp)
    ncp_groups: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for record in ncp_records:
        ncp_groups.setdefault(
            (int(record["pid"]), int(record["ctx"])), []
        ).append(record)
    ncp_key, ncp_events = max(
        ncp_groups.items(),
        key=lambda item: (
            sum(
                1
                for record in item[1]
                if record["serial"] in host
                and record["imageId"] == host[record["serial"]]["imageId"]
            ),
            len(item[1]),
        ),
    )
    ncp, ncp_retries, ncp_conflicts = unique_by_serial(ncp_events)

    problems: list[dict[str, Any]] = []
    for serial in sorted(set(dxvk["acquires"]).intersection(dxvk["presents"])):
        acquire = dxvk["acquires"][serial]
        present = dxvk["presents"][serial]
        if (acquire["index"], acquire["image"]) != (
            present["index"],
            present["image"],
        ):
            problems.append(
                mismatch(
                    "dxvk-acquire-present",
                    serial,
                    acquire=acquire,
                    present=present,
                )
            )

    joined_serials = sorted(
        set(dxvk["presents"])
        .intersection(wine["presents"])
        .intersection(guest)
        .intersection(host)
    )
    for serial in joined_serials:
        dxvk_record = dxvk["presents"][serial]
        wine_record = wine["presents"][serial]
        guest_record = guest[serial]
        host_record = host[serial]
        if (dxvk_record["index"], dxvk_record["image"]) != (
            wine_record["index"],
            wine_record["image"],
        ):
            problems.append(
                mismatch(
                    "dxvk-wine",
                    serial,
                    dxvk=dxvk_record,
                    wine=wine_record,
                )
            )
        if wine_record["image"] != guest_record["rawImage"]:
            problems.append(
                mismatch(
                    "wine-guest",
                    serial,
                    wineImage=wine_record["image"],
                    guestRawImage=guest_record["rawImage"],
                )
            )
        if guest_record["imageId"] != host_record["imageId"]:
            problems.append(
                mismatch(
                    "guest-host",
                    serial,
                    guestImageId=guest_record["imageId"],
                    hostImageId=host_record["imageId"],
                )
            )

    order_joined = sorted(set(order).intersection(host))
    for serial in order_joined:
        if order[serial]["source"] != host[serial]["hostImage"]:
            problems.append(
                mismatch(
                    "host-ncp-source",
                    serial,
                    hostImage=host[serial]["hostImage"],
                    source=order[serial]["source"],
                )
            )

    target_index_counts: dict[int, int] = {}
    target_sequence: list[int] = []
    invalid_targets: list[dict[str, Any]] = []
    timestamp_regressions: list[dict[str, int]] = []
    previous_timestamp: int | None = None
    for serial in sorted(order):
        record = order[serial]
        target_index = int(record["targetIndex"])
        target_sequence.append(target_index)
        target_index_counts[target_index] = (
            target_index_counts.get(target_index, 0) + 1
        )
        if target_index < 0 or not record["target"]:
            invalid_targets.append(
                {
                    "serial": serial,
                    "targetIndex": target_index,
                    "target": record["target"],
                }
            )
        timestamp = int(record["timestamp"])
        if previous_timestamp is not None and timestamp <= previous_timestamp:
            timestamp_regressions.append(
                {
                    "serial": serial,
                    "previous": previous_timestamp,
                    "current": timestamp,
                }
            )
        previous_timestamp = timestamp

    all_conflicts = (
        guest_conflicts + host_conflicts + order_conflicts + ncp_conflicts
    )
    regressions = {
        "dxvk": serial_regressions(
            [
                dxvk["presents"][key]
                for key in dxvk["eventSerials"]
                if key in dxvk["presents"]
            ]
        ),
        "wine": serial_regressions(
            [
                wine["presents"][key]
                for key in wine["eventSerials"]
                if key in wine["presents"]
            ]
        ),
        "guest": serial_regressions(guest_events),
        "host": serial_regressions(host_events),
        "ncpOrder": serial_regressions(order_events),
    }
    regression_count = sum(len(records) for records in regressions.values())
    pass_identity = (
        bool(joined_serials)
        and not problems
        and not all_conflicts
        and regression_count == 0
    )
    pass_publish = (
        pass_identity
        and bool(order_joined)
        and not invalid_targets
        and not timestamp_regressions
    )

    return {
        "schemaVersion": 1,
        "sources": {
            "dxvk": str(args.dxvk),
            "wine": str(args.wine),
            "host": str(args.host),
            "hilog": str(args.hilog),
        },
        "selectedSessions": {
            "dxvk": {
                "index": dxvk_index,
                "count": len(dxvk_sessions),
                "lineRange": dxvk["lineRange"],
            },
            "wine": {
                "index": wine_index,
                "count": len(wine_sessions),
                "lineRange": wine["lineRange"],
            },
            "guest": {"index": guest_index, "count": len(guest_sessions)},
            "host": {
                "index": host_index,
                "count": len(host_sessions),
                "ctx": next(iter(host.values()))["ctx"],
            },
            "ncp": {"pid": ncp_key[0], "ctx": ncp_key[1]},
            "ncpOrder": {
                "index": order_index,
                "count": len(order_sessions),
            },
        },
        "counts": {
            "dxvkPresent": len(dxvk["presents"]),
            "winePresent": len(wine["presents"]),
            "guestPresent": len(guest),
            "hostPresentUnique": len(host),
            "hostRetryAttempts": host_retries,
            "ncpSampledPresent": len(ncp),
            "ncpRetryAttempts": ncp_retries,
            "ncpOrder": len(order),
            "joinedThroughHost": len(joined_serials),
            "joinedThroughNcpOrder": len(order_joined),
        },
        "serialRange": (
            [joined_serials[0], joined_serials[-1]] if joined_serials else []
        ),
        "imageMaps": {
            "dxvk": {
                str(key): value["image"]
                for key, value in sorted(dxvk["maps"].items())
            },
            "wine": {
                str(key): value["image"]
                for key, value in sorted(wine["maps"].items())
            },
            "guestToHost": {
                str(image_id): sorted(
                    {
                        record["hostImage"]
                        for serial, record in host.items()
                        if image_id == guest.get(serial, {}).get("imageId")
                    }
                )
                for image_id in sorted(
                    {record["imageId"] for record in guest.values()}
                )
            },
        },
        "targetAcquisition": {
            "indexCounts": {
                str(key): value
                for key, value in sorted(target_index_counts.items())
            },
            "sequenceStart": target_sequence[:30],
            "sequenceEnd": target_sequence[-30:],
            "note": (
                "The acquire order is driver-controlled and is not required "
                "to follow a fixed modulo cycle."
            ),
        },
        "retryIdentityConflicts": all_conflicts[:100],
        "serialRegressions": regressions,
        "invalidTargets": invalid_targets[:100],
        "timestampRegressions": timestamp_regressions[:100],
        "identityMismatches": problems[:100],
        "verdict": (
            "EXACT-THROUGH-NCP-PUBLISH"
            if pass_publish
            else "EXACT-THROUGH-HOST-PRESENT"
            if pass_identity
            else "IDENTITY-OR-ORDER-MISMATCH"
        ),
        "scope": (
            "Identity and order only. A PASS does not prove that the acquired "
            "DXVK presenter image contains the newest completed render content."
        ),
    }


def main() -> int:
    args = parse_args()
    result = analyze(args)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if result["verdict"] != "IDENTITY-OR-ORDER-MISMATCH" else 2


if __name__ == "__main__":
    raise SystemExit(main())
