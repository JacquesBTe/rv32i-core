# Phase 6 (plan) — Formal verification with riscv-formal

**Status: not started.** This document describes intended work, not
anything implemented yet. Nothing described here should be read as a
claim about the current state of `core.v` or `soc.v`.

## Why

`tests/run_tests.sh` and the unit testbenches are all example-based:
riscv-tests exercises the instructions its authors thought to write down,
and the randomized unit tests explore each module's own input space, not
the pipeline's cross-instruction interactions. Both are strong at catching
regressions in behavior someone already thought to check, and neither is
built to find a hazard-interaction bug that nobody wrote a directed test
for — which is exactly the shape of every real bug this project has found
so far (the `id_ex_mem_re` bubble, the `if_id_pc`/`id_ex_pc` flush bugs
documented in phase5.md, all three surfaced by a specific runtime
coincidence rather than a specific test case). A bounded model checker
explores every reachable state up to some depth instead of the states a
test author enumerated, which is a materially different kind of coverage
for exactly this class of bug.

`formal/riscv-formal` is already present as a git submodule
(`YosysHQ/riscv-formal`, pinned at `c992aa6`) but nothing in this repo
references it yet — there is no `cores/rv32i-core/` directory under it,
and no build target that invokes it.

## What riscv-formal actually needs

riscv-formal checks a core against the RISC-V Formal Interface (RVFI): a
per-retired-instruction trace bus (`rvfi_valid`, `rvfi_order`, `rvfi_insn`,
`rvfi_pc_rdata`/`rvfi_pc_wdata`, `rvfi_rd_addr`/`rvfi_rd_wdata`,
`rvfi_mem_*`, and more) that a symbolic model checker (via SymbiYosys,
already part of the OSS CAD Suite toolchain this project uses per
phase0.md) can assert properties against — that every retired instruction
had a causally-justified reason to retire, that register/memory writes
match the ISA's defined semantics for the retired instruction, and so on.
Integrating a new core, based on the `nerv` example already vendored under
`formal/riscv-formal/cores/nerv/`, needs two things:

- **A `cores/rv32i-core/checks.cfg`** — declares the ISA string
  (`rv32i_zicsr`, matching every other tool in this project's flow), which
  RVFI channel(s) to check, per-check search depths, and which CSRs exist
  and what update discipline each one follows (`mcycle`/`minstret` are
  free-running counters in riscv-formal's vocabulary, `mstatus` is
  general-purpose state, etc.) — this core implements a real subset of
  `mstatus` (MIE/MPIE/MPP) plus `mtvec`/`mepc`/`mcause`/`mscratch`/`mie`/
  `mip`/`mhartid`, and would need entries reflecting exactly that set, not
  the fuller CSR file `nerv`'s example config documents.
- **A `cores/rv32i-core/wrapper.sv`** — adapts this core's actual
  interface to the RVFI signal set. `core.v` already exposes a
  single-retirement-per-cycle trace bus of its own
  (`trace_pc`/`trace_instr`/`trace_rd`/`trace_wdata`/`trace_we`/
  `trace_mem_addr`/`trace_mem_wdata`/`trace_mem_we`/`trace_trap`, built
  originally for the Phase 0 `trace_diff.py` harness) — the wrapper's job
  is largely translating those existing signals into their RVFI-shaped
  equivalents, plus adding whatever RVFI needs that the trace bus doesn't
  already carry (`rvfi_order`, a monotonically increasing per-retirement
  counter; `rvfi_trap` conditions split by cause the way RVFI expects,
  rather than this project's single `trace_trap` bit).

## Scope for a first pass

Given the core's current state (no supervisor mode, one interrupt source,
`bresp`/`rresp` unused — see phase5.md's remaining gaps), a first
integration should target:

- **Instruction-level correctness** — the `insn` check category, which
  verifies each retired instruction's register/PC effects match the RVFI
  reference model's for that opcode. This is the check most likely to
  independently confirm what riscv-tests already covers, and cheapest to
  stand up first as a sanity check that the wrapper itself is correct
  before trusting deeper checks built on top of it.
- **Causal/`pc_fwd`/`pc_bwd`** — that every retirement's PC follows
  causally from the previous one (sequential, or a taken branch/jump/trap/
  `mret` target), which is precisely the class of bug this project has
  actually hit three times in Phase 5. This is the highest-value check
  given this project's actual bug history, not a generic checklist item.
- **`csr_ill`** — that every CSR address this core treats as illegal
  really is illegal per the configured CSR list, and vice versa; cheap to
  configure now that `csr.v`'s `csr_ignored` list is stable (phase4.md,
  phase5.md).

Deliberately **not** in scope for a first pass: the `bus_*` checks (this
core's AXI4-Lite master isn't the kind of pluggable memory interface those
checks assume without more wrapper work), and the interrupt-liveness
checks (`NERV_FAIRNESS`-equivalent), since this core's one interrupt
source and Phase 5's already-hardware-confirmed fix are lower priority
than getting basic instruction-level checks running at all.

## Open questions

- Whether `bus_stall`-driven multi-cycle instructions (every load/store
  now takes more than one cycle to retire, per phase5.md's timing
  section) need special handling in the wrapper, or whether presenting
  `rvfi_valid` only on the cycle an instruction actually leaves the
  pipeline is sufficient — RVFI is defined around one-valid-cycle-per-
  retirement, which this core already produces via `trace_we`/`trace_trap`
  gating, but the multi-cycle MEM stage hasn't been checked against RVFI's
  assumptions specifically.
- What check depths are actually affordable — `nerv`'s example config
  runs some categories to depth 10, which may or may not be tractable for
  a 5-stage pipeline with a variable-latency memory stage; this needs to
  be found empirically once a wrapper exists, not guessed here.
