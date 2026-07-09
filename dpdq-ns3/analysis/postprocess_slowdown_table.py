#!/usr/bin/env python3
"""Post-process mix FCT outputs into slowdown summary tables.

This script scans one or more mix directories (or FCT files), computes
slowdown = fct / ideal_fct, and writes a text summary next to each FCT file.

The default table format uses the report buckets:
  [0k, 100k), [100k, 1M), [1M, ∞), BG total, All total, and Foreground.
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MIX_DIR = REPO_ROOT / "mix"
DEFAULT_BUCKETS: Sequence[Tuple[str, int, Optional[int]]] = (
    ("[0k, 100k)", 0, 100_000),
    ("[100k, 1M)", 100_000, 1_000_000),
    ("[1M, ∞)", 1_000_000, None),
)


@dataclass
class FlowRecord:
    size_bytes: int
    fct_ns: int
    ideal_fct_ns: int
    slowdown: float
    is_foreground: bool


@dataclass
class GroupSummary:
    label: str
    count: int
    bytes_total: int
    p50_slowdown: float
    p99_slowdown: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build per-run slowdown summary tables from mix FCT outputs."
    )
    parser.add_argument(
        "inputs",
        nargs="*",
        default=[str(DEFAULT_MIX_DIR)],
        help="Mix directories or *-fct.txt files to process. Defaults to repo mix/.",
    )
    parser.add_argument(
        "--output-suffix",
        default="-slowdown.txt",
        help="Suffix for generated text files.",
    )
    return parser.parse_args()


def percentile(sorted_values: Sequence[float], p: float) -> float:
    if not sorted_values:
        return float("nan")
    if len(sorted_values) == 1:
        return float(sorted_values[0])
    rank = (p / 100.0) * (len(sorted_values) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(sorted_values[lo])
    frac = rank - lo
    return float(sorted_values[lo] + frac * (sorted_values[hi] - sorted_values[lo]))


def discover_fct_files(inputs: Iterable[str]) -> List[Path]:
    fct_files: List[Path] = []
    for raw in inputs:
        path = Path(raw).expanduser().resolve()
        if path.is_file():
            if path.name.endswith(".txt"):
                fct_files.append(path)
            continue
        if path.is_dir():
            fct_files.extend(sorted(path.rglob("*-fct.txt")))
    deduped = sorted(dict.fromkeys(fct_files))
    return deduped


def parse_fct_line(parts: Sequence[str]) -> Optional[FlowRecord]:
    if len(parts) < 8:
        return None

    try:
        size_bytes = int(parts[4])
        fct_ns = int(parts[6])
    except ValueError:
        return None

    ideal_idx: Optional[int] = None
    fg_idx: Optional[int] = None

    if len(parts) >= 11:
        ideal_idx = 8
        fg_idx = 10
    elif len(parts) == 10:
        ideal_idx = 7
        fg_idx = 9
    elif len(parts) == 9:
        ideal_idx = 8
    elif len(parts) == 8:
        ideal_idx = 7

    if ideal_idx is None:
        return None

    try:
        ideal_fct_ns = int(parts[ideal_idx])
    except ValueError:
        return None

    if ideal_fct_ns <= 0:
        return None

    is_foreground = False
    if fg_idx is not None and len(parts) > fg_idx:
        try:
            is_foreground = int(parts[fg_idx]) != 0
        except ValueError:
            is_foreground = False

    return FlowRecord(
        size_bytes=size_bytes,
        fct_ns=fct_ns,
        ideal_fct_ns=ideal_fct_ns,
        slowdown=fct_ns / ideal_fct_ns,
        is_foreground=is_foreground,
    )


def load_flow_records(fct_file: Path) -> List[FlowRecord]:
    records: List[FlowRecord] = []
    for raw in fct_file.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line:
            continue
        record = parse_fct_line(line.split())
        if record is not None:
            records.append(record)
    return records


def summarize_group(label: str, records: Sequence[FlowRecord]) -> GroupSummary:
    if not records:
        return GroupSummary(
            label=label,
            count=0,
            bytes_total=0,
            p50_slowdown=float("nan"),
            p99_slowdown=float("nan"),
        )

    slowdowns = sorted(record.slowdown for record in records)
    bytes_total = sum(record.size_bytes for record in records)
    return GroupSummary(
        label=label,
        count=len(records),
        bytes_total=bytes_total,
        p50_slowdown=percentile(slowdowns, 50.0),
        p99_slowdown=percentile(slowdowns, 99.0),
    )


def build_summaries(records: Sequence[FlowRecord]) -> List[GroupSummary]:
    summaries: List[GroupSummary] = []
    for label, lower, upper in DEFAULT_BUCKETS:
        group = [
            record
            for record in records
            if record.size_bytes >= lower
            and (upper is None or record.size_bytes < upper)
        ]
        summaries.append(summarize_group(label, group))

    summaries.append(summarize_group("BG total", records))
    return summaries


def background_records(records: Sequence[FlowRecord]) -> List[FlowRecord]:
    return [record for record in records if not record.is_foreground]


def foreground_records(records: Sequence[FlowRecord]) -> List[FlowRecord]:
    return [record for record in records if record.is_foreground]


def format_float(value: float) -> str:
    if math.isnan(value):
        return "NA"
    return f"{value:.3f}"


def format_bytes(value: int) -> str:
    units = ("B", "KB", "MB", "GB", "TB")
    scaled = float(value)
    unit = units[0]
    for candidate in units:
        unit = candidate
        if scaled < 1000.0 or candidate == units[-1]:
            break
        scaled /= 1000.0
    if unit == "B":
        return f"{value} B"
    return f"{scaled:.2f} {unit}"


def derive_output_path(fct_file: Path, suffix: str) -> Path:
    if fct_file.name.endswith("-fct.txt"):
        prefix = fct_file.name[: -len("-fct.txt")]
    else:
        prefix = fct_file.stem
    return fct_file.parent / f"{prefix}{suffix}"


def render_table(
    fct_file: Path,
    records: Sequence[FlowRecord],
    summaries: Sequence[GroupSummary],
) -> str:
    background_summary = summarize_group("Background", background_records(records))
    all_summary = summarize_group("All total", records)
    foreground_summary = summarize_group("Foreground", foreground_records(records))

    header = ("Bucket", "P50 slowdown", "P99 slowdown", "Flows/Bytes")
    rows: List[Tuple[str, str, str, str]] = []
    for summary in summaries:
        rows.append(
            (
                summary.label,
                format_float(summary.p50_slowdown),
                format_float(summary.p99_slowdown),
                f"{summary.count} / {format_bytes(summary.bytes_total)}",
            )
        )
    rows.append(
        (
            all_summary.label,
            format_float(all_summary.p50_slowdown),
            format_float(all_summary.p99_slowdown),
            f"{all_summary.count} / {format_bytes(all_summary.bytes_total)}",
        )
    )
    rows.extend(
        (
            ("", "", "", ""),
            ("Foreground", "P50 slowdown", "P99 slowdown", "Flows/Bytes"),
            (
                "",
                format_float(foreground_summary.p50_slowdown),
                format_float(foreground_summary.p99_slowdown),
                f"{foreground_summary.count} / {format_bytes(foreground_summary.bytes_total)}",
            ),
        )
    )

    widths = [
        max(len(header[0]), *(len(row[0]) for row in rows)),
        max(len(header[1]), *(len(row[1]) for row in rows)),
        max(len(header[2]), *(len(row[2]) for row in rows)),
        max(len(header[3]), *(len(row[3]) for row in rows)),
    ]

    def format_row(cols: Sequence[str]) -> str:
        return (
            f"{cols[0]:<{widths[0]}}  "
            f"{cols[1]:>{widths[1]}}  "
            f"{cols[2]:>{widths[2]}}  "
            f"{cols[3]:>{widths[3]}}"
        )

    lines = [
        f"Source FCT: {fct_file}",
        f"Total flows: {len(records)}",
        f"Background slowdown: P50={format_float(background_summary.p50_slowdown)} "
        f"P99={format_float(background_summary.p99_slowdown)} "
        f"Bytes={format_bytes(background_summary.bytes_total)}",
        "",
        format_row(header),
        format_row(("-" * widths[0], "-" * widths[1], "-" * widths[2], "-" * widths[3])),
    ]
    lines.extend(format_row(row) for row in rows)
    lines.append("")
    lines.append("Slowdown = FCT / ideal_FCT.")
    return "\n".join(lines)


def process_fct_file(fct_file: Path, output_suffix: str) -> Optional[Path]:
    records = load_flow_records(fct_file)
    if not records:
        return None
    summaries = build_summaries(background_records(records))
    out_path = derive_output_path(fct_file, output_suffix)
    out_path.write_text(render_table(fct_file, records, summaries) + "\n", encoding="utf-8")
    return out_path


def main() -> int:
    args = parse_args()
    fct_files = discover_fct_files(args.inputs)
    if not fct_files:
        print("No FCT files found.")
        return 1

    written = 0
    skipped = 0
    for fct_file in fct_files:
        out_path = process_fct_file(fct_file, args.output_suffix)
        if out_path is None:
            skipped += 1
            print(f"Skipped (no valid slowdown rows): {fct_file}")
            continue
        written += 1
        print(f"Wrote: {out_path}")

    print(f"Processed {written} FCT file(s), skipped {skipped}.")
    return 0 if written > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
