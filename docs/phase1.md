# Phase 1 — Building blocks

## What this phase was for

Six modules, each written, linted, and verified standalone before the next was
started. No integration, no datapath, no CPU, just making sure each modulen works as intendent by itself.

The rule was strict: a module was not finished until it lint-cleaned with
`-Wall` treated as errors and passed its own testbench. This is slower than
writing everything and debugging it together, and it is the reason Phase 2's
integration bugs were all *wiring* bugs rather than module bugs.

Order: `regfile`, `alu`, `immgen`, `imem`, `dmem`, `decoder`. The regfile went
first deliberately — it is small, it establishes the testbench pattern reused
by everything after it, and its same-cycle write/read question is a miniature
version of the forwarding problem that dominates Phase 4.

`branch_cmp` and `csr` were added later, in Phase 2, and are documented there.

## The verification harness

Everything in this project is simulated with Verilator, which is a translator
rather than an event simulator: it compiles synthesisable Verilog into a C++
class. There is no `initial` block, no `#10` delay, no clock generator. The
testbench is an ordinary C++ program that pokes member variables and calls
`eval()`.

**`sim/verilator/tb_common.h`** is the shared harness, header-only:

- **`Tb<DUT>`** — a template class holding the DUT, an FST trace, a timestamp
  counter, and a seeded RNG. Templated because `Vregfile`, `Valu` and friends
  are unrelated C++ classes with no common base.
- **`tick()`** — one full clock cycle. Settles inputs while `clk` is low,
  raises it, lowers it. Always leaves `clk` low, so the caller's model is
  consistently "set inputs → `tick()` → they were captured."
- **`settle()`** — no clock edge. Recomputes combinational logic and advances
  the trace timestamp so the waveform is readable. Without the timestamp bump
  every stimulus lands at the same time and GTKWave shows one vertical wall.
- **`CHECK_EQ(got, want)`** — a macro rather than a function, because `#got`
  stringifies the source text of the expression and `__LINE__` locates it. A
  failure reports the section, the file and line, the expression as written,
  and both values in hex and decimal.
- **`tb_begin("name")`** — labels a section so failures say what was being
  tested.
- Seeded RNG, with the seed printed and overridable by `+seed=`, so any random
  failure is reproducible.

**The testbench pattern** used for every module: directed tests covering the
specific edge cases the design decisions created, followed by a randomised pass
against a C++ shadow model. Directed tests catch the cases you thought of;
random catches the ones you did not.

**`sim/verilator/selftest/selftest.v`** — a throwaway 8-bit counter written
before any real RTL, purely to prove the Verilator build, the FST tracing, and
GTKWave all worked. Debugging a new module and a new toolchain simultaneously
means every failure has two possible causes; the counter is trivially correct
by inspection, so any failure is a flow problem.

**The Makefile** takes `MODULE=<name>` and derives everything from it, with a
per-module override hook (`RTL_<module> :=`) for anything needing more than one
source file. `make lint`, `make MODULE=x`, `make waves MODULE=x`,
`make test-all`.

One thing that cost real time later and is worth stating plainly: **`make lint`
does not rebuild.** It parses. Only `make MODULE=x` recompiles. Several rounds
of edits during Phase 4 produced identical simulation results because a stale
binary was being run.

---

## `regfile.v`

32 registers of 32 bits, two asynchronous read ports, one synchronous write
port. Four decisions.

### Asynchronous read

The alternative was a registered read, which is what block RAM gives you and
what higher-Fmax cores use.

Async won for three reasons. The write-first bypass becomes a single mux rather
than external logic. Stalling is trivial — hold the pipeline register and the
regfile keeps combinationally reading whatever address is held, with no
internal state that can advance behind your back. And it maps to distributed
RAM (LUTRAM in SLICEM), costing tens of LUTs out of 20,800 and leaving every
block RAM free for `imem` and `dmem`, which actually need the capacity.

The cost is that the array lookup sits in the ID combinational path. On a
5-stage core the critical path is almost always EX — a 32-bit ALU carry chain
plus forwarding muxes — so moving the regfile read behind a flop would optimise
a stage that was not limiting anything.

