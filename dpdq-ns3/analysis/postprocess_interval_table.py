#!/usr/bin/env python3
"""Aggregate OFF/PAUSE interval traces by (switch_id, port_id) pair."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence


SPINE_SWITCH_IDS = {0, 1, 2, 3}
LEAF_SWITCH_IDS = set(range(4, 16))
RECEIVER_LEAF_ID = 10
SOURCE_LEAF_IDS = LEAF_SWITCH_IDS - {RECEIVER_LEAF_ID}
LEAF_TO_SPINE_PORT_IDS = {1, 2, 3, 4}
SPINE_TO_RECEIVER_LEAF_PORT_ID = 7
RECEIVER_HOST_DOWNLINK_PORT_ID = 5


@dataclass
class IntervalRecord:
    switch_id: int
    port_id: int
    duration_ns: int


@dataclass
class SwitchPortSummary:
    switch_id: int
    port_id: int
    path_role: str
    event_count: int
    total_duration_ns: int
    avg_duration_ns: float
    max_duration_ns: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Post-process xoff/pause interval files into CSV summaries grouped by each (switch_id, port_id) pair."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="One or more interval files to summarize.",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Directory for generated CSV files. Defaults to each input file's parent.",
    )
    parser.add_argument(
        "--sort-by",
        choices=("switch", "total", "count", "max"),
        default="total",
        help="Row ordering in the output CSV.",
    )
    return parser.parse_args()


def detect_duration_column(header: Sequence[str]) -> int:
    for idx, name in enumerate(header):
        if name == "duration_ns":
            return idx
    raise ValueError("Could not find duration_ns column in header.")


def detect_switch_column(header: Sequence[str]) -> int:
    for idx, name in enumerate(header):
        if name == "switch_id":
            return idx
    raise ValueError("Could not find switch_id column in header.")


def detect_port_column(header: Sequence[str]) -> int:
    for candidate in ("out_dev", "ifindex", "port_id", "port"):
        for idx, name in enumerate(header):
            if name == candidate:
                return idx
    raise ValueError("Could not find port column in header.")


def load_records(path: Path) -> List[IntervalRecord]:
    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    if not lines:
        return []

    header = lines[0].split()
    switch_idx = detect_switch_column(header)
    port_idx = detect_port_column(header)
    duration_idx = detect_duration_column(header)

    records: List[IntervalRecord] = []
    for raw in lines[1:]:
        line = raw.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) <= max(switch_idx, port_idx, duration_idx):
            continue
        try:
            records.append(
                IntervalRecord(
                    switch_id=int(parts[switch_idx]),
                    port_id=int(parts[port_idx]),
                    duration_ns=int(parts[duration_idx]),
                )
            )
        except ValueError:
            continue
    return records


def classify_path_role(switch_id: int, port_id: int) -> str:
    """Classify a switch-port pair for the fixed topology96 44->1 incast."""
    if switch_id in SOURCE_LEAF_IDS and port_id in LEAF_TO_SPINE_PORT_IDS:
        return "leaf->spine"
    if switch_id in SPINE_SWITCH_IDS and port_id == SPINE_TO_RECEIVER_LEAF_PORT_ID:
        return "spine->leaf"
    if switch_id == RECEIVER_LEAF_ID and port_id == RECEIVER_HOST_DOWNLINK_PORT_ID:
        return "downlink"
    return "non-incast"


def summarize(records: Iterable[IntervalRecord]) -> List[SwitchPortSummary]:
    # Group by the switch-port pair so each row represents one port on one switch.
    per_switch_port: Dict[tuple[int, int], List[int]] = {}
    for record in records:
        key = (record.switch_id, record.port_id)
        per_switch_port.setdefault(key, []).append(record.duration_ns)

    summaries: List[SwitchPortSummary] = []
    for (switch_id, port_id), durations in per_switch_port.items():
        total = sum(durations)
        count = len(durations)
        summaries.append(
            SwitchPortSummary(
                switch_id=switch_id,
                port_id=port_id,
                path_role=classify_path_role(switch_id, port_id),
                event_count=count,
                total_duration_ns=total,
                avg_duration_ns=total / count if count else 0.0,
                max_duration_ns=max(durations) if durations else 0,
            )
        )
    return summaries


def sort_summaries(
    summaries: List[SwitchPortSummary], sort_by: str
) -> List[SwitchPortSummary]:
    if sort_by == "switch":
        return sorted(summaries, key=lambda row: (row.switch_id, row.port_id))
    if sort_by == "count":
        return sorted(
            summaries,
            key=lambda row: (-row.event_count, row.switch_id, row.port_id),
        )
    if sort_by == "max":
        return sorted(
            summaries,
            key=lambda row: (-row.max_duration_ns, row.switch_id, row.port_id),
        )
    return sorted(
        summaries,
        key=lambda row: (-row.total_duration_ns, row.switch_id, row.port_id),
    )


def derive_output_path(path: Path, output_dir: Path | None) -> Path:
    target_dir = output_dir if output_dir is not None else path.parent
    stem = path.name
    if stem.endswith(".txt"):
        stem = stem[:-4]
    return target_dir / f"{stem}-switch-port-summary.csv"


def write_csv(path: Path, summaries: Sequence[SwitchPortSummary]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "switch_id",
                "port_id",
                "path_role",
                "event_count",
                "total_duration_us",
                "avg_duration_us",
            ]
        )
        for row in summaries:
            writer.writerow(
                [
                    row.switch_id,
                    row.port_id,
                    row.path_role,
                    row.event_count,
                    f"{row.total_duration_ns / 1e3:.3f}",
                    f"{row.avg_duration_ns / 1e3:.3f}",
                ]
            )


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir).resolve() if args.output_dir else None

    for raw_input in args.inputs:
        path = Path(raw_input).expanduser().resolve()
        records = load_records(path)
        summaries = sort_summaries(summarize(records), args.sort_by)
        output_path = derive_output_path(path, output_dir)
        write_csv(output_path, summaries)
        print(f"Wrote {output_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
