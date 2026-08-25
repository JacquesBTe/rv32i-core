# Phase 0 — Toolchain and trace harness

## What this phase was for

Nothing in this phase is part of the CPU. It exists so that later phases have
a way to tell whether the CPU is correct, and a way to get compiled code into
it. Every phase after this one depends on the reference-comparison flow built
here.

The principle: before writing a line of RTL, be able to answer "is this right?"
mechanically rather than by inspection.

## Toolchain

**Host:** Windows with WSL2 Ubuntu 24.04. The repository lives on the WSL
filesystem at `~/rv32i-core`. Vivado runs on the Windows side and is not used
until Phase 5.

**Compiler:** xPack `riscv-none-elf-gcc` 15.2.0, prefix `riscv-none-elf-`,
built for `rv32i_zicsr` with the `ilp32` ABI.

The obvious choice, the riscv-collab prebuilt toolchain was tried first and
rejected. Its `libgcc` contains compressed instructions, which this core does
not implement. Linking against it would have produced binaries that fault on
instructions the core is not supposed to support, and the failure would have
looked like a decoder bug rather than a toolchain problem. The xPack build was
verified to have a clean `rv32i`/`ilp32` multilib before being adopted.

**Reference model:** Spike, built from source at `~/tools/spike`. Spike is the
official RISC-V ISA simulator and serves as the golden model for every
correctness check from Phase 2 onward.

**Simulation and synthesis:** OSS CAD Suite 2026-03-08 — Verilator 5.047,
Yosys, SymbiYosys, GTKWave. Loaded into a shell by the `eda` name.

Worth noting for anyone reproducing this: `eda` is a shell name, not a
`PATH` modification, so it does not propagate into `make` subshells or new
terminals. This causes `verilator: command not found` in exactly the situations
where it is least expected.

**Submodules:** `tests/riscv-tests` (the official ISA test suite) and
`formal/riscv-formal` (for Phase 6).

## Software build flow

`sw/` holds everything needed to turn C into something the core can execute.

- **`common/link.ld`** — places the program at `0x80000000` in a 64 KB region,
  with a `.tohost` section for the HTIF exit protocol.
- **`common/crt0.S`** — the C runtime startup: sets up `sp` and `gp`, clears
  `.bss`, calls `main`, and exits by writing `(code << 1) | 1` to `tohost`.
- **`common/makehex.py`** — converts a raw binary to one 32-bit
  little-endian hex word per line, which is the format `$readmemh` expects.
- **`Makefile`** — `CROSS_COMPILE ?= riscv-none-elf-`, drives
  C → ELF → disassembly → binary → hex.

The disassembly step is not decorative. Reading the `.dis` file is how
instruction encodings were verified by hand in Phase 1, and how the first
divergences were understood in Phase 2.

The reference run for any ELF is:

```
spike --isa=rv32i_zicsr -m0x80000000:0x10000 --log-commits <elf> 2> log
```

## The trace harness

`tests/trace_diff.py` is the piece that makes every later phase debuggable.

It parses Spike's commit log and a CSV emitted by the core, aligns them, and
reports the first point at which they disagree, with five instructions of
context on either side.

**Core CSV format**, one retired instruction per line, hex without `0x`:

```
pc,instr,rd,wdata        80000010,00100093,1,00000001
pc,instr,-,-             for instructions with no register writeback
```

Three details in the parser that matter:

- **Writes to `x0` are dropped.** Spike does not log them, and the core
  executes them constantly — every `nop` is `addi x0, x0, 0`, every
  unconditional jump is `jal x0`, every return is `jalr x0`.
- **PCs below `0x80000000` are skipped.** Spike runs a small boot ROM at
  `0x1000` that sets up `a0`/`a1` and jumps to the program; the core starts
  directly at `0x80000000`.
- **Instructions that trap are omitted** (added in Phase 2, once traps
  existed). Spike does not log a trapped instruction; if the core logs one,
  every subsequent line is offset by one and the diff reports a divergence
  that is really just misalignment.

**Validation before use.** The harness was tested two ways before being
trusted: Spike against itself (4,995 instructions, PASS), and Spike against a
deliberately mutated copy of its own log (correctly reported the divergence and
exited non-zero). A comparison tool that cannot detect a difference is worse
than no tool, because it produces false confidence.

## Repository layout established here

```
rv32i-core/
  rtl/{core,soc,include}
  sim/{tb,verilator,waves}
  tests/          trace_diff.py, riscv-tests submodule
  sw/{common,examples}
  formal/         riscv-formal submodule
  fpga/
  docs/
```

## What Phase 0 produced

- A verified compiler that emits only instructions the core will implement
- A reference simulator to check against
- A build flow from C to a hex image
- A trace comparison tool, itself validated
- A repository structure

None of it executes an instruction. All of it is why Phases 2 through 5 were
tractable.