There is also a hard constraint that makes the choice less free than it looks:
**Xilinx block RAM does not provide write-first behaviour across ports.**
Same-port write-first works, but a read on port B at the same address a write is
happening on port A in the same cycle produces undefined data — a documented
collision case. So a synchronous read would have needed the bypass built
externally on the RAM output anyway, in the path the change was meant to
shorten.

### `x0` enforced on both the write and the read

`x0` is hardwired to zero: reads return zero, writes are discarded.

This is not a corner case being defended against. Writes to `x0` are constant
in real code — `nop` is `addi x0, x0, 0`, `j` is `jal x0`, `ret` is
`jalr x0, ra, 0`. Compile anything and disassemble it; they are everywhere.

The read gate alone is sufficient: if reads of address 0 always return zero, it
does not matter what is in `regs[0]`. The write gate is redundant for
correctness but keeps `regs[0]` visibly clean in waveforms and register dumps,
which matters when comparing against Spike.

Enforced inside the module rather than by having the decoder never assert
`rd_we` for `x0`. A module should enforce its own invariants — otherwise
correctness depends on every caller honouring an unwritten contract, and Phase
6's formal tools will drive the ports directly and try exactly that.

### Write-first bypass, three-term condition

In the pipeline, WB writes the register file while ID reads it. When the
addresses match, the read must return the value being written, not the stale
array contents.

Read-first is what you get for free. Write-first costs one 32-bit mux per read
port, with the comparator running in parallel with the array lookup, so the
added delay is one mux level.

Write-first was chosen because read-first does not save the mux — it *relocates*
it. Choose read-first and Phase 4 needs a WB→ID forwarding path built in logic
hanging off the regfile's output in ID: the same mux, scattered outside the
module instead of encapsulated. Write-first handles the distance-3 hazard
internally, leaving Phase 4 with only the two EX-stage forwarding paths.

The condition is three terms: **the write is enabled, the write address matches
the read address, and the address is not `x0`.**

The third term is the one that bites. A `nop` has `rd_we` high and `rd_addr`
zero, so an instruction reading `x0` in the same cycle would match, and the
bypass would forward the nop's deliberately-discarded result out of a register
the ISA guarantees is zero.

The read path is ordered **array → bypass mux → `x0` gate**, in that order.
With the gate last, nothing upstream can leak a non-zero value out of address
zero regardless of what the bypass logic does. That structural ordering holds
even if the condition is later changed and got wrong.

The bypass is an explicit mux, not a blocking-assignment ordering trick.
Relying on event-scheduling order to make the read "see" the write is a genuine
race: it may resolve one way in Verilator, another in a different simulator,
and a third in synthesis.

The whole thing sits behind a `BYPASS` parameter, which turned out to matter in
Phase 2 — see that document.

### No reset, but an `initial` block

RV32I leaves register state undefined at reset, and `crt0.S` sets up `sp` and
`gp` before anything reads them, so a reset buys nothing architecturally.

It costs a great deal structurally: **distributed RAM has no reset port.** One
`if (!rst_n)` and Vivado can no longer infer LUTRAM — it falls back to 1,024
discrete flip-flops plus a 32-way read mux tree, and the resource report
silently triples.

An `initial` block zeroes the array instead. Uninitialised Verilog arrays are
`X`, and `X` propagates: one stale read turns the whole downstream waveform red
with no indication of where it started. More importantly, **Spike starts its
registers at zero**, so if the core ever reads before writing, the divergence
should be reproducible and point at the real first difference.

This is an FPGA affordance. There is no bitstream in ASIC; flops power up in
whatever state they power up in. Noted so the habit does not get carried into a
tapeout.

### Verification

Nine directed tests plus a shadow-model random pass, roughly 10,000 checks. The
three that actually find things: the bypass firing when it should, the bypass
respecting `rd_we`, and the bypass not resurrecting `x0`.

One subtlety in the testbench itself: the write helper drops `rd_we` *after*
the clock edge. Leave it high and read the same address, and the bypass
satisfies the read even if the array write never happened — the test passes and
proves nothing.

