# Phase 3 — Pipeline registers

## What this phase was for

Cut the single-cycle datapath into five stages by inserting four register
banks. **No hazard logic** — that is deliberately Phase 4.

The phase changes nothing about *what* the core computes. It changes *when*.
And it deliberately produces a core that fails most of the test suite, because
the mechanisms that make a pipeline correct are the next phase's work.

## The change

In the single-cycle core, a signal propagates the entire datapath between clock
edges — PC → imem → decoder → regfile → ALU → dmem → writeback mux → back to
the regfile. The clock period must accommodate all of it in series. The ALU
sits idle while imem is read; imem sits idle during the memory access.

Four register banks cut that path into five segments. The clock now only has to
accommodate the longest single segment, and five different instructions can
occupy the five segments simultaneously.

Each instruction still takes five cycles. One *completes* per cycle once the
pipe is full.

| register | carries |
|---|---|
| IF/ID | `pc`, `instr` |
| ID/EX | `pc`, `pc_plus4`, `rs1_data`, `rs2_data`, `imm`, register addresses, `funct3`, `instr`, and every control signal |
| EX/MEM | `alu_result`, `rs2_data`, `pc_plus4`, `csr_rdata`, `rd_addr`, `funct3`, control |
| MEM/WB | `alu_result`, `mem_rdata`, `pc_plus4`, `csr_rdata`, `rd_addr`, control |

There is no state machine. All five stages run every cycle unconditionally; the
clock edge is the only sequencer. On each edge all four banks capture
simultaneously, and that single event advances every instruction by one stage.

## Decisions

### Individual flops, not a packed control word

Roughly fifteen control signals travel from ID to EX to MEM to WB. Packing them
into one vector means one register per stage and much less repetitive code;
individual flops mean each signal has a name in the waveform.

Readability won. This is the phase where you spend time in GTKWave working out
which instruction is where, and `id_ex_reg_we` is a great deal easier to find
than bit 7 of a bus. Packing is a reasonable later refactor once the shape is
stable.

### Branch resolution in EX

The alternative is ID, which would halve the misprediction penalty from two
bubbles to one.

EX won because everything is already there: `branch_cmp` needs `rs1_data` and
`rs2_data`, which arrive in EX, and `jalr` needs the ALU. Resolving in ID means
duplicating the comparator into ID, reading the register file a stage earlier,
and — the real cost — putting the *forwarding* problem in ID as well, which is
messier than forwarding into EX.

The price is one extra flush cycle. On a Basys 3 with no branch predictor that
is a small IPC difference, and it becomes interesting only in Phase 7.

### Reset clears control, not data

Every register bank's reset branch zeroes `reg_we`, `mem_we`, the branch and
jump flags, and the CSR enables — and leaves the data fields alone.

Same principle as the PC in Phase 2: **reset what determines behaviour, not
bulk storage.** Garbage in `id_ex_rs1_data` is harmless because nothing acts on
it while `id_ex_reg_we` is low. A stray `reg_we` during reset would corrupt the
register file.

### File organisation

`core.v` was restructured top to bottom as IF → IF/ID → ID → ID/EX → EX →
EX/MEM → MEM → MEM/WB → WB, with banner comments, and all declarations grouped
by stage at the top.

This is not cosmetic. The file roughly tripled in length and stage boundaries
are the only structure it has. When debugging, "what does EX see?" is answered
by looking at one block.

## The rule that catches wiring bugs

Every signal falls into one of three categories, and knowing which determines
its prefix:

- **Produced in this stage** — bare wire, no prefix. `alu_result` in EX,
  `mem_rdata` in MEM, `instr` in IF. Nothing in front of them to read from.
- **Passing through** — carries the previous stage's prefix.
  `id_ex_rs2_data` in EX is a value the regfile produced back in ID.
- **Control** — decoded once in ID, carried the whole way.

**If a module reads a bare wire that was not produced in its own stage, that is
a bug.** A bare `pc` appearing in EX means the ALU is seeing the PC of an
instruction three ahead of the one it is executing.

Two consequences worth noting. `pc_plus4` travels the entire pipeline
unchanged, because `jal` computes it in IF and writes it in WB.
`funct3` stops at EX/MEM, because `dmem` is its last consumer — not every
signal goes the full distance.

Three signals were carried with no consumer at all: `id_ex_rs1_addr`,
`id_ex_rs2_addr` and `id_ex_mem_re`. They exist for Phase 4's forwarding
comparators and load-use detection, and were lint-suppressed in the meantime.

## The two backward paths

Everything else flows forward. These two do not, and both are sources of
trouble.

**Writeback.** The regfile's read ports are in ID; its write port is in WB. So
`rd_addr` connects to `mem_wb_rd_addr` and `rd_we` to `mem_wb_reg_we`, while
the read addresses come from the current ID decode. One module straddling two
stages four apart.

**PC redirect.** Branches and jumps resolve in EX, but the PC lives in IF. So
`pc_redirect` and `pc_target` are computed in EX and consumed two stages
earlier.

That backward direction is exactly why hazards exist. Forward-flowing data
always arrives before it is needed; backward-flowing data arrives late.

## Verification

The suite cannot be the checkpoint here — without hazard logic it fails by
design. So the check is a program in which no hazard can occur.

**Nop-padded program.** Four `nop`s between every real instruction. With five
stages, every dependency has fully retired before the next instruction reads
it, and every branch shadow is filled with nops that do nothing.

```
addi x1, x0, 5        →  x1 = 00000005
addi x2, x0, 7        →  x2 = 00000007
add  x3, x1, x2       →  x3 = 0000000c
sub  x4, x1, x2       →  x4 = fffffffe
```

Identical to the single-cycle results. That proves the pipeline registers carry
the right signals to the right stages — which is the entire Phase 3 claim.

## The two expected failure modes

Unpadded `riscv-tests` fail, and the trace shows exactly why.

**Stale register reads.** `add x5, x1, x2` followed by an instruction reading
`x5`: the second reads the register file in cycle 3, the first does not write
it until WB in cycle 5. Two cycles too early. The value exists — it is sitting
in a pipeline register — it just has not reached the register file.

**Unflushed branch shadow.** A branch resolves in EX in cycle 3. By then the
instructions at PC+4 and PC+8 are already in ID and IF. The PC redirects, but
those two continue through the pipe and retire.

The trace made this visible directly:

```
80000000,0500006f,-,-              ← jal, jumps to 0x80000050
80000004,34202f73,30,00000002      ← shadow: csrr t5, mcause -- wrote x30
```

In Phase 2 that second line did not exist; the single-cycle core redirected
before `0x80000004` could be fetched. Now it executes and writes a register.

Worse, a shadow instruction can be *itself* a branch or jump, computing a
target from garbage context and sending the core somewhere arbitrary. That is
what produced timeouts at addresses like `0x000000c4` — a wrongly-executed
instruction jumped into low memory, and `imem`'s address slicing wrapped it into
something fetchable.

Both are structural consequences of pipelining, not bugs in what was written.
The nop-padded test proves the datapath is sound; Phase 4 makes it correct
without the padding.

## What Phase 4 needs from this

Three mechanisms, none of them a state machine:

- **Forwarding** — combinational comparators plus muxes on the ALU inputs.
- **Stalling** — enables on the pipeline registers. `if (en) ... ;` — no
  assignment means hold.
- **Flushing** — clearing a register's control signals to zero, turning that
  instruction into a nop.
