# Phase 2 — Single-cycle core (complete)

## What was built

One commit, `0f0afbb`, 2026-08-11 ("single-cycle core executes R-type and
jal"). This is where the placeholder files from Phase 1 got real content:
`alu.v`, `decoder.v`, `dmem.v`, `imem.v`, `immgen.v` were all empty at the
end of Phase 1 and are written here, along with two brand-new files,
`branch_cmp.v` and `core.v`, and a rewrite of `rtl/include/rv32i_defs.vh`
from a leftover placeholder into the ALU opcode macros (`` `ALU_ADD ``
etc.) it still uses today.

`core.v` at this point is genuinely single-cycle: `imem` is read
combinationally off `pc`, `decoder`/`immgen`/`regfile` all read that same
instruction in the same cycle, `alu`/`branch_cmp` compute off those decoded
fields, `dmem` is read and written combinationally off the ALU result, and
the writeback mux feeds `regfile`'s write port — all in one clock period,
with only `pc` itself as a register. `regfile` is instantiated as
`#(.BYPASS(0))`, with a comment reading "flip to 1 in phase 3" — there's
nothing to bypass yet, since a single-cycle design's own writeback and its
own next read never share a cycle the way a pipelined design's can.

Verification at this stage is a hand-assembled 8-instruction smoke test
(`tb_core.cpp`): `addi x1,x0,5`; `addi x2,x0,7`; then `add`/`sub`/`and`/
`or`/`xor` of x1 and x2 into x3–x7; then `jal x0,0` as an infinite loop.
It prints a 20-cycle PC/instruction/writeback trace rather than asserting
against expected values — there's no `CHECK_EQ` in this testbench, so
`tb.finish()` reports 0 checks. This phase's verification is "does the
trace look right," not an automated pass/fail; the trace-diff-against-Spike
rigor from Phase 0 isn't applied to the whole core until Phase 4's
riscv-tests run.

## Design decisions and why

**ALU operation encoding follows funct7[5]:funct3 directly.**
`rv32i_defs.vh`'s macros are 4-bit codes shaped as `{instr[30], funct3}` —
`` `ALU_ADD `` is `4'b0000`, `` `ALU_SUB `` is `4'b1000` (funct3 000 for
both, distinguished only by the high bit, which is `instr[30]`), and so on
for the SLL/SRL/SRA pair. `decoder.v` builds `alu_op` for R-type (`OP`)
instructions as `{instr[30], funct3}` directly — no separate case-based
translation table, the encoding was chosen specifically so the instruction
bits could be wired straight through.

**OP-IMM masks `instr[30]` except for `funct3 == 101`.** The same
`{instr[30], funct3}` shape is reused for I-type ALU ops, but as
`{(funct3 == 3'b101) ? instr[30] : 1'b0, funct3}`. RV32I only defines two
immediate-form opcodes where bit 30 matters — `SRLI`/`SRAI`, both
`funct3 = 101`, distinguished by that bit — every other I-type ALU op
(`ADDI`, `SLTI`, `ANDI`, ...) has no legal encoding where bit 30 is
meaningful, but nothing stops a program's immediate field from happening
to set it. Masking it to 0 except in the one case that actually needs it
prevents, e.g., an `ADDI` whose immediate happens to have bit 10 set from
accidentally decoding as a subtract-shaped ALU op.

**PC redirect priority.** `pc_redirect = is_jal || is_jalr || (is_branch
&& branch_taken)`, and `pc_target` is `alu_result & ~32'd1` for `jalr`
(clearing the low bit, per the spec) or `pc + imm` otherwise — `jal` and
taken branches both compute their target as `pc + imm`, so there's no need
for a third case.

**`SYSTEM` (ecall/ebreak/CSR) instructions decode as a no-op.** The
decoder's case for `7'b1110011` is empty at this point, with a comment
reading "until phase 5." CSR and trap support does eventually arrive
before an actual Phase 5 — in the commit that becomes labeled Phase 4 (see
phase4.md) — but as of this phase the plan was still to defer it later
than that.

## Verified

No automated pass/fail at this phase (see above) — a hand-traced
8-instruction program confirms the datapath wiring by inspection. The
first automated, quantified test count for the *core* comes in Phase 4.