---

## `alu.v`

Ten operations: ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, SLT, SLTU. Purely
combinational.

### `alu_op = {instr[30], funct3}`

The obvious encoding is a sequential enum, which keeps the ALU ignorant of
RISC-V instruction encoding at the cost of a lookup table in the decoder.

RV32I already encodes these ten operations in `funct3` plus one bit of
`funct7`. Choosing `{instr[30], funct3}` as the ALU's select makes R-type
decode a plain wire concatenation.

The ALU's Verilog is identical either way — it references `` `ALU_ADD``,
`` `ALU_SUB`` and never sees the numbers. Only the constant values in
`rtl/include/rv32i_defs.vh` differ. So the readable module and the cheap
decoder are not in tension.

ADD/SUB and SRL/SRA are the only pairs sharing a `funct3`, which is precisely
what bit 30 exists to disambiguate. The consequence for the decoder is
documented below.

### Signed and unsigned

Verilog wires are unsigned by default. This is the single biggest trap in the
module, and it fails silently.

`a < b` is an unsigned comparison — correct for SLTU, wrong for SLT. And `>>>`
is the arithmetic shift operator, but **it only shifts arithmetically when its
left operand is signed**; on a plain `wire [31:0]` it degrades to a logical
shift with no warning.

So SLT uses `$signed(a) < $signed(b)`, SRA uses `$signed(a) >>> shamt`, and
SLTU and SRL use no cast at all. Four operations care; the other six are
bit-identical for signed and unsigned because two's complement is designed that
way.

The reason this is dangerous rather than merely wrong: **it only fails on
negative operands.** Test with 5 and 10 and everything passes. Shift a positive
number right arithmetically and logically and the results are identical.

RV32I has no condition flags — no carry, no overflow, no zero flag anywhere in
the architecture. Comparison results are ordinary register values, which is why
`slt` and `sltu` are instructions at all and why the ALU has no flag outputs.

### Shift amount

Always `b[4:0]`. RV32I mandates that only the low five bits are significant, so
shifting by 33 shifts by 1 — not by 33, and not by 0. Feeding the full 32-bit
`b` into a Verilog shift operator produces zero for large counts, which
contradicts the spec.

The same trap exists in the reference model: **C shifts by ≥ 32 are undefined
behaviour**, not "gives zero". x86 hardware happens to mask to five bits so it
often accidentally agrees, but the compiler is free to assume it never happens.
The C model masks explicitly.

### Verification

Roughly 20,000 checks. The random pass biases toward edge values a third of the
time, because uniform 32-bit random essentially never produces `0x80000000` or
a shift amount of exactly 32 — the values where the sign and masking bugs live.

Directed tests worth calling out: SRA of `0x80000000` by 63 (masks to 31, so
every bit becomes a copy of the sign bit — `0xFFFFFFFF`), and SLT versus SLTU
on `0x80000000` against `0x7FFFFFFF`, which are adjacent bit patterns across
the sign bit and therefore give opposite answers.

---

## `immgen.v`

Extracts the immediate from an instruction and sign-extends it to 32 bits. Five
formats: I, S, B, U, J.

### Self-decoding

The alternative was an `imm_sel` input from the decoder, saving a second
`case` on the opcode.

Self-decoding won because it makes the module a pure function of the
instruction — a cleaner contract for Phase 6's formal tools, and a much simpler
testbench. With `imm_sel`, the testbench would need the opcode-to-format
mapping itself in order to generate sensible stimulus, duplicating the logic
into C++ anyway.

The duplication argument against is real but weak here: the opcode-to-format
mapping is a frozen ISA fact, not evolving design logic.

### Why the immediates are scrambled

B-type and J-type look deranged until you see the reason.

The central design decision of the RISC-V encoding is that **`rs1`, `rs2` and
`rd` are always in the same bit positions** across every format. That means the
decoder extracts register addresses with plain wires regardless of instruction
type — no mux, no format detection.

The immediates got whatever was left over. And within that constraint, two
rules generate the whole scheme:

1. **`instr[31]` is the sign bit in every format.** So sign extension is one
   wire fanned out to the upper bits, with no mux in front of it.
2. **Every other immediate bit stays at the same instruction position across as
   many formats as possible.** So most output bits need a 2-input mux rather
   than a 5-input one.

Comparing S and B makes it concrete: bits 10 down to 1 do not move at all. The
slot that held `imm[0]` in S-type (`instr[7]`) is reused for `imm[11]` in
B-type. One bit relocated instead of twelve.

The obvious alternative — store the branch offset as a plain 12-bit field and
shift left by one in hardware — would move *every* immediate bit to a different
position than S uses, making each output bit's mux wider.

B and J also hardwire bit 0 to zero, because instructions are 4-byte aligned so
any branch or jump target is even. Not storing it buys a doubled reach for
free: B gets 13 bits of range from 12 stored bits.

U-type is the one format with no sign extension. Its value already occupies bit
31; there is nothing above it to extend into.

### Verification

Roughly 20,000 checks. Programmatic encoders build instruction words from a
plain signed immediate, and the random pass round-trips them: encode, decode,
compare.

**The circularity problem:** if the encoder and the RTL scramble B-type
identically wrong, the round-trip passes and proves nothing. So the first
section of the testbench uses instruction words taken from real GCC-produced
disassembly, which is ground truth independent of anything written here.

Two bugs were later found in this testbench and none in the module — see the
note at the end of this document.

---

## `imem.v`

Instruction memory. 16,384 words of 32 bits — 64 KB, matching the linker
script's region.

As built in this phase: asynchronous read, no clock. A plain
`assign instr = mem[addr[15:2]]`. This was a deliberate staging decision so
that Phase 2's single-cycle core is genuinely single-cycle with nothing to
reason about. It was converted to synchronous read in Phase 5, when the FPGA
forced it — 64 KB cannot fit in LUTRAM, and Artix-7 block RAM reads are always
registered.

**`addr[15:2]` does three things at once.** Dropping the low two bits is
division by four (the PC is a byte address; the array is word-indexed).
Ignoring bits above 15 means `0x80000000` lands at index 0 with no subtractor,
and out-of-range addresses wrap rather than reading past the array. The cost is
aliasing — `0x80010000` also hits index 0 — which nothing in this system
generates.

No write port. Nothing in the design writes instruction memory, and leaving the
port out means Vivado infers a ROM with no fetch/store arbitration to worry
about. This is the reason `fence_i` cannot pass; see Phase 5.

`$readmemh` loads the program, with a `+imem=<file>` plusarg override so one
build can run every test. The plusarg path is excluded from synthesis —
`string` is SystemVerilog and Vivado rejects it in a `.v` file.

---

## `dmem.v`

Data memory. Same 16,384 words, but where `imem` is a lookup table, this must
handle three access sizes in both directions plus sign extension.

**The core problem:** storage is 32-bit words; software wants byte granularity.

**Stores must not clobber neighbours.** `sb` changes one byte and leaves three
alone, but assigning to `mem[i]` writes all 32 bits. The fix is to treat the
word as four independent byte lanes, each with its own write enable — a `wstrb`
computed from the access size and `addr[1:0]`. `sb` enables one lane, `sh` two,
`sw` all four.

This is how memory actually works; Artix-7 block RAM has byte-write-enable pins
for exactly this. The four lane writes are **four separate `if` statements**,
which is both what makes them independent and the shape Vivado recognises for
byte-write inference.

The read-modify-write alternative — read the word, splice in the byte, write it
back — needs a read and a write in the same cycle, which a single-ported memory
cannot do.

**Data has to move.** `sb` of `0xAB` to byte 3 arrives on `wdata` in bits 7:0
(it came from a register) and must land in bits 31:24. Shift left by 8 × lane
on the way in; shift right by the same on the way out.

**Loads extend.** A byte holding `0xFF` is either −1 or 255 depending on
whether the C variable was `char` or `unsigned char`. `lb` sign-extends, `lbu`
zero-extends.

`funct3` does useful work here: bits [1:0] are the size and bit [2] is the
unsigned flag, so store logic only looks at [1:0] and load logic uses [2] to
pick the extension, rather than enumerating five cases.

No misalignment detection. RV32I says misaligned accesses should trap, and
there was no trap mechanism at this point. Noted in the source and addressed in
Phase 5 — differently than expected.

All of the byte-lane and extension logic moved out of this module in Phase 5,
when `dmem` went behind the AXI bus and became a plain word memory behind
`wstrb`.

---

## `decoder.v`

Reads the instruction and produces the control signals that steer everything
else. The only module not in the data path.

**Structure:** one `always @(*)` with defaults assigned first and a `case` on
`instr[6:0]` overriding only what differs. Defaults-then-override guarantees
every signal is assigned on every path — no latch inference — and keeps each
arm two or three lines instead of twelve.

**Register fields are plain wires.** `rs1_addr = instr[19:15]`,
`rs2_addr = instr[24:20]`, `rd_addr = instr[11:7]`, `funct3 = instr[14:12]`.
Fixed positions across all formats is what makes this free.

They are extracted unconditionally, even for formats that have no such field.
`lui` is U-type with no `rs1`, but `instr[19:15]` carries immediate bits and the
regfile dutifully reads whatever register that names. Harmless — the datapath
ignores it — and cheaper than gating.

**`alu_a` needs three sources**, which is why there are two control bits rather
than one: `rs1` normally, zero for `lui`, PC for `auipc`. Both of those become
plain ADD instead of needing new ALU operations.

### The bit-30 trap

`alu_op = {instr[30], funct3}` works for R-type, where bit 30 is genuinely part
of `funct7` and distinguishes `add` from `sub`, `srl` from `sra`.

**For I-type it does not.** In `addi`, bit 30 is part of the immediate.
`addi x5, x1, -1` has immediate `0xFFF` — all ones — so bit 30 is set, and
passing it through makes `addi` decode as `sub`. Every negative-immediate add in
the program computes the wrong answer.

The exception is `srai`. Shift-immediate instructions only need five bits of
immediate for the shift amount, leaving room for `funct7` to act as an opcode
field, so `srai` legitimately uses bit 30 exactly as `sra` does. Its `funct3`
is `101`, shared with `srli`.

So OP-IMM masks bit 3 of `alu_op` unless `funct3 == 3'b101`.

