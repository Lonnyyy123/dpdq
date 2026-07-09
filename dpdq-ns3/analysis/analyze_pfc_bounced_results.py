#!/usr/bin/env python3
"""Plot PFC pauses and bounced credits on the same figure.

This script scans config files, pairs:
  - `pfc-<workload>.txt` via `PFC_OUTPUT_FILE`
  - `cb-<workload>.txt` via `BOUNCED_OUTPUT_FILE`
  - `cbthr0-<workload>.txt` via `BOUNCED_OUTPUT_FILE`

For each workload with PFC and one or more bounced-credit series available, it
generates one figure with all time series overlaid on the same axes.
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


@dataclass
class TimePoint:
    time_ns: int
    count: int


@dataclass
class BouncedSeries:
    label: str
    config: Path
    data_file: Path
    points: List[TimePoint]


@dataclass
class WorkloadPlot:
    workload: str
    pfc_label: str
    pfc_config: Path
    pfc_file: Path
    pfc_points: List[TimePoint]
    bounced_series: List[BouncedSeries]


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Plot PFC pauses and bounced credits on the same figure"
    )
    parser.add_argument(
        "--config-roots",
        nargs="+",
        default=[str(repo_root / "analysis" / "tmp" / "fct-seven-configs")],
        help="One or more roots to scan recursively for config *.txt files",
    )
    parser.add_argument(
        "--output-dir",
        default=str(repo_root / "analysis" / "output" / "pfc-vs-bounced"),
        help="Directory for generated plots and summary CSV",
    )
    parser.add_argument(
        "--time-unit",
        choices=["ns", "us", "ms"],
        default="ms",
        help="X-axis unit for plots",
    )
    return parser.parse_args()


def discover_config_files(config_roots: Iterable[Path]) -> List[Path]:
    files: List[Path] = []
    for root in config_roots:
        if not root.exists():
            continue
        files.extend(sorted(p for p in root.rglob("*.txt") if p.is_file()))
    return files


def parse_config_output_path(config_path: Path, key: str) -> Optional[str]:
    for raw in config_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[0] == key:
            return parts[1]
    return None


def resolve_output_path(raw_path: str, repo_root: Path, config_path: Path) -> Path:
    if raw_path.startswith("/ns-3.19/"):
        return repo_root / raw_path[len("/ns-3.19/") :]

    path = Path(raw_path)
    if path.is_absolute():
        return path

    local_candidate = (config_path.parent / path).resolve()
    if local_candidate.exists():
        return local_candidate
    return (repo_root / path).resolve()


def sanitize_name(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text)


def parse_config_stem(config_path: Path) -> Tuple[str, str]:
    prefix, sep, workload = config_path.stem.partition("-")
    if not sep or not workload:
        return config_path.stem, ""
    return prefix, workload


def load_count_series(data_file: Path) -> List[TimePoint]:
    points: List[TimePoint] = []
    for raw in data_file.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("time_ns"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            points.append(TimePoint(time_ns=int(parts[0]), count=int(parts[1])))
        except ValueError:
            continue
    return points


def time_scale(unit: str) -> Tuple[float, str]:
    if unit == "ns":
        return 1.0, "ns"
    if unit == "us":
        return 1e3, "us"
    return 1e6, "ms"


def plot_pair(plot_data: WorkloadPlot, out_png: Path, time_unit: str) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    scale, unit_label = time_scale(time_unit)
    pfc_x = [point.time_ns / scale for point in plot_data.pfc_points]
    pfc_y = [point.count for point in plot_data.pfc_points]

    plt.figure(figsize=(9.0, 5.0))
    plt.plot(pfc_x, pfc_y, linewidth=1.8, label=plot_data.pfc_label)
    for bounced in plot_data.bounced_series:
        bounced_x = [point.time_ns / scale for point in bounced.points]
        bounced_y = [point.count for point in bounced.points]
        plt.plot(bounced_x, bounced_y, linewidth=1.8, label=bounced.label)
    plt.grid(True, alpha=0.3, linestyle="--")
    plt.xlabel(f"Time ({unit_label})")
    plt.ylabel("Count per Interval")
    plt.title(f"PFC Pauses vs Bounced Credits: {plot_data.workload}")
    plt.legend(loc="best", fontsize=9)
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]

    config_roots = [Path(path).resolve() for path in args.config_roots]
    output_dir = Path(args.output_dir).resolve()
    plot_dir = output_dir / "pfc_bounced"
    output_dir.mkdir(parents=True, exist_ok=True)
    plot_dir.mkdir(parents=True, exist_ok=True)

    config_files = discover_config_files(config_roots)
    if not config_files:
        print("No config files found.")
        return 1

    pfc_configs: Dict[str, Tuple[Path, Path]] = {}
    bounced_configs: Dict[str, List[Tuple[str, Path, Path]]] = {}

    for cfg in config_files:
        prefix, workload = parse_config_stem(cfg)
        if not workload:
            continue

        if prefix == "pfc":
            raw_pfc = parse_config_output_path(cfg, "PFC_OUTPUT_FILE")
            if not raw_pfc or raw_pfc == "/dev/null":
                continue
            pfc_file = resolve_output_path(raw_pfc, repo_root, cfg)
            if pfc_file.exists():
                pfc_configs[workload] = (cfg, pfc_file)
        elif prefix in {"cb", "cbthr0"}:
            raw_bounced = parse_config_output_path(cfg, "BOUNCED_OUTPUT_FILE")
            if not raw_bounced or raw_bounced == "/dev/null":
                continue
            bounced_file = resolve_output_path(raw_bounced, repo_root, cfg)
            if bounced_file.exists():
                bounced_configs.setdefault(workload, []).append((cfg.stem, cfg, bounced_file))

    workloads = sorted(set(pfc_configs) & set(bounced_configs))
    if not workloads:
        print("No workload has both pfc and bounced data.")
        return 2

    plots: List[WorkloadPlot] = []
    for workload in workloads:
        pfc_config, pfc_file = pfc_configs[workload]
        pfc_points = load_count_series(pfc_file)
        if not pfc_points:
            continue

        bounced_series: List[BouncedSeries] = []
        for label, bounced_config, bounced_file in bounced_configs[workload]:
            bounced_points = load_count_series(bounced_file)
            if not bounced_points:
                continue
            bounced_series.append(
                BouncedSeries(
                    label=label,
                    config=bounced_config,
                    data_file=bounced_file,
                    points=bounced_points,
                )
            )

        if not bounced_series:
            continue

        plots.append(
            WorkloadPlot(
                workload=workload,
                pfc_label=pfc_config.stem,
                pfc_config=pfc_config,
                pfc_file=pfc_file,
                pfc_points=pfc_points,
                bounced_series=bounced_series,
            )
        )

    if not plots:
        print("No valid paired time series found.")
        return 3

    for plot_data in plots:
        out_png = plot_dir / f"{sanitize_name(plot_data.workload)}-pfc-vs-bounced.png"
        plot_pair(plot_data, out_png=out_png, time_unit=args.time_unit)

    summary_csv = output_dir / "pfc_bounced_summary.csv"
    with summary_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "workload",
                "pfc_config",
                "pfc_file",
                "pfc_points",
                "bounced_labels",
                "bounced_configs",
                "bounced_files",
                "bounced_point_counts",
            ]
        )
        for plot_data in plots:
            writer.writerow(
                [
                    plot_data.workload,
                    str(plot_data.pfc_config),
                    str(plot_data.pfc_file),
                    len(plot_data.pfc_points),
                    ";".join(series.label for series in plot_data.bounced_series),
                    ";".join(str(series.config) for series in plot_data.bounced_series),
                    ";".join(str(series.data_file) for series in plot_data.bounced_series),
                    ";".join(str(len(series.points)) for series in plot_data.bounced_series),
                ]
            )

    print(f"Configs scanned: {len(config_files)}")
    print(f"Paired workloads: {len(plots)}")
    print(f"Plot dir: {plot_dir}")
    print(f"Summary CSV: {summary_csv}")
    print("Generated plots:")
    for plot_data in plots:
        print(plot_dir / f"{sanitize_name(plot_data.workload)}-pfc-vs-bounced.png")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
