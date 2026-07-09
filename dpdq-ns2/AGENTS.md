# AGENTS.md

This file captures the working context for the `sird` repository so future agents can continue development without rebuilding the same project knowledge from scratch.

## Scope

Repository: `/home/lonnie/repos/sird`

Main active workstreams:
- `sird-ls`: SIRD on the ns-2 leaf-spine setup.
- `ppass-ls`: DPDQ / credit-bounce / PhantomPass-style extensions on the same simulator base.

Primary simulator in current work:
- `ns2.34/ns-2.34`
- Front-end scripts under `scripts/r2p2/coord/` and `scripts/r2p2/sim-scripts/r2p2cc/`

## High-Level Project Model

This repo is an ns-2 based datacenter transport simulator with multiple transports integrated into the `r2p2` code path.

Implemented / relevant protocols:
- SIRD
- DPDQ / CreditBouncer-style flow control work
- DCTCP
- Homa
- pFabric
- XPass / ExpressPass-related queueing and routing code

Current protocol mapping in code:
- SIRD logic is implemented in the `r2p2` transport path, mainly in `R2p2CCHybrid` and related support classes.
- DPDQ work is an extension on top of that path, not a separate transport stack.

## Current Research Context

The active line of work has two related goals:
1. Maintain `sird-ls` as the SIRD baseline on ns-2.
2. Extend `ppass-ls` / related branches with DPDQ behavior, especially:
   - credit bounce
   - switch-side flow control
   - phantom queue based control
   - resend / retransmission support inspired by Homa's receiver-driven resend model

Important semantic distinction:
- SIRD baseline: receiver-driven crediting with sender-mark and ECN feedback.
- DPDQ extension: adds switch-side `PAUSE` / `NORMAL` port state and bounced credits.

## Important Branch Conventions

Use these branch roles unless explicitly changed:
- `sird-ls`: SIRD baseline and SIRD-specific modifications.
- `ppass-ls`: DPDQ / PhantomPass / credit-bounce branch.

Do not assume a feature belongs on both branches.
When porting functionality between branches, first identify whether the behavior is:
- transport semantic
- switch/classifier semantic
- experiment/config-only

## Key Source Files

### Transport / SIRD / resend logic
- `ns2.34/ns-2.34/apps/r2p2-cc/r2p2/cc/micro-rpcs/r2p2-cc-hybrid.cc`
- `ns2.34/ns-2.34/apps/r2p2-cc/r2p2/cc/micro-rpcs/r2p2-cc-micro.h`
- `ns2.34/ns-2.34/apps/r2p2-cc/r2p2/cc/micro-rpcs/r2p2-hybrid-support.{h,cc}`
- `ns2.34/ns-2.34/apps/r2p2-cc/r2p2/core/r2p2-hdr.h`
- `ns2.34/ns-2.34/apps/r2p2-cc/r2p2/core/r2p2.cc`
- `ns2.34/ns-2.34/apps/r2p2-cc/r2p2/core/r2p2-server.cc`
- `ns2.34/ns-2.34/apps/r2p2-cc/r2p2/app/r2p2-app.cc`

### Switch / classifier / queue logic
- `ns2.34/ns-2.34/classifier/classifier-hash.{h,cc}`
- `ns2.34/ns-2.34/classifier/classifier-mpath.cc`
- `ns2.34/ns-2.34/queue/ppass-queue.{h,cc}`
- `ns2.34/ns-2.34/queue/drop-tail.cc`
- `ns2.34/ns-2.34/common/net-interface.{h,cc}`

### Experiment front-end
- `scripts/r2p2/coord/run`
- `scripts/r2p2/coord/util.sh`
- `scripts/r2p2/coord/config/common.sh`
- `scripts/r2p2/sim-scripts/r2p2cc/simulation.tcl`
- `scripts/r2p2/coord/config/examples/*.sh`

### Post-processing
- `scripts/r2p2/post-process/*`
- `scripts/r2p2/coord/results/*`

## How Simulations Are Run

Always run from:
- `scripts/r2p2/coord/`

Typical command:
```bash
./run ./config/examples/<config>.sh 1 1 1 1 0 &> out.txt
```

For long runs:
```bash
nohup ./run ./config/examples/<config>.sh 1 1 1 1 0 &> out.txt &
```

Important note:
- `coord/run` is the real entry point for experiment execution.
- `simulation.tcl` is the main TCL scenario builder.
- Config files define parameter lists using `*_l` variables.

## Build Flow

Root install helper:
```bash
./setup.sh
```

Common manual build flow:
```bash
cd ns2.34/ns-2.34
./configure --enable-debug
make clean && make -j
```

Practical note:
- `./configure --enable-debug` enables assertions.
- If assertions are undesirable for a run, use a build without that flag.
- Source edits in `ns2.34/ns-2.34` usually require `make` again.

## Current SIRD Semantics in This Repo

The SIRD host logic is implemented in the `r2p2` hybrid CC path.

Relevant behaviors already discussed and/or implemented in this repo history:
- receiver-driven grants
- unsolicited / unscheduled / scheduled message split
- sender-mark based AIMD signal
- ECN-based AIMD signal
- resend-based recovery inspired by Homa

