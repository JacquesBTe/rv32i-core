# Phase 2 — Single-cycle core

## What this phase was for

Wire the six Phase 1 modules into a CPU that actually executes programs, and
prove it against the official RISC-V test suite.

The hard checkpoint: all `rv32ui-p-*` tests pass. Meeting it means the RV32I
implementation is *correct*, and every phase after this one is about making it
faster without breaking that.

## What was added

**`rtl/core/core.v`** — the top level. Instantiates every module, adds the PC
register, the operand muxes, the writeback mux, and the next-address logic.
One clock edge per instruction; everything between edges is combinational
settling.

**`rtl/core/branch_cmp.v`** — the comparator for the six branch types.

**`rtl/core/csr.v`** — control and status registers. Not originally planned for
this phase; see below.

## Structural decisions

### `branch_cmp` separate from the ALU

The tempting alternative is a `zero` flag output on the ALU, which `beq` could
use.

Rejected for two reasons. RV32I has six branch types needing equality plus
signed and unsigned comparison — a single zero flag covers two of them. And
branch comparison has different timing requirements than the ALU: Phase 3 might
want branches resolved in ID rather than EX to halve the misprediction penalty,
which is only possible if the comparator is a separate module that can be
relocated.

`branch_cmp` takes `rs1_data` and `rs2_data` directly, bypassing the ALU
entirely. It has the same `$signed()` trap as the ALU — casts on `blt`/`bge`,
none on `bltu`/`bgeu`. And `bge` is `>=`, not `>`; RV32I has no `bgt`, since
the assembler synthesises it by swapping operands.

### `branch_taken` must be gated

`branch_cmp` runs combinationally every cycle on whatever is on its inputs. It
does not know what instruction is executing. During an `add`, `funct3` is `000`
which it reads as `beq`, and if the operands happen to be equal it asserts
`taken`.

So the redirect condition is `is_branch && branch_taken`, never `branch_taken`
alone. The same pattern as `mem_we` for `dmem` and `reg_we` for the regfile:
the module computes unconditionally and a decoder signal decides whether anyone
cares.

### PC next-address logic

Three redirect sources plus the sequential default, with priority:

```
trap → mtvec        (outranks everything)
mret → mepc
jalr → alu_result & ~1
else → pc + imm     (branches and jal)
```

Trap first, so a faulting `jalr` goes to `mtvec` rather than its own computed
target.

Note the asymmetry: branches and `jal` compute their target from the **PC**, so
`core.v` does that addition. `jalr` computes from a **register**, and the ALU
already did it — `alu_src` selects the immediate, and `pc_target` takes
`alu_result`.

`jalr` masks bit 0 of the target. Spec-mandated, one line, easy to miss.
Branches and `jal` do not need it because their immediates have bit 0 hardwired
to zero in the encoding.

### `alu_a` is a 3-way mux

`rs1_data` normally, `32'b0` for `lui`, `pc` for `auipc`. That makes both
instructions plain ADD rather than requiring new ALU operations: `lui` is
`0 + imm`, `auipc` is `PC + imm`.

The two control bits are mutually exclusive by construction — an instruction
cannot be both — so the nested ternary's priority ordering is irrelevant.

### The writeback mux has three sources

ALU result, memory data, or PC+4. That last one is why `jal` and `jalr` work:
they save a return address that comes from neither the ALU nor memory.

---

## The combinational loop

The first real problem. Verilator reported `UNOPTFLAT`:

```
alu_result → wb_data → regfile.rd_data → (bypass) → rs1_data → alu_a → alu_result
```

The regfile's write-first bypass creates a path from `rd_data` straight to
`rs1_data`. In `core.v`, `rd_data` is `wb_data`, which for R-type is
`alu_result`. So the ALU's output can reach its own input with no register in
between.

**This was real, not a false positive.** In a single-cycle core the instruction
writing `rd` and the instruction reading `rs1` are the *same instruction*, so
the bypass should never fire — but it fires whenever `rd_addr` equals
`rs1_addr`, which is common (`add x5, x5, x1`). Then `rs1_data` becomes the ALU
result rather than the old `x5`, and the ALU recomputes on its own output. The
loop settles to something, but not necessarily the right thing, and the result
is timing-dependent.

The bypass was designed for the *pipelined* core, where WB and ID hold
different instructions. In single-cycle it is actively harmful.

**Fix:** a `BYPASS` parameter on the regfile, instantiated as `.BYPASS(0)` here
and flipped to 1 in Phase 4 when the pipeline registers made it meaningful
again. The regfile's own testbench keeps using the default of 1, so the bypass
stays verified.

---

## CSRs, pulled forward from Phase 5

The plan had CSRs in Phase 5. They turned out to be required here, and the
discovery is worth recording in full because it was not obvious.

### The symptom

The first `rv32ui-p-add` run diverged at instruction 32 and then hung. The
trace comparison pointed at:

```
ref:  pc=0x800000cc  instr=0xf1402573  x10=0x00000000
dut:  pc=0x800000cc  instr=0xf1402573  --
```

`0xf1402573` is `csrr a0, mhartid`. Spike writes 0 to `x10`; the core treated
SYSTEM as a nop and wrote nothing.

