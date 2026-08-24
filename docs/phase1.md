# Phase 1 — Register file (complete)

## What was built

Two commits, one minute apart (`01f9f6e`, `ca3ac9e`, both 2026-07-26). The
first added `rtl/core/regfile.v` alone; the second is the one with real
content — the register file plus its Verilator testbench. `alu.v`,
`decoder.v`, `immgen.v`, `dmem.v`, and `imem.v` also exist as of this commit,
but as empty, zero-byte placeholder files — nothing in them yet. They get
real content in Phase 2. `rtl/include/rv32i_defs.vh` exists too, but not
with the ALU opcode macros it has now; those also arrive in Phase 2 with
`alu.v`. So despite the "building blocks" framing the commit message might
suggest, Phase 1 is really just the register file.

`sim/verilator/tb_regfile.cpp` is the testbench, and it sets the pattern
every later testbench in this project follows: `tb_common.h`'s `Tb<DUT>`
template wraps a Verilated model with `tick()`/`settle()` helpers and a
`CHECK_EQ` macro that counts checks and failures instead of stopping at the
first one, directed cases first, then a randomized run checked against a
software shadow model. Here that's 5,000 random writes against a 32-entry
`model[32]` array, plus 9 directed cases before it (write/read-back on both
ports, write with `we` low, x0 write/read on both ports, bypass, bypass
respecting `we`, bypass not resurrecting x0, neighboring registers
undisturbed).

## Design decisions and why

**Asynchronous read.** `rs1_data`/`rs2_data` come from `regs[rs1_addr]`
indexed directly in a continuous assignment, not a registered output — no
clock edge on the read path.

**Write-first bypass, three-term condition.** `bypass_rs1` is
`BYPASS && rd_we && (rd_addr == rs1_addr) && (rs1_addr != 5'b0)` (and the
same shape for `rs2`). All three terms matter independently: `BYPASS` is a
module parameter so a caller can disable it entirely (see below), `rd_we`
guards against a same-address match that isn't actually a write this cycle,
and the `!= 0` term keeps x0 out of the bypass path — a write to x0 must
never appear to make x0 nonzero, and without that term the bypass would
briefly forward `rd_data` for any address that happens to be 0.

**x0 gating happens twice.** Once on the write side
(`rd_we && rd_addr != 5'b0` guards the `regs[rd_addr] <= rd_data`), and
again on the read side (`assign rs1_data = (rs1_addr == 5'b0) ? 32'b0 :
rs1_muxed`). The write-side guard alone isn't sufficient — `regs[0]` starts
at zero and nothing ever writes it, so in principle reading it directly
would already return zero — but the explicit read-side mux makes that
guarantee independent of whatever `regs[0]`'s initial value happens to be,
and is what test 8 (`bypass does not resurrect x0`) actually exercises: a
write to x0 with `rd_we` high still must not leak through the bypass mux,
which the read-side gate rather than the write-side guard is what catches.

**`BYPASS` is a parameter, not baked in.** `regfile #(.BYPASS(0))` disables
the write-first bypass entirely — read and write ports behave like a plain
two-read/one-write RAM with no same-cycle forwarding. This isn't used in
Phase 1 (the testbench instantiates the default `BYPASS=1`), but is what
lets Phase 2's single-cycle core start with `BYPASS(0)` — a single-cycle
design's write and read of the same register never happen in the same
cycle for the same instruction the way a pipelined design's can, so there's
nothing to bypass yet — and flip it on later once pipelining creates same-
cycle write/read hazards. See phase2.md and phase4.md.

**No reset — an `initial` block instead.** There's no `rst`/`rst_n` port
on this module at all; `regs[]` is zeroed once, in simulation, by a
for-loop `initial` block. The file carries a comment explaining this
directly: "only have this initial block since FPGA needs to initialize /
if designing for silicon add this for synthesis and remove initial block."
A commented-out `` `ifndef SYNTHESIS `` guarded version of the same loop
sits right above it, unused. The FPGA target this project builds for
(Xilinx Artix-7) loads a distributed-RAM/LUTRAM array's initial contents
from the bitstream, so the `initial` block is synthesizable and correct
for that target; it would not be for an ASIC flow, which is what the
comment is flagging as a known, deliberate limitation rather than an
oversight.

## Verified

`make run MODULE=regfile` (via `sim/verilator/Makefile`): 10,014 checks, 0
failures.