Important resend design direction:
- receiver detects missing data and issues `RESEND`
- no `PING` / `UNKNOWN` message-level recovery is required
- resend is local repair, not full message restart
- `BUSY` support was explicitly deemed unnecessary for this project line

## Current DPDQ / credit bounce semantics

DPDQ flow control adds switch-side port states:
- `NORMAL`
- `PAUSE`

When active, the switch may bounce credit packets back toward the receiver instead of forwarding them toward the sender.

Important concepts:
- phantom queue is per-port, not per-subqueue
- physical egress queue accounting may be split from phantom queue accounting
- bounced credit must revoke previously issued receiver-side reservation state
- symmetric routing is required so scheduled data can follow the reverse path of the corresponding credit where needed

## Routing / symmetry notes

There has been repeated work around symmetric routing and packet spray.
Key code paths:
- `classifier-mpath.cc`
- `classifier-hash.cc`
- `r2p2-hdr.h` fields used to preserve path-related information

Known conceptual rule:
- credit packet path choice and corresponding data return path must remain consistent when the experiment requires symmetric routing.

## Queueing notes

There are multiple queue types in this repo.
Important current understanding:
- `XPassDropTail` is a dual-queue design: control traffic and data traffic are separated.
- Existing DPDQ work has previously used ordinary DropTail / RED-based egress queues plus classifier-side logic.
- A next-step direction is to introduce a dedicated DPDQ queue with:
  - one control queue for `GRANT`, `GRANT_REQ`, `RESEND`, and similar control packets
  - one data queue for ordinary request/data packets
  - strict priority for control queue
  - separate configurable sizes for the two physical queues
  - egress occupancy reported as the sum of both physical queues
  - phantom queue remaining per-port rather than split per-subqueue

## Existing experiment configs worth knowing

SIRD leaf-spine:
- `scripts/r2p2/coord/config/examples/leaf-spine-sird-w3-0.75coreload.sh`
- `scripts/r2p2/coord/config/examples/leaf-spine-sird-w4-0.75coreload.sh`

DPDQ / resend smoke:
- `scripts/r2p2/coord/config/examples/dpdq-resend-smoke.sh`

Other useful examples:
- `scripts/r2p2/coord/config/examples/simple-incast.sh`
- `scripts/r2p2/coord/config/examples/congested-sender.sh`
- `scripts/r2p2/coord/config/examples/generated-traffic.sh`
- `scripts/r2p2/coord/config/examples/dctcp-incast.sh`

## Result files to inspect

Raw outputs:
- `applications_trace.str`: flow/message start and completion events
- `packet_trace.tr`: full packet-level trace when enabled
- `qmon/`: queue monitor raw outputs
- `cc_trace.str`: transport/CC internal trace when enabled

Processed outputs:
- `output/app.csv`
- `output/q.csv`
- `output/qlink.csv`
- `output/qts/...`
- `output/app/CDFs/...`

Recent useful files for switch-side debugging:
- `*_real_qlen.csv`
- `dpdq_switch_events.csv`

## Known pitfalls

- Always launch experiments from `scripts/r2p2/coord/`.
- The `*_l` parameter handling is fragile; if a variable is referenced by `util.sh` but not defined in config/common, runs fail with “Variable ... has not been set”.
- The post-processing scripts assume specific qmon column formats; changing queue monitor output shape can break aggregation.
- Some queue and classifier changes require matching TCL wiring in `simulation.tcl`, not just C++ changes.
- Header changes in `r2p2-hdr.h` must preserve copy semantics; explicit copy/assignment can matter for simulator stability.
- Remote/push setup may differ per branch; always confirm `git remote -v` and branch upstream before pushing.

## When editing protocol logic

Before changing behavior, verify which layer owns the state:
- receiver transport state
- sender transport state
- switch classifier state
- physical queue state
- phantom queue state
- TCL wiring / experiment parameter only

For DPDQ work specifically, avoid mixing these concepts accidentally:
- real egress queue occupancy
- phantom queue occupancy
- credit granted statistics
- credit replenished / revoked statistics
- sender available credit
- receiver outstanding granted bytes

## Recommended next-step checklist for future agents

When continuing DPDQ queue work on `ppass-ls`:
1. Inspect `XPassDropTail` dual-queue structure.
2. Add a dedicated DPDQ queue class rather than overloading plain DropTail.
3. Route control packets to the control subqueue.
4. Route data packets to the data subqueue.
5. Make scheduler strict-priority in favor of control traffic.
6. Expose independent queue size parameters for both subqueues.
7. Make switch-side egress occupancy use the sum of both subqueues.
8. Keep phantom queue per-port, not per-subqueue.
9. Update `simulation.tcl` and `common.sh`/`util.sh` together.
10. Re-run a small smoke config before large leaf-spine experiments.

## Minimal branch sanity checks

Before coding:
```bash
git branch --show-current
git status --short
git remote -v
```

Before trusting a run:
```bash
cd scripts/r2p2/coord
bash -n ./config/examples/<config>.sh
./run ./config/examples/<config>.sh 1 0 0 0 0
```

Before pushing:
```bash
git log --oneline --decorate -n 10
git remote -v
git status --short
```