Harmless in itself — `x10` was already zero. But five instructions later:

```
ref:  ...dc (csrw mtvec, t0)  →  ...e4
dut:  ...dc (csrw mtvec, t0)  →  ...e0   (0x74445073)
```

**Spike skipped `0x800000e0` entirely.**

### What the test is actually doing

`0x74445073` is `csrwi mnstatus, 8` — a write to CSR `0x744`, which Spike does
not implement. Spike traps, and `mtvec` had been set two instructions earlier to
point at `0x800000e4` — the instruction *after* the one that faults.

The riscv-tests init code is **deliberately probing** what the implementation
supports, using the trap mechanism as the answer. If the CSR exists, execution
continues; if it does not, the trap skips past.

So `rv32ui-p-*` tests are not pure RV32I. They require machine-mode CSRs and a
trap mechanism just to reach the test body.

### What was implemented

`csr.v` with six real registers: `mstatus` (0x300), `mtvec` (0x305),
`mscratch` (0x340), `mepc` (0x341), `mcause` (0x342), and `mhartid` (0xF14,
reading zero and ignoring writes).

Three access modes — read-write, read-set, read-clear — returning the pre-write
value, which is what the instruction writes back to `rd`.

Two enable rules from the spec that are easy to get wrong and matter for the
trap probing:

- `csrrs`/`csrrc` with `rs1 == x0` is a pure read and **must not write**.
  Setting zero bits would be a no-op, but a write can have side effects.
- `csrrw` with `rd == x0` need not read.

Trap entry writes `mepc` and `mcause` directly, bypassing the instruction path,
and takes priority over any instruction-side write — the faulting instruction
must not also commit its own CSR update.

### The `csr_ignored` list

After implementing the six registers, the core hung in an infinite loop at
`0x80000114`.

That address is `csrwi mie, 0`. `mie` is `0x304`, unimplemented, so it trapped
— and `mtvec` at that point pointed at `0x80000114` itself. Trap, jump to self,
trap forever.

The init code sets `mtvec` to point *past* each CSR it expects might fault, but
only for the ones it is deliberately probing. `mie` is not one of those; the
code assumes it exists.

So a set of CSRs are recognised but not implemented — writes accepted and
dropped, reads return zero. That is legal WARL behaviour:

```
misa 0x301, medeleg 0x302, mideleg 0x303, mie 0x304,
mtval 0x343, mip 0x344, satp 0x180, pmpcfg0 0x3A0, pmpaddr0 0x3B0
```

`mnstatus` (0x744) is deliberately **not** in that list. Spike traps on it too,
and the test relies on that trap.

`csr_illegal` is gated on `csr_re || csr_we`. Every instruction presents
`instr[31:20]` on `csr_addr` — for an ordinary `addi` that is whatever the
immediate happens to be — so without the gate, any instruction with an awkward
immediate would fault.

---

## Trace alignment

One more harness change was needed. **Spike does not log instructions that
trap.** The core did, so after the first trap every subsequent line was offset
by one and `trace_diff.py` reported a divergence that was really just
misalignment.

`core.v` gained a `trace_trap` output, and the testbench skips the CSV line
when it is high.

`trace_we` is also gated on `rd_addr != 0`, because Spike does not log writes to
`x0` and the core executes them constantly.

## Test harness

`tb_core.cpp` runs a program and watches for the HTIF exit: a store to
`tohost` (`0x80001000` for these binaries) with a non-zero value. Value 1 means
pass; anything else is `(test_number << 1) | 1`.

This needed three trace outputs on `core.v` — `trace_mem_addr`,
`trace_mem_wdata`, `trace_mem_we` — so the testbench can see the store.

`tests/run_tests.sh` sweeps every `rv32ui-p-*` ELF: `objcopy` to binary,
`makehex.py` to hex, run with `+imem=` and `+dmem=` plusargs, grep for
`PASS via tohost`.

**Both memories get the same image.** The tests place data after the code in
the same 64 KB region, and both memories ignore the upper address bits, so
loading the same hex into each works. Discovered when every load test failed
with an empty `dmem`.

---

## Result: 39 / 42

All arithmetic, logic, shift, comparison, branch, jump, load, store, `lui` and
`auipc` tests pass.

Three failures, all traceable to the same gap — **no privilege modes**:

| test | why |
|---|---|
| `ma_data` | misaligned accesses need detection and traps |
| `fence_i` | needs `fence.i` semantics and a writable `imem` |
| `ld_st` | U-mode `ecall` — the core hardcodes `mcause = 11` (M-mode); Spike reports 8 |

The `ld_st` failure is precise: the test performs an `ecall` from user mode and
checks `mcause`. Both 8 and 11 are correct answers *for their privilege mode*.
Reporting 11 is correct for a machine-mode-only implementation; the test
exercises U-mode, which this core did not support.

Two of these were closed in Phase 5. `fence_i` remains and is genuinely out of
scope.

## What this phase established

A working RV32I CPU, and — more importantly — the debugging method used for
everything after: run a test, diff against Spike, look at the first divergence.
Not waveforms. The trace comparison names the instruction, the PC, and both
answers, which turns "the core is broken" into "instruction 47 wrote the wrong
value to `x5`."
