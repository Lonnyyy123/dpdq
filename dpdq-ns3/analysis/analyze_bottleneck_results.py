#!/usr/bin/env python3
"""Analyze bottleneck monitor outputs referenced by config files.

For each config file containing BOTTLENECK_MON_FILE, this script:
1) Loads the corresponding bottleneck file.
2) Extracts one monitored link (src_switch, dst_switch, ifindex).
3) Plots a merged comparison figure with shared time axis:
   - throughput (tx_bps)
   - data queue max:
       * cb/cbthr0 use cb_data_q_bytes_max
       * gbn/irn/pfc use egress_data_q_bytes_max
   - phantom queue max (cb_phantom_q_bytes_max), only for cb/cbthr0
4) Writes a summary CSV.

Expected bottleneck line formats:
  CB:
  time_ns src_switch dst_switch ifindex tx_bps
  cb_data_q_bytes_inst cb_data_q_bytes_max
  cb_credit_q_bytes_inst cb_credit_q_bytes_max
  cb_phantom_q_bytes_inst cb_phantom_q_bytes_max
  Non-CB:
  time_ns src_switch dst_switch ifindex tx_bps
  egress_data_q_bytes_inst egress_data_q_bytes_max
"""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


@dataclass
class BottleneckPoint:
    time_ns: int
    src_switch: int
    dst_switch: int
    ifindex: int
    tx_bps: int
    data_q_max: int
    credit_q_max: int
    phantom_q_max: int
    format_kind: str