This is the most common decoder bug in a first RISC-V core, and the testbench
covers it exhaustively: 5,000 random immediates against every non-shift
`funct3`, asserting bit 3 of `alu_op` never sets.

### Loads, stores, `jalr`, `lui`, `auipc`

All force `alu_op = ADD` regardless of their `funct3`, because the ALU is
computing an address or passing an immediate through, not performing the
operation `funct3` names.

### `illegal`

Raised by the `default` arm for any unrecognised opcode. Useless until Phase 2
added traps, but wiring it early meant the signal existed when needed, and gave
the testbench something to check for garbage instructions. The testbench also
verifies an illegal instruction asserts neither `reg_we` nor `mem_we` — a
faulting instruction that still writes state would corrupt things before a trap
handler could intervene.

---

## Results

Six modules, all lint-clean with `-Wall` as errors, all passing their own
testbenches. Roughly 90,000 checks across the phase.

No CPU yet. Nothing has executed an instruction.

## A note added later

In Phase 5, two of these testbenches were found to be broken, and in both cases
**the module was correct and the test was wrong.**

`tb_imem` had never been updated after the Phase 5 conversion to synchronous
read — it drove an address, called `settle()`, and expected data immediately.

`tb_immgen` had two bugs, both dating from this phase and never caught because
the final edits were not re-run: a `sext()` helper that did not truncate its
input to `n` bits before sign-extending (so the "expected" values were full
32-bit garbage), and a B-type directed test using −4094 where the format's
minimum is −4096.

Worth recording for two reasons. First, it is the argument for the
ground-truth-anchored section of `tb_immgen` — the disassembly-derived tests
passed throughout, which is why the module was never suspected. Second, a test
suite is only as good as the last time it was actually run to completion.
