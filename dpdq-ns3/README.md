# DPDQ ns-3 Simulator

This repository contains the ns-3 simulation code for DPDQ. It is built on top of the TLT RDMA simulator codebase and extends it with the mechanisms and experiment scripts used in this branch.

The upstream TLT RDMA repository is available at:

- `https://github.com/kaist-ina/ns3-tlt-rdma-public`

## 1. Repository Overview

- Base codebase: TLT RDMA ns-3 simulator.
- Main purpose of this branch: simulate DPDQ-related transport and switch behavior in ns-3.
- Primary experiment entry point in this branch: `run-exp5-single.sh`.
- Main output locations:
  - `mix/`: simulation outputs
  - `logs/`: run logs
  - `config/generated-exp5/`: generated runtime configs
  - `analysis/output/`: analysis figures and summary tables

## 2. Environment and Docker

The recommended environment is Docker. The build helper script uses:

- Ubuntu 16.04 container image
- `gcc-5` / `g++-5`
- `python`
- `make`, `cmake`, `pkg-config`, `libc6-dev`

Runtime and analysis on the host side typically need:

- Docker
- Bash
- Python 3
- `matplotlib` for plotting analysis results

If Docker access is restricted on your machine, either add your user to the Docker group or run the build command with `sudo`.

## 3. Build

From the repository root:

```bash
./docker/dev-build.sh -j"$(nproc)"
```

Build artifacts are written to `build/`. In particular, the `exp5` launcher expects:

```bash
build/scratch/exp5
```

## 4. Run `exp5`

The main single-run entry point is:

```bash
./run-exp5-single.sh
```

Current configs bundled in this branch support these `exp5` algorithm modes:

- `dpdq-ccfc`
- `pfc-dcqcn`
- `pfc-hpcc`

Supported workloads in the script are:

- `hadoop`
- `websearch`

Example commands:

```bash
./run-exp5-single.sh --algo dpdq-ccfc --workload hadoop --load 0.60
```

```bash
./run-exp5-single.sh --algo pfc-dcqcn --workload websearch --load 0.60
```

```bash
./run-exp5-single.sh --algo pfc-hpcc --workload hadoop --load 0.60
```


### Workflow

The typical workflow in this branch is:

1. Start from one of the base configs under `config/`.
2. Run `run-exp5-single.sh`, which generates a runtime config under `config/generated-exp5/`.
3. The launcher then calls `dev-run-local.sh` and `main.py` to prepare the final simulator input.
4. The ns-3 executable built from `scratch/exp5.cc` runs and writes outputs under `mix/exp5-.../` and logs under `logs/`.
5. Post-process the results with `analysis/postprocess_slowdown_table.py`.
