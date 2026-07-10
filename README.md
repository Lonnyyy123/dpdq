# DPDQ

This repository contains the implementation and evaluation artifacts for DPDQ,
a receiver-driven transport protocol for datacenters based on dual queues.

## Repository Layout

| Directory | Description |
| --- | --- |
| `dpdq-ns2/` | ns-2 based simulator and experiment scripts for DPDQ and related transport protocols, with DPDQ-specific protocol, queueing, experiment, and analysis extensions. |
| `dpdq-ns3/` | ns-3 based RDMA simulation and evaluation artifacts, including experiment configurations, workloads, analysis scripts, and reproducibility helpers. |
| `tofino/` | Intel Tofino 1 TNA/P4_16 implementation of the DPDQ switch-side logic, including the P4 pipeline and target-specific setup notes. |
