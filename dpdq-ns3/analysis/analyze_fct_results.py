#!/usr/bin/env python3
"""Analyze FCT results for config files.

For each config file containing FCT_OUTPUT_FILE, this script:
1) Loads the corresponding FCT file.
2) Computes P50/P90 FCT and average slowdown (fct / ideal_fct).
3) Plots one FCT CDF figure per config.
4) Computes PhantomPass-style slowdown-vs-size buckets:
   sort by flow size, split into roughly-500-flow buckets,
   then compute per-bucket P50/P99 slowdown.
5) Writes summary CSVs and comparison plots.

Expected FCT line format:
  sip dip sport dport size start_time fct standalone_fct ideal_fct
Optional extra columns after the 9th one are ignored for slowdown analysis.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


@dataclass
class ConfigResult:
    config_path: Path
    fct_file: Path
    num_flows: int
    p50_ns: float
    p90_ns: float
    avg_slowdown: float
    run_status: str
    run_returncode: int


@dataclass
class FlowMetric:
    size_bytes: int
    fct_ns: int
    slowdown: float


@dataclass
class SlowdownBucket:
    median_size_bytes: float
    p50_slowdown: float
    p99_slowdown: float
    count: int


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Analyze FCT files referenced by config files")
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
        "--plot-unit",
        choices=["ns", "us", "ms"],
        default="us",
        help="X-axis unit for CDF plots",
    )
    parser.add_argument(
        "--slowdown-bucket-size",
        type=int,
        default=500,
        help="Target number of flows per slowdown-vs-size bucket",
    )
    parser.add_argument(
        "--run-main",
        action="store_true",
        help="Run each config via main.py before loading FCT files",
    )
    parser.add_argument(
        "--main-py",
        default=str(repo_root / "main.py"),
        help="Path to main.py used when --run-main is enabled",
    )
    parser.add_argument(
        "--main-timeout-sec",
        type=int,
        default=0,
        help="Timeout per main.py run in seconds (0 means no timeout)",
    )
    parser.add_argument(
        "--stop-on-run-error",
        action="store_true",
        help="Stop immediately if one config fails during --run-main",
    )
    return parser.parse_args()


def discover_config_files(config_roots: Iterable[Path]) -> List[Path]:
    files: List[Path] = []
    for root in config_roots:
        if not root.exists():
            continue
        files.extend(sorted(p for p in root.rglob("*.txt") if p.is_file()))
    return files


def parse_config_fct_output_path(config_path: Path) -> Optional[str]:
    for raw in config_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[0] == "FCT_OUTPUT_FILE":
            return parts[1]
    return None


def resolve_fct_path(raw_path: str, repo_root: Path, config_path: Path) -> Path:
    # Docker-style absolute path used by this repo.
    if raw_path.startswith("/ns-3.19/"):
        return repo_root / raw_path[len("/ns-3.19/") :]

    p = Path(raw_path)
    if p.is_absolute():
        return p

    # Relative path: prefer config-local path, then repo-root path.
    local_candidate = (config_path.parent / p).resolve()
    if local_candidate.exists():
        return local_candidate
    return (repo_root / p).resolve()


def percentile(sorted_values: List[float], p: float) -> float:
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


def load_fct_metrics(fct_file: Path) -> Tuple[List[int], List[float], List[FlowMetric]]:
    fcts: List[int] = []
    slowdowns: List[float] = []
    flow_metrics: List[FlowMetric] = []

    for raw in fct_file.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 9:
            continue

        try:
            size_bytes = int(parts[4])
            fct_ns = int(parts[6])
            ideal_fct_ns = int(parts[8])
        except ValueError:
            continue

        fcts.append(fct_ns)
        if ideal_fct_ns > 0:
            slowdown = fct_ns / ideal_fct_ns
            slowdowns.append(slowdown)
            flow_metrics.append(
                FlowMetric(size_bytes=size_bytes, fct_ns=fct_ns, slowdown=slowdown)
            )

    return fcts, slowdowns, flow_metrics


def build_slowdown_buckets(
    flow_metrics: List[FlowMetric], bucket_target_size: int
) -> List[SlowdownBucket]:
    if bucket_target_size <= 0 or len(flow_metrics) < bucket_target_size:
        return []

    sorted_metrics = sorted(flow_metrics, key=lambda x: x.size_bytes)
    bin_count = len(sorted_metrics) // bucket_target_size
    if bin_count == 0:
        return []

    values_per_bin = len(sorted_metrics) // bin_count
    rem = len(sorted_metrics) % bin_count
    buckets: List[SlowdownBucket] = []

    for bin_num in range(bin_count):
        bin_start = bin_num * values_per_bin
        bin_end = (bin_num + 1) * values_per_bin - 1
        if bin_num == bin_count - 1:
            bin_end += rem

        chunk = sorted_metrics[bin_start : bin_end + 1]
        sizes = sorted(float(item.size_bytes) for item in chunk)
        slowdowns = sorted(item.slowdown for item in chunk)
        buckets.append(
            SlowdownBucket(
                median_size_bytes=percentile(sizes, 50.0),
                p50_slowdown=percentile(slowdowns, 50.0),
                p99_slowdown=percentile(slowdowns, 99.0),
                count=len(chunk),
            )
        )

    return buckets


def sanitize_name(path_text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", path_text)


def config_display_name(config_path: Path) -> str:
    return config_path.stem


def unit_scale(unit: str) -> Tuple[float, str]:
    if unit == "ns":
        return 1.0, "ns"
    if unit == "us":
        return 1e3, "us"
    return 1e6, "ms"


def plot_cdf(fcts_ns: List[int], title: str, out_png: Path, plot_unit: str) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    sorted_fct = sorted(fcts_ns)
    n = len(sorted_fct)
    y = [(i + 1) / n for i in range(n)]

    scale, label = unit_scale(plot_unit)
    x = [v / scale for v in sorted_fct]

    plt.figure(figsize=(7.2, 4.8))
    plt.plot(x, y, linewidth=1.8)
    plt.grid(True, alpha=0.3)
    plt.xlabel(f"FCT ({label})")
    plt.ylabel("CDF")
    plt.title(title)
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def plot_multi_cdf(series: List[Tuple[str, List[int]]], title: str, out_png: Path, plot_unit: str) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    scale, label = unit_scale(plot_unit)

    plt.figure(figsize=(8.0, 5.2))
    for name, fcts_ns in series:
        if not fcts_ns:
            continue
        sorted_fct = sorted(fcts_ns)
        n = len(sorted_fct)
        y = [(i + 1) / n for i in range(n)]
        x = [v / scale for v in sorted_fct]
        plt.plot(x, y, linewidth=1.5, label=name)

    plt.grid(True, alpha=0.3)
    plt.xlabel(f"FCT ({label})")
    plt.ylabel("CDF")
    plt.title(title)
    plt.legend(loc="lower right", fontsize=8)
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def write_slowdown_buckets_csv(out_csv: Path, buckets: List[SlowdownBucket]) -> None:
    with out_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["median_size_bytes", "p50_slowdown", "p99_slowdown", "count"])
        for bucket in buckets:
            writer.writerow(
                [
                    f"{bucket.median_size_bytes:.3f}",
                    f"{bucket.p50_slowdown:.6f}",
                    f"{bucket.p99_slowdown:.6f}",
                    bucket.count,
                ]
            )


def plot_slowdown_comparison_panels(
    series: List[Tuple[str, List[SlowdownBucket]]],
    out_png: Path,
) -> None:
    try:
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for plotting. Install it with: pip install matplotlib"
        ) from exc

    fig, axes = plt.subplots(2, 1, figsize=(10.5, 10.0), sharex=True)
    metric_specs = [
        ("p50", "Comparison of Median (P50) Slowdown", "P50 Slowdown"),
        ("p99", "Comparison of Tail (P99) Slowdown", "P99 Slowdown"),
    ]

    for ax, (metric, title, ylabel) in zip(axes, metric_specs):
        for name, buckets in series:
            if not buckets:
                continue
            x = [bucket.median_size_bytes for bucket in buckets]
            if metric == "p50":
                y = [bucket.p50_slowdown for bucket in buckets]
            else:
                y = [bucket.p99_slowdown for bucket in buckets]
            ax.plot(
                x,
                y,
                marker="o",
                linewidth=1.7,
                markersize=3.0,
                markeredgewidth=0.0,
                alpha=0.9,
                label=name,
            )

        ax.set_xscale("log")
        ax.grid(True, which="both", alpha=0.35, linestyle="--")
        ax.set_ylabel(ylabel)
        ax.set_title(title)
        ax.legend(loc="upper right", fontsize=8)

    axes[-1].set_xlabel("Flow Size (Bytes)")
    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)


def run_config_with_main(
    cfg: Path,
    repo_root: Path,
    main_py: Path,
    timeout_sec: int,
    run_log_dir: Path,
) -> Tuple[bool, int]:
    cmd = [sys.executable, str(main_py), "-i", str(cfg)]
    timeout = None if timeout_sec <= 0 else timeout_sec

    try:
        proc = subprocess.run(
            cmd,
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
        )
        run_rc = int(proc.returncode)
        run_ok = run_rc == 0
        stderr_text = proc.stderr or ""
        if "Traceback" in stderr_text:
            run_ok = False
            if run_rc == 0:
                run_rc = 1

        log_prefix = sanitize_name(str(cfg))
        (run_log_dir / f"{log_prefix}.stdout.log").write_text(
            proc.stdout or "", encoding="utf-8"
        )
        (run_log_dir / f"{log_prefix}.stderr.log").write_text(
            proc.stderr or "", encoding="utf-8"
        )
        return run_ok, run_rc
    except subprocess.TimeoutExpired as exc:
        log_prefix = sanitize_name(str(cfg))
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
        (run_log_dir / f"{log_prefix}.stdout.log").write_text(stdout, encoding="utf-8")
        (run_log_dir / f"{log_prefix}.stderr.log").write_text(stderr, encoding="utf-8")
        return False, 124


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]

    config_roots = [Path(p).resolve() for p in args.config_roots]
    out_dir = Path(args.output_dir).resolve()
    cdf_dir = out_dir / "cdf"
    slowdown_dir = out_dir / "slowdown"
    run_log_dir = out_dir / "run_logs"
    out_dir.mkdir(parents=True, exist_ok=True)
    cdf_dir.mkdir(parents=True, exist_ok=True)
    slowdown_dir.mkdir(parents=True, exist_ok=True)
    run_log_dir.mkdir(parents=True, exist_ok=True)

    main_py = Path(args.main_py).resolve()
    if args.run_main and not main_py.exists():
        print(f"main.py not found: {main_py}")
        return 3

    config_files = discover_config_files(config_roots)
    if not config_files:
        print("No config files found.")
        return 1

    results: List[ConfigResult] = []
    cdf_series: List[Tuple[str, List[int]]] = []
    slowdown_series: List[Tuple[str, List[SlowdownBucket]]] = []
    skipped_no_fct = 0
    skipped_missing_file = 0
    run_failed = 0
    run_total = 0

    for cfg in config_files:
        run_status = "not_run"
        run_rc = 0
        if args.run_main:
            run_total += 1
            print(f"[RUN] {cfg}")
            ok, rc = run_config_with_main(
                cfg=cfg,
                repo_root=repo_root,
                main_py=main_py,
                timeout_sec=args.main_timeout_sec,
                run_log_dir=run_log_dir,
            )
            run_rc = rc
            if ok:
                run_status = "ok"
            else:
                run_status = "failed"
                run_failed += 1
                print(f"  -> run failed (rc={rc})")
                if args.stop_on_run_error:
                    print("Stopped due to --stop-on-run-error")
                    return 4

        raw_fct = parse_config_fct_output_path(cfg)
        if not raw_fct:
            skipped_no_fct += 1
            continue

        fct_file = resolve_fct_path(raw_fct, repo_root, cfg)
        if not fct_file.exists():
            skipped_missing_file += 1
            continue

        fcts_ns, slowdowns, flow_metrics = load_fct_metrics(fct_file)
        if not fcts_ns:
            continue

        sorted_fcts = sorted(float(v) for v in fcts_ns)
        p50_ns = percentile(sorted_fcts, 50.0)
        p90_ns = percentile(sorted_fcts, 90.0)
        avg_slowdown = (sum(slowdowns) / len(slowdowns)) if slowdowns else float("nan")

        cfg_rel = cfg.relative_to(repo_root) if cfg.is_relative_to(repo_root) else cfg
        png_name = sanitize_name(str(cfg_rel.with_suffix(""))) + "-fct-cdf.png"
        plot_cdf(
            fcts_ns,
            title=f"FCT CDF: {cfg_rel}",
            out_png=cdf_dir / png_name,
            plot_unit=args.plot_unit,
        )
        display_name = config_display_name(cfg)
        cdf_series.append((display_name, fcts_ns))

        slowdown_buckets = build_slowdown_buckets(flow_metrics, args.slowdown_bucket_size)
        if slowdown_buckets:
            slowdown_csv_name = sanitize_name(str(cfg_rel.with_suffix(""))) + "-slowdown-vs-size.csv"
            write_slowdown_buckets_csv(slowdown_dir / slowdown_csv_name, slowdown_buckets)
            slowdown_series.append((display_name, slowdown_buckets))

        results.append(
            ConfigResult(
                config_path=cfg,
                fct_file=fct_file,
                num_flows=len(fcts_ns),
                p50_ns=p50_ns,
                p90_ns=p90_ns,
                avg_slowdown=avg_slowdown,
                run_status=run_status,
                run_returncode=run_rc,
            )
        )

    if not results:
        print("No valid FCT data found.")
        print(f"Skipped configs without FCT_OUTPUT_FILE: {skipped_no_fct}")
        print(f"Skipped configs with missing FCT file: {skipped_missing_file}")
        return 2

    merged_cdf_png = cdf_dir / "all-configs-fct-cdf.png"
    plot_multi_cdf(
        cdf_series,
        title="FCT CDF: all configs",
        out_png=merged_cdf_png,
        plot_unit=args.plot_unit,
    )

    if slowdown_series:
        plot_slowdown_comparison_panels(
            slowdown_series,
            out_png=slowdown_dir / "all-configs-slowdown-vs-size.png",
        )

    summary_csv = out_dir / "fct_summary.csv"
    with summary_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "config_file",
                "fct_file",
                "num_flows",
                "p50_fct_ns",
                "p90_fct_ns",
                "avg_slowdown",
                "run_status",
                "run_returncode",
            ]
        )
        for r in sorted(results, key=lambda x: str(x.config_path)):
            writer.writerow(
                [
                    str(r.config_path),
                    str(r.fct_file),
                    r.num_flows,
                    f"{r.p50_ns:.3f}",
                    f"{r.p90_ns:.3f}",
                    f"{r.avg_slowdown:.6f}",
                    r.run_status,
                    r.run_returncode,
                ]
            )

    print(f"Configs scanned: {len(config_files)}")
    print(f"Analyzed configs: {len(results)}")
    print(f"Skipped (no FCT_OUTPUT_FILE): {skipped_no_fct}")
    print(f"Skipped (missing FCT file): {skipped_missing_file}")
    if args.run_main:
        print(f"Run main.py total: {run_total}")
        print(f"Run main.py failed: {run_failed}")
        print(f"Run logs dir: {run_log_dir}")
    print(f"Summary CSV: {summary_csv}")
    print(f"CDF plots dir: {cdf_dir}")
    print(f"Merged CDF plot: {merged_cdf_png}")
    print(f"Slowdown outputs dir: {slowdown_dir}")

    print("\nTopline metrics:")
    print("config_file,num_flows,p50_fct_ns,p90_fct_ns,avg_slowdown,run_status,run_returncode")
    for r in sorted(results, key=lambda x: str(x.config_path)):
        print(
            f"{r.config_path},{r.num_flows},{r.p50_ns:.3f},{r.p90_ns:.3f},{r.avg_slowdown:.6f},{r.run_status},{r.run_returncode}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
