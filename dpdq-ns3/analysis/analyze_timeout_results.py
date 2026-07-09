#!/usr/bin/env python3
"""Analyze timeout results for config files.

For each config file containing TIMEOUT_OUTPUT_FILE, this script:
1) Loads the corresponding timeout file.
2) Computes summary metrics such as total, peak, and average timeout count.
3) Plots one timeout time-series line chart per config.
4) Plots one multi-config comparison chart.
5) Writes a summary CSV.

Expected timeout line format:
  time_ns timeout_count
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


@dataclass
class TimeoutPoint:
    time_ns: int
    timeout_count: int


@dataclass
class ConfigResult:
    config_path: Path
    timeout_file: Path
    num_points: int
    total_timeouts: int
    peak_timeout_count: int
    avg_timeout_count: float


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Analyze timeout files referenced by config files"
    )
    parser.add_argument(
        "--config-roots",
        nargs="+",
        default=[str(repo_root / "config" / "exp-configs")],
        help="One or more config roots to scan recursively for *.txt files",
    )
    parser.add_argument(
        "--output-dir",
        default=str(repo_root / "analysis" / "output"),
        help="Directory for generated plots and summary CSV",
    )
    parser.add_argument(
        "--time-unit",
        choices=["ns", "us", "ms"],
        default="ms",
        help="X-axis unit for timeout plots",
    )
    return parser.parse_args()


def discover_config_files(config_roots: Iterable[Path]) -> List[Path]:
    files: List[Path] = []
    for root in config_roots:
        if not root.exists():
            continue
        files.extend(sorted(p for p in root.rglob("*.txt") if p.is_file()))
    return files


def parse_config_timeout_output_path(config_path: Path) -> Optional[str]:
    for raw in config_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[0] == "TIMEOUT_OUTPUT_FILE":
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


def sanitize_name(path_text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", path_text)


def config_display_name(config_path: Path) -> str:
    return config_path.stem


def time_scale(unit: str) -> Tuple[float, str]:
    if unit == "ns":
        return 1.0, "ns"
    if unit == "us":
        return 1e3, "us"
    return 1e6, "ms"


def load_timeout_points(timeout_file: Path) -> List[TimeoutPoint]:
    points: List[TimeoutPoint] = []
    for raw in timeout_file.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("time_ns"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            points.append(TimeoutPoint(time_ns=int(parts[0]), timeout_count=int(parts[1])))
        except ValueError:
            continue
    return points


def plot_timeout_series(
    points: List[TimeoutPoint],
    title: str,
    out_png: Path,
    time_unit: str,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    scale, label = time_scale(time_unit)
    x = [point.time_ns / scale for point in points]
    y = [point.timeout_count for point in points]

    plt.figure(figsize=(8.0, 4.8))
    plt.plot(x, y, linewidth=1.8)
    plt.grid(True, alpha=0.3)
    plt.xlabel(f"Time ({label})")
    plt.ylabel("Timeout Count")
    plt.title(title)
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def plot_multi_timeout_series(
    series: List[Tuple[str, List[TimeoutPoint]]],
    title: str,
    out_png: Path,
    time_unit: str,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    scale, label = time_scale(time_unit)
    plt.figure(figsize=(10.0, 5.4))
    for name, points in series:
        if not points:
            continue
        x = [point.time_ns / scale for point in points]
        y = [point.timeout_count for point in points]
        plt.plot(x, y, linewidth=1.6, label=name)

    plt.grid(True, alpha=0.3, linestyle="--")
    plt.xlabel(f"Time ({label})")
    plt.ylabel("Timeout Count")
    plt.title(title)
    plt.legend(loc="best", fontsize=8)
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]

    config_roots = [Path(path).resolve() for path in args.config_roots]
    output_dir = Path(args.output_dir).resolve()
    timeout_dir = output_dir / "timeout"
    output_dir.mkdir(parents=True, exist_ok=True)
    timeout_dir.mkdir(parents=True, exist_ok=True)

    config_files = discover_config_files(config_roots)
    if not config_files:
        print("No config files found.")
        return 1

    results: List[ConfigResult] = []
    plot_series: List[Tuple[str, List[TimeoutPoint]]] = []
    skipped_no_timeout = 0
    skipped_missing_file = 0

    for cfg in config_files:
        raw_timeout = parse_config_timeout_output_path(cfg)
        if not raw_timeout:
            skipped_no_timeout += 1
            continue

        timeout_file = resolve_output_path(raw_timeout, repo_root, cfg)
        if not timeout_file.exists():
            skipped_missing_file += 1
            continue

        points = load_timeout_points(timeout_file)
        if not points:
            continue

        total_timeouts = sum(point.timeout_count for point in points)
        peak_timeout_count = max(point.timeout_count for point in points)
        avg_timeout_count = total_timeouts / len(points)

        cfg_rel = cfg.relative_to(repo_root) if cfg.is_relative_to(repo_root) else cfg
        png_name = sanitize_name(str(cfg_rel.with_suffix(""))) + "-timeout-timeseries.png"
        plot_timeout_series(
            points,
            title=f"Timeouts Over Time: {cfg_rel}",
            out_png=timeout_dir / png_name,
            time_unit=args.time_unit,
        )
        plot_series.append((config_display_name(cfg), points))

        results.append(
            ConfigResult(
                config_path=cfg,
                timeout_file=timeout_file,
                num_points=len(points),
                total_timeouts=total_timeouts,
                peak_timeout_count=peak_timeout_count,
                avg_timeout_count=avg_timeout_count,
            )
        )

    if not results:
        print("No valid timeout data found.")
        print(f"Skipped configs without TIMEOUT_OUTPUT_FILE: {skipped_no_timeout}")
        print(f"Skipped configs with missing timeout file: {skipped_missing_file}")
        return 2

    merged_plot = timeout_dir / "all-configs-timeout-timeseries.png"
    plot_multi_timeout_series(
        plot_series,
        title="Timeouts Over Time: all configs",
        out_png=merged_plot,
        time_unit=args.time_unit,
    )

    summary_csv = output_dir / "timeout_summary.csv"
    with summary_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "config_file",
                "timeout_file",
                "num_points",
                "total_timeouts",
                "peak_timeout_count",
                "avg_timeout_count",
            ]
        )
        for result in sorted(results, key=lambda item: str(item.config_path)):
            writer.writerow(
                [
                    str(result.config_path),
                    str(result.timeout_file),
                    result.num_points,
                    result.total_timeouts,
                    result.peak_timeout_count,
                    f"{result.avg_timeout_count:.6f}",
                ]
            )

    print(f"Configs scanned: {len(config_files)}")
    print(f"Analyzed configs: {len(results)}")
    print(f"Skipped (no TIMEOUT_OUTPUT_FILE): {skipped_no_timeout}")
    print(f"Skipped (missing timeout file): {skipped_missing_file}")
    print(f"Summary CSV: {summary_csv}")
    print(f"Timeout plots dir: {timeout_dir}")
    print(f"Merged timeout plot: {merged_plot}")

    print("\nTopline metrics:")
    print("config_file,num_points,total_timeouts,peak_timeout_count,avg_timeout_count")
    for result in sorted(results, key=lambda item: str(item.config_path)):
        print(
            f"{result.config_path},{result.num_points},{result.total_timeouts},"
            f"{result.peak_timeout_count},{result.avg_timeout_count:.6f}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
