# Phase 4 — Hazards

## What this phase was for

Make the pipeline correct. Three mechanisms, built and verified one at a time,
in the order the plan specified: **forwarding → load-use stall → branch
flush.**

The ordering matters. Once forwarding works, the only remaining data hazard is
load-use, so a failure isolates cleanly. Build the stall first and a failure
could be either mechanism.

Target: back to the 39/42 Phase 2 achieved, but pipelined.

---

## Forwarding

### The problem

```
add x5, x1, x2      # I1
add x6, x5, x3      # I2 -- reads x5
```

I1's result exists at the end of EX in cycle 3. I2 needs it during cycle 4. But
I2 read the register file in cycle 3, before the result existed, so
`id_ex_rs1_data` holds the stale value.

The value is not missing — it is sitting in `ex_mem_alu_result`, one stage
ahead. Forwarding is a wire from there back to the ALU input, and a mux to
select it.

### Where the value lives, by distance

| reader is | when it is in EX, I1 is in | value lives in |
|---|---|---|
| 1 after | MEM | `ex_mem_*` |
| 2 after | WB | `mem_wb_*` / `wb_data` |
| 3 after | retiring that same cycle | regfile bypass |
| 4+ after | done | regfile array |

Three mechanisms for three distances. The third was already built in Phase 1 —
the regfile's write-first bypass — and this phase enabled it by flipping
`.BYPASS(0)` to `.BYPASS(1)`. The combinational loop that forced it off in
Phase 2 is gone, because ID and WB now hold different instructions with
pipeline registers between them.

### What gets forwarded

Not `alu_result`. That is wrong for three instruction types: `jal` and `jalr`
write `pc_plus4`, and CSR reads write `csr_rdata`.

So a wire computes **the value the instruction will eventually write back**:

```verilog
wire [31:0] ex_mem_wb_value = (ex_mem_wb_sel == 2'b11) ? ex_mem_csr_rdata :
                              (ex_mem_wb_sel == 2'b10) ? ex_mem_pc_plus4  :
                                                         ex_mem_alu_result;
```

Note the deliberate absence of a `wb_sel == 01` arm. A load falls through to
`ex_mem_alu_result`, which is the memory *address*, not the loaded data. That is
wrong, and intentionally so — the loaded data does not exist yet. It is the case
forwarding cannot fix, and the stall prevents it ever being used.

From WB, `wb_data` is already the fully-resolved writeback value including
loaded data, so nothing extra is needed.

### The condition

Four wires — two operands × two source stages — each three terms:

> the source stage writes a register **and** its `rd` matches the reader's `rs`
> **and** that register is not `x0`

Identical in shape to the regfile bypass written in Phase 1. The `!= 0` term is
why a `nop` in MEM does not forward its discarded result to an instruction
reading `x0`.

`id_ex_rs1_addr` and `id_ex_rs2_addr` were carried through the pipeline in
Phase 3 specifically for these comparisons.

### Priority: MEM before WB

```
add x5, x1, x2
add x5, x3, x4      # writes x5 again
add x6, x5, x7      # which x5?
```

When the third is in EX, the second is in MEM and the first is in WB. Both
match. MEM holds the more recent write, so MEM wins — checked first in the mux
chain.

Getting this backwards produces a bug that only appears when the same register
is written twice within three instructions, which is common in compiled code
and painful to locate.

### The muxes go at the top of EX

`id_ex_rs1_data` and `id_ex_rs2_data` are read by five things: the `alu_a` mux,
the `alu_b` mux, `branch_cmp`, the store data captured into `ex_mem_rs2_data`,
and `csr_wdata`.

A branch comparing stale values takes the wrong path. A store writing stale
data corrupts memory. Both need forwarding just as much as the ALU does.

So the forwarding is built **once**, as `fwd_rs1` and `fwd_rs2` at the top of
EX, and every use of the raw pipeline register in that stage is replaced. After
the change, `id_ex_rs1_data` and `id_ex_rs2_data` appear in exactly two places
in the file: the ID/EX capture, and the fallback arm of the forwarding muxes.

Missing one of the five is easy. `csr_wdata` was in fact missed on the first
pass and caught in review — and it matters, because riscv-tests' init code does
`auipc t0` / `addi t0` / `csrw mtvec, t0` back to back.

---

## Branch flush

Built second rather than third, out of the planned order, because forwarding
could not be observed until it was: the core derailed on the first `jal` in the
init code before reaching any instruction with a data dependency.

### The shadow

Branches resolve in EX. By the time `pc_redirect` goes high, two more fetches
have already happened.

| cycle | IF | ID | EX |
|---|---|---|---|
| 1 | `jal` | — | — |
| 2 | @ +4 | `jal` | — |
| 3 | @ +8 | @ +4 | **`jal` resolves** |
| 4 | target | @ +8 | @ +4 |

The PC redirects at the end of cycle 3, so cycle 4 fetches correctly. But the
two instructions at +4 and +8 are already in the pipe, and nothing removes
them.

This is not a branch *prediction* problem — `jal` is unconditional, there is
nothing to predict. The shadow exists purely because the redirect is computed
two stages after the fetch. Not-taken branches cost nothing.

### The fix

At the edge where `pc_redirect` is high, IF/ID and ID/EX are cleared rather
than loaded.

**Only those two.** EX/MEM and MEM/WB hold instructions fetched before the
branch, which are legitimately executing. The branch itself is in EX and must
complete.

Clearing means **control signals only** — `reg_we`, `mem_we`, the branch and
jump flags, the CSR enables. Zero those and the instruction becomes a nop
flowing harmlessly through the remaining stages. Data fields do not matter.