@dataclass
class ConfigResult:
    config_path: Path
    bottleneck_file: Path
    display_name: str
    src_switch: int
    dst_switch: int
    ifindex: int
    num_points: int
    max_tx_bps: int
    max_data_q_max: int
    max_credit_q_max: int
    max_phantom_q_max: int
    format_kind: str


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Analyze bottleneck monitor files referenced by config files"
    )
    parser.add_argument(
        "--config-roots",
        nargs="+",
        default=[str(repo_root / "analysis" / "tmp" / "configs")],
        help="One or more config roots to scan recursively for *.txt files",
    )
    parser.add_argument(
        "--output-dir",
        default=str(repo_root / "analysis" / "output" / "bottleneck"),
        help="Directory for generated plots and summary CSV",
    )
    parser.add_argument(
        "--time-unit",
        choices=["ns", "us", "ms"],
        default="us",
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


def load_bottleneck_points(bottleneck_file: Path) -> List[BottleneckPoint]:
    points: List[BottleneckPoint] = []
    for raw in bottleneck_file.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("time_ns"):
            continue
        parts = line.split()
        try:
            if len(parts) >= 11:
                points.append(
                    BottleneckPoint(
                        time_ns=int(parts[0]),
                        src_switch=int(parts[1]),
                        dst_switch=int(parts[2]),
                        ifindex=int(parts[3]),
                        tx_bps=int(parts[4]),
                        data_q_max=int(parts[6]),
                        credit_q_max=int(parts[8]),
                        phantom_q_max=int(parts[10]),
                        format_kind="cb",
                    )
                )
            elif len(parts) >= 7:
                points.append(
                    BottleneckPoint(
                        time_ns=int(parts[0]),
                        src_switch=int(parts[1]),
                        dst_switch=int(parts[2]),
                        ifindex=int(parts[3]),
                        tx_bps=int(parts[4]),
                        data_q_max=int(parts[6]),
                        credit_q_max=0,
                        phantom_q_max=0,
                        format_kind="egress",
                    )
                )
        except ValueError:
            continue
    return points


def select_monitored_link(points: List[BottleneckPoint]) -> Tuple[int, int, int]:
    counts = Counter((p.src_switch, p.dst_switch, p.ifindex) for p in points)
    return counts.most_common(1)[0][0]


def filter_link_points(
    points: List[BottleneckPoint], link_key: Tuple[int, int, int]
) -> List[BottleneckPoint]:
    src_switch, dst_switch, ifindex = link_key
    return [
        p
        for p in points
        if p.src_switch == src_switch and p.dst_switch == dst_switch and p.ifindex == ifindex
    ]


def plot_throughput_subplots(
    series: List[Tuple[str, List[BottleneckPoint]]],
    out_png: Path,
    time_unit: str,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    valid_series = [(name, points) for name, points in series if points]
    if not valid_series:
        return

    scale, unit_label = time_scale(time_unit)
    fig, axes = plt.subplots(
        len(valid_series),
        1,
        figsize=(11.5, max(7.5, 1.8 * len(valid_series))),
        sharex=True,
    )
    if len(valid_series) == 1:
        axes = [axes]

    for idx, (name, points) in enumerate(valid_series):
        x = [point.time_ns / scale for point in points]
        throughput_gbps = [point.tx_bps / 1e9 for point in points]

        axes[idx].plot(x, throughput_gbps, linewidth=1.5, label=name)

    for idx, ax in enumerate(axes):
        if idx == 0:
            ax.set_title("Bottleneck Throughput per 8us")
        ax.set_ylabel("Throughput\n(Gbps)")
        ax.grid(True, alpha=0.3, linestyle="--")
        ax.legend(loc="best", fontsize=8)
        ax.set_ylim(0, 40)
        ax.set_yticks([0, 10, 20, 30, 40])

    axes[-1].set_xlabel(f"Time ({unit_label})")

    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)


def plot_queue_subplots(
    series: List[Tuple[str, List[BottleneckPoint]]],
    out_png: Path,
    time_unit: str,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    valid_series = [(name, points) for name, points in series if points]
    if not valid_series:
        return

    scale, unit_label = time_scale(time_unit)
    fig, axes = plt.subplots(2, 1, figsize=(11.5, 6.8), sharex=True)
    data_q_ax, phantom_q_ax = axes

    for name, points in valid_series:
        x = [point.time_ns / scale for point in points]
        data_q_max = [point.data_q_max for point in points]
        data_q_ax.plot(x, data_q_max, linewidth=1.7, label=name)

        if any(point.format_kind == "cb" for point in points):
            phantom_q_max = [point.phantom_q_max for point in points]
            phantom_q_ax.plot(x, phantom_q_max, linewidth=1.7, label=name)

    data_q_ax.set_title("Bottleneck Data Queue Max per 8us")
    data_q_ax.set_ylabel("Data Queue Max (Bytes)")
    phantom_q_ax.set_title("Bottleneck Phantom Queue Max per 8us")
    phantom_q_ax.set_ylabel("Phantom Queue Max (Bytes)")
    phantom_q_ax.set_xlabel(f"Time ({unit_label})")

    for ax in axes:
        ax.grid(True, alpha=0.3, linestyle="--")
        ax.legend(loc="best", fontsize=8)

    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)


def plot_throughput_heatmap(
    series: List[Tuple[str, List[BottleneckPoint]]],
    out_png: Path,
    time_unit: str,
) -> None:
    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib and numpy are required for plotting. Install them with: pip install matplotlib numpy"
        ) from exc

    valid_series = [(name, points) for name, points in series if points]
    if not valid_series:
        return

    scale, unit_label = time_scale(time_unit)
    max_len = max(len(points) for _, points in valid_series)
    heatmap = np.full((len(valid_series), max_len), np.nan, dtype=float)
    time_axis: List[float] = []

    for row_idx, (_, points) in enumerate(valid_series):
        if len(points) > len(time_axis):
            time_axis = [point.time_ns / scale for point in points]
        for col_idx, point in enumerate(points):
            heatmap[row_idx, col_idx] = point.tx_bps / 1e9

    masked = np.ma.masked_invalid(heatmap)
    fig, ax = plt.subplots(figsize=(11.0, 3.8))
    image = ax.imshow(
        masked,
        aspect="auto",
        interpolation="nearest",
        origin="upper",
        cmap="viridis",
    )
    ax.set_yticks(range(len(valid_series)))
    ax.set_yticklabels([name for name, _ in valid_series])

    if time_axis:
        tick_count = min(8, len(time_axis))
        if tick_count > 1:
            tick_positions = np.linspace(0, len(time_axis) - 1, tick_count)
            tick_indices = [int(round(pos)) for pos in tick_positions]
            ax.set_xticks(tick_indices)
            ax.set_xticklabels([f"{time_axis[idx]:.0f}" for idx in tick_indices])
    ax.set_xlabel(f"Time ({unit_label})")
    ax.set_title("Bottleneck Throughput Heatmap")

    cbar = fig.colorbar(image, ax=ax)
    cbar.set_label("Throughput (Gbps)")

    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]

    config_roots = [Path(path).resolve() for path in args.config_roots]
    output_dir = Path(args.output_dir).resolve()
    bottleneck_dir = output_dir / "bottleneck"
    output_dir.mkdir(parents=True, exist_ok=True)
    bottleneck_dir.mkdir(parents=True, exist_ok=True)

    config_files = discover_config_files(config_roots)
    if not config_files:
        print("No config files found.")
        return 1

    results: List[ConfigResult] = []
    plot_series: List[Tuple[str, List[BottleneckPoint]]] = []
    skipped_no_bottleneck = 0
    skipped_missing_file = 0

    for cfg in config_files:
        raw_bottleneck = parse_config_output_path(cfg, "BOTTLENECK_MON_FILE")
        if not raw_bottleneck:
            skipped_no_bottleneck += 1
            continue

        bottleneck_file = resolve_output_path(raw_bottleneck, repo_root, cfg)
        if not bottleneck_file.exists():
            skipped_missing_file += 1
            continue

        points = load_bottleneck_points(bottleneck_file)
        if not points:
            continue

        link_key = select_monitored_link(points)
        filtered_points = filter_link_points(points, link_key)
        if not filtered_points:
            continue

        display_name = config_display_name(cfg)
        plot_series.append((display_name, filtered_points))

        results.append(
            ConfigResult(
                config_path=cfg,
                bottleneck_file=bottleneck_file,
                display_name=display_name,
                src_switch=link_key[0],
                dst_switch=link_key[1],
                ifindex=link_key[2],
                num_points=len(filtered_points),
                max_tx_bps=max(point.tx_bps for point in filtered_points),
                max_data_q_max=max(point.data_q_max for point in filtered_points),
                max_credit_q_max=max(point.credit_q_max for point in filtered_points),
                max_phantom_q_max=max(point.phantom_q_max for point in filtered_points),
                format_kind=filtered_points[0].format_kind,
            )
        )

    if not results:
        print("No valid bottleneck data found.")
        print(f"Skipped configs without BOTTLENECK_MON_FILE: {skipped_no_bottleneck}")
        print(f"Skipped configs with missing bottleneck file: {skipped_missing_file}")
        return 2

    throughput_plot = bottleneck_dir / "all-configs-throughput-subplots.png"
    plot_throughput_subplots(
        plot_series,
        out_png=throughput_plot,
        time_unit=args.time_unit,
    )
    queue_plot = bottleneck_dir / "all-configs-queue-subplots.png"
    plot_queue_subplots(
        plot_series,
        out_png=queue_plot,
        time_unit=args.time_unit,
    )
    throughput_heatmap_png = bottleneck_dir / "all-configs-throughput-heatmap.png"
    plot_throughput_heatmap(
        plot_series,
        out_png=throughput_heatmap_png,
        time_unit=args.time_unit,
    )

    summary_csv = output_dir / "bottleneck_summary.csv"
    with summary_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "config_file",
                "display_name",
                "bottleneck_file",
                "src_switch",
                "dst_switch",
                "ifindex",
                "num_points",
                "max_tx_bps",
                "max_data_q_bytes_max",
                "max_credit_q_bytes_max",
                "max_phantom_q_bytes_max",
                "format_kind",
            ]
        )
        for result in sorted(results, key=lambda item: str(item.config_path)):
            writer.writerow(
                [
                    str(result.config_path),
                    result.display_name,
                    str(result.bottleneck_file),
                    result.src_switch,
                    result.dst_switch,
                    result.ifindex,
                    result.num_points,
                    result.max_tx_bps,
                    result.max_data_q_max,
                    result.max_credit_q_max,
                    result.max_phantom_q_max,
                    result.format_kind,
                ]
            )

    print(f"Configs scanned: {len(config_files)}")
    print(f"Analyzed configs: {len(results)}")
    print(f"Skipped (no BOTTLENECK_MON_FILE): {skipped_no_bottleneck}")
    print(f"Skipped (missing bottleneck file): {skipped_missing_file}")
    print(f"Summary CSV: {summary_csv}")
    print(f"Bottleneck plots dir: {bottleneck_dir}")
    print(f"Throughput subplot figure: {throughput_plot}")
    print(f"Queue subplot figure: {queue_plot}")
    print(f"Throughput heatmap: {throughput_heatmap_png}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
