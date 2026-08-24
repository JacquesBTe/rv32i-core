# Phase 7 (plan) — Performance

**Status: not started.** This document describes intended work, not
anything implemented yet. Nothing described here should be read as a
claim about the current state of `core.v` or `soc.v`.

## Starting point

Phase 5's timing section documents the current tradeoff directly: moving
`dmem` behind the AXI4-Lite bus improved post-route WNS (+0.538 ns to
+1.092 ns) at the cost of roughly 40% more cycles to retire the same
riscv-tests programs, since a load or store that used to complete within
the memory stage's own cycle now costs at least one `bus_stall` cycle for
the AXI4-Lite handshake. Separately, phase4.md's branch-flush section
notes every taken branch, jump, `mret`, or trap costs a fixed two-cycle
bubble (`if_id`/`id_ex` both flushed) — there's no prediction at all today;
IF always fetches `pc + 4` and only learns otherwise once EX resolves it.
Any performance work should be measured against both of these, not just
overall clock frequency, since the bus already traded frequency for IPC
once.

## Branch prediction

The cheapest first step is static prediction — backward branches predicted
taken, forward branches predicted not-taken — which needs no additional
state, just fetching `pc + imm` speculatively for a backward branch
instead of always `pc + 4`, and squashing on a misprediction exactly the
way an actual mispredict (today's *only* case) already does. This doesn't
help `jal`/`jalr` (always taken, so "prediction" is really just moving
target computation earlier) or forward branches, so it's a partial fix
sized to this core's simplicity rather than a general BTB.

A 2-bit saturating-counter table indexed by low PC bits would cover more
cases but adds real state (a small BRAM/LUTRAM array) and a new hazard
class of its own — a predictor update path that itself needs to survive
`bus_stall` correctly, given this project's history of exactly that shape
of bug (phase5.md). Worth attempting only after static prediction is
measured and shown to leave meaningful IPC on the table.

## CoreMark

No standard benchmark currently runs on this core — `tests/` is
correctness-only (riscv-tests + unit testbenches). Porting CoreMark would
need:

- **A timing source.** This core has no `mcycle`/`minstret` performance
  counters — `csr.v`'s implemented set is `mstatus`, `mie`, `mip`,
  `mtvec`, `mepc`, `mcause`, `mscratch`, `mhartid` only (phase4.md,
  phase5.md); every other address is either explicitly ignored (WARL) or
  illegal. `rtl/soc/timer.v`'s memory-mapped `mtime` (a free-running
  64-bit counter, already used for the machine timer interrupt) is the
  only cycle-accurate clock source on the bus today and would have to
  stand in for CoreMark's usual `clock()`/cycle-counter hook.
- **A fit inside 64 KB with no heap.** `sw/common/link.ld` gives every
  program a single 64 KB region for `.text`/`.rodata`/`.data`/`.bss`/
  stack combined, and `crt0.S` sets up no heap at all — CoreMark's default
  configuration expects to be buildable with static allocation
  (`MEM_METHOD_STATIC`), which fits this project's existing bare-metal
  model, but the total footprint needs to be checked against the 64 KB
  budget once actually built, not assumed.
- **Iteration count tuned to real hardware run time**, not simulation —
  CoreMark's standard configuration expects on the order of ~10 seconds
  of execution for a stable score; at 75 MHz that's a real number of
  cycles worth sanity-checking against the interconnect's actual observed
  throughput (phase5.md's "~40% more instructions" note) before committing
  to an iteration count.

## Caches

The core is still Harvard-split at the top level — `imem` and `dmem`
never shared a path — but only `dmem` sits behind the bus now; `imem` is
still a direct synchronous BRAM read with no transaction overhead. That
asymmetry means a cache is likely to pay off first, and most, in front of
`axil_master` on the `dmem` side: a small direct-mapped cache absorbing
repeat accesses to the same word (loop-local variables, a stack frame
being spilled and reloaded) would remove `bus_stall` cycles exactly where
Phase 5 introduced them, without touching the interconnect, the
peripherals, or the AXI4-Lite protocol itself. `imem` gains comparatively
little from a cache of its own, since it's not on the bus and isn't
subject to `bus_stall` today — that only becomes a reasonable target if a
future decision puts instruction fetch behind the bus too (e.g. to unify
the address space and close the `fence_i` gap noted in phase5.md), which
is a bigger and separate architectural change.

## Custom instruction

`rtl/include/rv32i_defs.vh`'s ALU opcode space is 4 bits and only 10 of
the 16 possible codes are assigned (`ALU_ADD` `0000`, `SLL` `0001`, `SLT`
`0010`, `SLTU` `0011`, `XOR` `0100`, `SRL` `0101`, `OR` `0110`, `AND`
`0111`, `SUB` `1000`, `SRA` `1101`) — there's room to add an ALU operation
without widening the encoding. The more open question is which
instruction actually justifies one: RV32I's own custom-opcode space
(`0001011`, `0101011`, `1011011`, `1111011` — the four opcodes the base
spec reserves and never defines) would need `decoder.v` to route a fifth
case in its main opcode `case`, wired the same way `OP`/`OP-IMM` already
are. What operation belongs there isn't decided by anything in this repo
today; picking one (a CRC step, a population count, something
CoreMark-adjacent once that's running) should follow from where profiling
actually shows time going, once there's a benchmark (see above) to profile
in the first place, rather than being chosen speculatively here.