The flush signal is `pc_redirect` itself, which already includes `trap`. That
covers traps for free, and it has to: when an instruction faults and the PC
jumps to `mtvec`, the two behind it are equally wrong.

A trapping instruction gets both treatments — its own writes suppressed by
`reg_we_final`/`mem_we_final`, and the two behind it flushed. Two different
mechanisms for "this instruction must not commit" versus "these instructions
must not exist."

### Stale state in the bubble

The first working version flushed correctly but corrupted the PC chain.

The flush branch cleared control signals and left `id_ex_pc` and `id_ex_instr`
holding whatever was there. Harmless for the nop itself, but the stale PC
propagated: `ex_mem_pc` and `mem_wb_pc` carried it into the trace, and a
subsequent branch reading a corrupted `id_ex_pc` computed `pc + imm` from the
wrong base.

The IF/ID flush had a subtler version: it assigned `if_id_pc <= pc`, but during
a flush `pc` is being redirected, so the nop received the *target's* address.

The fix was to give the bubble its own clean state — zero the PC fields, load a
real nop into the instruction field, and clear `rs1_addr`/`rs2_addr`/`rd_addr`
as well. Those last three matter because the forwarding comparators read them
every cycle: a stale `rd_addr` in a bubble could make the next instruction's
forwarding logic see a dependency that does not exist.

---

## The reset bug

This one cost the most time, and it had been present since Phase 3.

### The symptom

After the flush was working, every test still failed identically — same
timeout, same PC, `0x000000c4`. And the PC in the trace lost bit 31 after the
first few instructions: `80000000`, then `00000000`, `00000004`, `00000050`.

The core was running in low memory.

### Finding it

The trace comparison was not enough here, because the divergence was at
instruction 0 and everything after was garbage. GTKWave was.

Signals added: `pc`, `if_id_pc`, `if_id_instr`, `id_ex_instr`, `id_ex_is_jal`,
`flush`, `pc_redirect`, `pc_target`, `ex_mem_pc`, `mem_wb_pc`.

The waveform showed `flush` and `pc_redirect` going high around 30 ns —
**before** `id_ex_is_jal` rose at 60 ns. A redirect with no jump.

### The cause

`if_id_instr` reset to `32'b0`.

Instruction word zero has opcode `0000000`, which the decoder's `default` arm
correctly flags as illegal. That propagated to `id_ex_illegal`, raised `trap` in
EX, raised `pc_redirect`, and jumped the PC to `mtvec_out` — which is zero at
reset.

So the core trapped on its own reset-cleared pipeline slots and jumped to
address 0. Everything after ran in low memory, and `imem`'s address slicing
made that fetch *something*, so it kept going.

### The fix

Reset instruction registers to `32'h00000013` — `addi x0, x0, 0`, a genuine nop
— never zero.

This is worth stating as a rule because it recurs: **a zeroed instruction
register is an illegal instruction, and an illegal instruction with `mtvec` at
zero sends the PC to address 0.** Any register holding an instruction word
should reset to a nop.

---

## Load-use stall

Built last, and the smallest of the three.

### The problem forwarding cannot fix

```
lw  x5, 0(x1)
add x6, x5, x2      # needs x5 immediately
```

| cycle | 3 | 4 |
|---|---|---|
| `lw` | EX (computes address) | **MEM (dmem reading)** |
| `add` | ID | **EX (needs x5 now)** |

In cycle 4 the `add` needs `x5`, but `dmem` is fetching it during that very
cycle. The data does not exist until the end of cycle 4. No wire can forward a
value that has not been read.

The rule this establishes: **forwarding fixes "the answer exists but has not
been filed"; stalling fixes "the answer does not exist yet."**

### Detection

```verilog
wire load_use = id_ex_mem_re
             && (id_ex_rd_addr != 5'b0)
             && ((id_ex_rd_addr == rs1_addr) || (id_ex_rd_addr == rs2_addr));
```

Note this compares against the **ID-stage** `rs1_addr`/`rs2_addr` — the
instruction currently decoding — not `id_ex_rs1_addr`. A different question from
forwarding: "does the instruction being decoded right now need what the load is
fetching?"

`id_ex_mem_re` was carried through Phase 3 for exactly this.

### The response

| register | action |
|---|---|
| `pc` | hold |
| IF/ID | hold |
| ID/EX | bubble |
| EX/MEM, MEM/WB | normal |

The load must keep advancing through MEM to WB or it never completes and the
stall never resolves. The dependent instruction freezes. The gap between them
fills with a bubble.

**Stall upstream, let downstream drain.**

Priority: flush before stall. If a branch redirected, the instruction being
stalled for is on the wrong path and should be discarded rather than held.

---

## Result: 39 / 42

Same three exclusions as Phase 2 — `fence_i`, `ma_data`, `ld_st` — all still
traceable to missing privilege modes, and all unrelated to hazard handling.

The pass pattern is itself the evidence each mechanism works. Every load
passing is the stall. Every branch and jump passing is the flush. Every
dependent-instruction sequence passing is forwarding.

## What this phase established

A correct pipelined RV32I core. Everything after it is peripherals,
verification, and performance.

Also two lessons that recurred:

**`make lint` does not rebuild.** Three rounds of edits produced identical
results because a stale binary was being run. Only `make MODULE=x` recompiles.

**A bug in one mechanism can hide another.** Forwarding appeared broken when it
was not, because the core derailed on the first `jal` before reaching any
dependent instruction. The one-mechanism-at-a-time discipline is what made this
diagnosable — when everything failed identically at the same address, it was
clearly not a data-dependency problem.
