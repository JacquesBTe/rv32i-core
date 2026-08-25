# Phase 5 — SoC: bus, peripherals, interrupts, and first silicon

## What this phase was for

Two things at once, and they turned out to be entangled.

**Wrap the core in a system.** An AXI4-Lite bus, GPIO, UART, a timer, and the
CSR and interrupt plumbing to use them. Phases 0–4 produced a CPU; this phase
produces something a program can actually interact with.

**Put it on real silicon.** This is the first phase where the design leaves
simulation. That forces changes the core had been deferring since Phase 1 —
specifically, both memories must become synchronous, because 64 KB cannot fit
in LUTRAM and Artix-7 block RAM reads are always registered.

The FPGA work came first, deliberately. Adding a bus and three peripherals on
top of a core that has never run in fabric means any failure has several
possible causes.

---

## 0. Decisions

### Bus protocol: AXI4-Lite

Three candidates were weighed.

**AXI4-Lite** — ARM's AMBA standard, and the native language of the Xilinx
ecosystem. Five independent channels (write address, write data, write
response, read address, read data), each with its own valid/ready handshake,
totalling around eighteen signals.

**Wishbone** — one handshake, about eight signals, roughly a quarter the code.
It is also what the Caravel harness uses, which is directly relevant given
prior SKY130 tapeout work.

**A custom valid/ready bus** — fastest to working, zero transferable value.

For a three-peripheral SoC on a Basys 3, AXI4-Lite is over-engineered in pure
engineering terms. Wishbone does the same job with far less code, and the
five-channel split is pure overhead for an in-order core that always has
address and data together.

It was chosen anyway, for reasons outside the design itself. Vivado's IP
Integrator speaks AXI, so attaching any Xilinx block later — a DDR controller,
DMA, an Ethernet MAC — requires it. And the bus is the one component where the
*standard* matters more than the implementation: nobody is impressed by a
hand-rolled arbiter, but "speaks AXI4-Lite" is a checkable claim.

Strictly Lite. No bursts, no IDs, no out-of-order. Adding any of those turns
this into a much larger project.

### Vendored code

Alex Forencich's `verilog-axi` and `verilog-uart` were inspected and partially
adopted, both MIT licensed.

What was taken:

| file | lines | purpose |
|---|---|---|
| `axil_reg_if.v` + `_wr` + `_rd` | 449 | AXI-Lite slave → register read/write interface |
| `axil_interconnect.v` | 564 | 1 master × N slaves, parameterised address decode |
| `arbiter.v`, `priority_encoder.v` | 251 | interconnect dependencies |
| `uart_rx.v`, `uart_tx.v` | 257 | UART bit timing, framing, oversampling |

Roughly 1,500 lines not written here.

What is **not** in those repositories, contrary to expectation: any peripherals
at all. `verilog-axi` is bus infrastructure — no GPIO, no timer, no UART. Its
UART lives in a separate repository and is AXI-Stream rather than AXI-Lite, so
it needed a wrapper regardless.

`axil_reg_if` turned out to be a better version of the register-interface module
that had been sketched for this phase: it carries a wait/ack handshake on the
register side so a slow peripheral can stall properly, plus a timeout.

Deliberately not taken: `axil_ram.v`, because using it would have meant losing
`dmem`'s byte-lane handling; and `axil_crossbar.v`, which is a full N×M crossbar
where there is exactly one master.

Everything lives in `rtl/vendor/` rather than mixed into `rtl/soc/`, so it is
unambiguous which code is original. Their lint warnings — mostly style
differences from this project's `-Wall`-as-errors setting — are suppressed by a
`vendor.vlt` Verilator config scoped to that directory, leaving the project's
own lint strict.

### Memory map

```
0x8000_0000   dmem    64 KB   (addr[31] == 1)
0x1000_0000   gpio     4 KB
0x1000_1000   uart     4 KB
0x1000_2000   timer    4 KB
```

Peripherals at `0x1000_0000` rather than adjacent to memory, so `addr[31]`
alone separates memory from device and the top-level decode is a single bit.

Each peripheral gets 4 KB despite needing about sixteen bytes. Wasteful in
address space, free in hardware — decode `addr[15:12]` to pick the peripheral
and `addr[3:2]` to pick the register. Tight packing would need wider
comparators for no benefit.

**`imem` stays off the bus.** The core remains Harvard-split, and instruction
fetch is unchanged from Phase 4. Merging the memories would mean the bus has to
arbitrate between fetch and load/store — a structural hazard that does not
currently exist — and it would be the only way to make `fence_i` meaningful,
which is not worth the cost.

**`dmem` stays at `0x8000_0000`.** Moving it would require changing `link.ld`
and `crt0.S`, and since `imem` is off the bus there is no ambiguity about which
memory an address refers to: fetches go to `imem` directly, loads and stores go
through the bus. Code and data share an address range and could in principle
collide, which is not a problem at this scale.

### The bus stalls the pipeline

Choosing AXI4-Lite effectively decided this. Its `READY` signals exist so a
slave can say "not this cycle"; guaranteeing single-cycle responses everywhere
would make the handshake decorative and take the complexity without the
capability.

A bus stall reaches further than anything previously in the design:

| | load-use | branch flush | bus stall |
|---|---|---|---|
| stalls | pc, IF/ID | — | pc and all four registers |
| flushes | ID/EX | IF/ID, ID/EX | — |
| duration | exactly 1 cycle | 1 event | unbounded |

Priority in every pipeline register:

```
reset → bus_stall → flush → load_use → normal capture
```

Bus stall outranks flush because a transaction physically in flight cannot be
abandoned; the branch redirect waits.

The one that bites: **MEM/WB must not advance while MEM is stalled**, or the
instruction in MEM is written back twice — once with garbage before the
transaction completes, once with real data. That is the bug that produces
intermittent register corruption.

`imem.en` and `dmem.en` both become `!(load_use || bus_stall)`.

A hedge to keep bring-up tractable: GPIO and the timer respond in one cycle.
They are register files; there is no reason to make the core wait. Only the
UART stalls, and only on a write when the transmitter is busy. The machinery is
real and exercised, but most accesses do not touch it.

---

## 1. Synchronous memories

Both memories had been asynchronous since Phase 1, twice deferred. The FPGA
forced the conversion.

### `imem`

The neat part: **the BRAM's own output register replaces `if_id_instr`.**

```
async:  pc → imem (combinational) → if_id_instr flop → decoder
sync:   pc → imem (registered output) → decoder
```

With a synchronous read, `instr` in cycle N holds the instruction fetched in
cycle N−1 — which is exactly one cycle behind `pc`, which is exactly what an
IF/ID instruction register provides. Adding one on top would make the decoder
two instructions behind. So the flop was deleted, and one bank of 32
registers with it.

`if_id_pc` stays, since the PC is not in block RAM.

**The flush mechanism had to change.** Previously a flush wrote `32'h00000013`
into `if_id_instr`. You cannot write into a BRAM output register. So an
`if_id_valid` bit was added, cleared on flush, gating a nop in front of the
decoder:

```verilog
wire [31:0] id_instr = if_id_valid ? instr : 32'h00000013;
```

That also fixes the Phase 4 reset bug structurally rather than by convention:
for the first cycle after reset the memory's output is garbage, and
`if_id_valid` being low turns it into a nop automatically.

### The `en` bug

The conversion broke every load test — and only the load tests, all failing at
the same test case, which is the pattern that says one specific mechanism.

The cause: **holding the PC during a stall is not enough with a synchronous
memory, because the next fetch is already in flight.**

| cycle | `pc` | `instr` | what happens |
|---|---|---|---|
| 2 | A+4 | I1 (`lw`) | I1 in ID |
| 3 | A+8 | I2 | I2 in ID, I1 in EX → `load_use` fires |
| 4 | A+8 (held) | **I3** | I2 is gone |

In cycle 3 the hazard is detected and the PC held. But `imem`'s address input
was *already* A+8 during cycle 3, so its output register loads I3 at the end of
that cycle regardless. Holding the PC from cycle 3 onward does not cancel a read
already committed.

The fix is a read enable, held low during the stall:

```verilog
always @(posedge clk) if (en) instr <= mem[addr[15:2]];
```

Xilinx block RAM has a physical `ENA` pin for exactly this, so it is not added
logic — it is a port that was always there.

This generalises: `en` later became `!(load_use || bus_stall)` when the bus
arrived.

### `dmem`

Same conversion, one extra consideration: **when data is delayed by a register,
its control must be delayed by the same register.**

The lane selection and sign extension are driven by `addr[1:0]` and `funct3`,
which describe the *current* address, while the registered `word` holds data
from the *previous* one. Registering `lane_q` and `funct3_q` alongside `word`
keeps them describing the same transaction.

`dmem`'s output register then replaces `mem_wb_mem_rdata` — a second bank of
flops deleted.

Passed the suite first time, unlike `imem`.

### Result

39/42 maintained through both conversions, and the design became genuinely
FPGA-ready. Two register banks removed, replaced by registers the memories were
already providing.

---

## 2. FPGA bring-up

### The board wrapper

`rtl/fpga/basys3_top.v` bridges between the core and physical pins:

**Clock.** The Basys 3 has a 100 MHz oscillator on pin W5. A Clocking Wizard
MMCM produces 75 MHz from it. Initially a counter-based divider was used, which
works but drives flops from a LUT output — Vivado warns, and the derived clock
is not properly constrained. The MMCM is the correct approach and its `locked`
output is used to hold the core in reset until the clock is stable.

**Reset.** `btnC`, which is active-high on the Basys 3 while the core wants
active-low. It is also asynchronous to the clock and mechanically bouncy, so it
passes through two flops:

```verilog
reg [1:0] rst_sync = 2'b0;
always @(posedge cpu_clk) rst_sync <= {rst_sync[0], ~btnC & locked};
```

The two flops solve metastability — the first may go metastable, the second has
a full clock period to sample a settled value. They also make reset *release*
synchronous, so the whole design leaves reset on the same edge rather than
scattered across two.

None of this existed in simulation. Verilator's `rst_n` was a C++ variable set
between clock edges by construction.

### Vivado friction

Several days were lost to toolchain problems rather than design problems.
Recording them because they recur.

**Vivado cannot read WSL UNC paths.** `\\wsl$\Ubuntu-24.04\...` browses fine in
its file dialog and is then rejected by `add_files` with "Illegal file or
directory name." Mapping a network drive also failed. The resolution was a
second clone on the Windows side, with `git pull` before every build.

**`string` is SystemVerilog.** Verilator accepts it in a `.v` file; Vivado does
not. The `$value$plusargs` program-loading code in both memories is guarded with
`// synthesis translate_off` pragmas rather than `` `ifndef SYNTHESIS`` —
Yosys does not define `SYNTHESIS`, and this code may eventually go through an
open-source flow.

**`$readmemh` relative paths do not resolve.** Vivado resolves them against the
synthesis run directory, not the source tree, and fails silently apart from one
warning among hundreds. Absolute paths with forward slashes are required.

**Synthesis prunes designs with unconnected outputs.** This was the most
misleading. Synthesising `core` with nothing driving a pin produced a design of
106 registers instead of roughly 3,000 — synthesis works backward from outputs,
and with none, everything upstream is dead code. Only the PC survived, because
`pc <= pc + 4` is a self-contained loop.

The timing report was then perfectly valid and completely meaningless: WNS
+5.886 ns on a critical path that was just the PC increment adder.

The lesson: **check the endpoint count before believing any timing number.** If
it is far below what the design should contain, something was optimised away.

**A program that specialises the core is nearly as bad.** With a three-instruction
counting loop in `imem`, synthesis eliminated 31 of 32 registers and every ALU
operation except ADD, giving 166 registers. Constant propagation from a known
instruction stream is aggressive. Measuring Fmax requires a program that
exercises the whole design — one of the `rv32ui-p-*` images.

### Timing methodology

Worth stating explicitly, because the first several measurements were worthless:

1. Load a program that exercises the full instruction set.
2. Connect an output to a real pin, or the design is pruned.
3. Check the endpoint count first. It should be in the low thousands.
4. Read **post-route**, not post-synthesis. Synthesis estimates routing delay;
   implementation measures it.

With those satisfied, the core alone measured **−1.543 ns post-route against a
10 ns period** — about 87 MHz. 100 MHz was not achievable, so the target became
75 MHz, at which the core closed with **+0.538 ns**.

The critical path at that point was interesting: a load's data coming out of
block RAM, sign-extended, forwarded into EX, driving the branch comparator, and
generating `flush` — which reached the reset pins of the ID/EX registers with a
fanout of 87. In other words `lw` / `nop` / `beq` on the loaded value, resolving
in a single cycle. The BRAM's 2.454 ns clock-to-out was the single largest
component.

---

## 3. The bus, steps 1–6

Built module by module, each verified standalone before integration.

**`rtl/soc/axil_master.v`** — converts the MEM stage's single load or store into
AXI transactions and drives `bus_stall` until they complete. It also absorbs
the byte-lane logic that was in `dmem`: `wstrb` and the write shift outbound,
the read shift and sign extension inbound. That logic belongs here now because
`wstrb` is an AXI signal — a slave should not need to know about `lb` versus
`lbu`.

Two protocol details that matter: AW and W are independent channels and may be
accepted on different cycles, so each is tracked separately rather than assumed
to complete together. And `bus_stall` drops on the same cycle the response
arrives, with `mem_rdata` valid combinationally on that cycle, so MEM/WB can
capture at the same edge the pipeline resumes.

Verified standalone against a C++ stub slave with independently configurable
delays on `awready`, `wready` and `arready` — 3,029 checks.

**Pipeline stall plumbing** — `bus_stall` added to all four register banks and
the PC, in the priority order above. Verified with `bus_stall` tied low: 39/42
unchanged.

**`dmem` simplified** to a plain word memory behind `wstrb`. No `funct3`, no
shifting, no sign extension.

**`rtl/soc/gpio.v`** — two registers behind `axil_reg_if`: LED output and switch
input. 1,561 checks.

**`rtl/soc/soc.v`** — core, `axil_master`, the vendored interconnect, `dmem`
behind an `axil_mem` wrapper, and GPIO. This step caught a real timing bug in
the `mem_wb_rdata` capture before it reached the gate. **39/42 maintained with
`dmem` behind the bus** — the phase's most significant checkpoint.

**Hardware.** `sw/examples/gpio_loop.c` — LED = switches, looped — running on
the board with switches driving LEDs through the full path: core →
`axil_master` → interconnect → address decode → GPIO → pins.

### Timing went *up*

| stage | WNS at 75 MHz | endpoints |
|---|---|---|
| Core only, before the bus | +0.538 ns | 2956 |
| After bus integration | +1.223 ns | 2982 |

Adding the bus improved timing by 0.7 ns, which is counterintuitive until you
look at what moved. The old critical path was BRAM output → sign extend →
forwarding mux → branch comparator, all in one cycle. Moving the byte-lane and
sign-extension logic into `axil_master`, and putting an AXI handshake between
the memory and the pipeline, broke that chain across a cycle boundary.

The bus bought Fmax at the cost of IPC — instruction counts on the same tests
rose roughly 40%, since every load and store now goes through AXI handshaking
instead of a single-cycle `dmem`.

### An open item

Block RAM utilisation after this step was **16 tiles**, not the ~30 expected for
two 64 KB memories. 16 × RAMB36 is 576 Kb, and 64 KB is 512 Kb — that is one
memory, not two.

The likely cause is `dmem` falling out of BRAM inference when it was simplified:
the write block lost its outer `if (en)` gate, and Vivado's byte-write-enable
template expects `if (en) begin if (wstrb[i]) ... end`. Worth confirming with
`report_utilization -hierarchical`. It works either way, but if `dmem` is
sitting in fabric it will matter as more peripherals are added.

---

## 4. Steps 7–13: peripherals, interrupts, and the bugs interrupts exposed

### `uart_axil` (step 7)

A thin wrapper around the two vendored AXI-Stream shift registers,
`uart_rx`/`uart_tx`, behind `axil_reg_if`. Three registers:

```
0x00  DATA      write -> transmit byte;  read -> receive byte (pops it)
0x04  STATUS    bit 0 tx_busy, bit 1 rx_valid, bit 2 overrun, bit 3 frame_error
0x08  PRESCALE  baud divisor, clk / (baud * 8)
```

**Reading `DATA` never stalls; writing it does — deliberately asymmetric.**
A read with `rx_valid` low just returns whatever is sitting in the
(possibly stale) receive register, on the assumption software checks
`STATUS` first. The alternative — `reg_rd_wait` until a byte actually
arrives — was the other option on the table, and was rejected for
bring-up specifically: a wedged RX line (nothing connected, or a baud
mismatch) would hang every future bus read forever, on a bus every load
and store now goes through. A write, on the other hand, does stall
(`reg_wr_wait`) until `uart_tx` is ready for the next byte — there's no
polling convention on the write side that would make silently dropping a
byte acceptable, the way there is on the read side.

**Sticky status bits, cleared by the read that reports them.**
`overrun_error`/`frame_error` out of `uart_rx` are one-cycle pulses — real
enough to catch, but gone by the time a polling loop gets around to
checking. `overrun_sticky`/`frame_sticky` latch them and only clear on a
`STATUS` read, with clear-then-set ordering in the same always-block so a
pulse landing on the exact cycle of the clearing read still sets the flag
back for the *next* poll rather than being lost in the collision.

7,652 checks, standalone against a C++ model of the AXI-Stream side.

### Wiring in as a third slave (step 8 prep)

Mechanical — `axil_interconnect`'s `M_BASE_ADDR`/`M_ADDR_WIDTH` parameters
extended from two entries to three, `uart_axil` added to `soc.v` at
`0x1000_1000`. Nothing about this step was interesting on its own, which
is exactly what having `gpio` as a working precedent from step 4 buys you.

### `timer.v` (step 9)

A free-running 64-bit `mtime` and a 64-bit `mtimecmp`, split across four
32-bit registers the way RISC-V's machine timer convention expects:

```
0x00  mtime[31:0]     0x08  mtimecmp[31:0]
0x04  mtime[63:32]     0x0C  mtimecmp[63:32]
```

`timer_irq = (mtime >= mtimecmp)` — a plain combinational comparison, no
edge logic. `mtimecmp` resets to all-ones (`{64{1'b1}}`) rather than zero,
specifically so the interrupt can't fire before software has configured a
real threshold; `mtime` obviously can't be ≥ all-ones until software
raises `mtimecmp` down to something sane first. A register file exactly
like `gpio` — no real latency, `wait` tied low, `ack` mirrors `en` — so
this added nothing to the bus-stall story on its own.

Only 17 checks, and that's the right number for this module: six directed
scenarios (write and read each half of `mtime` and `mtimecmp`
independently, confirm `timer_irq` follows the comparison, confirm
re-arming `mtimecmp` forward clears it, confirm writing it back below
`mtime` re-raises it) fully cover a module with no hidden states — a free
counter and a comparator have no corner a random pass would find that the
directed cases don't already hit directly.

### Real CSR semantics: `mie`/`mip`/`mstatus`, privilege mode (step 10)

Phase 2's `csr.v` was six registers and an idea of a trap — enough to get
past riscv-tests' CSR probing (see phase2.md), but `mstatus` was a flat
32-bit register with no real bit semantics, there was no `mie`/`mip` at
all (both on the `csr_ignored` WARL list), and there was no notion of
privilege mode — every `ecall` hardcoded `mcause = 11`, the M-mode cause
code.

This step gives `mstatus` real fields — `MIE` (bit 3, global interrupt
enable), `MPIE` (bit 7, saved `MIE` across a trap), `MPP` (bits 12:11,
saved privilege mode) — plus `mie`/`mip` (bit 7 only, `MTIE`/`MTIP`, wired
straight to the timer's `timer_irq`) and a 1-bit `priv_m` register tracking
current privilege. Trap entry now does the real save: `MPIE <= MIE`,
`MIE <= 0`, `MPP <= priv_m`, `priv_m <= 1` (this core traps to M-mode
unconditionally, since there's no delegation). `mret` does the inverse:
`priv_m <= MPP`, `MIE <= MPIE`, `MPIE <= 1`, `MPP <= 0` (least-privileged,
per spec — there's nothing below U-mode to restore into, so it resets to
the floor).

This is also what finally closes the `ld_st` failure from Phase 2:
`ecall`'s cause is now `priv_m ? 11 : 8` instead of a hardcoded 11 —
`ld_st` traps from U-mode and checks for 8, which the old hardcoded value
could never produce regardless of what mode the core thought it was in,
because the core had no concept of mode to check.

**Everything here has to survive `bus_stall` without double-acting.** A
bus transaction can still be in flight for the instruction sitting behind
this one in EX; while `bus_stall` holds `id_ex`, `csr.v`'s own inputs are
the same frozen values every cycle for as long as the stall lasts. Capturing
`mepc`/`mcause` on a trap is naturally idempotent under that repetition —
writing the same value twice is harmless — but the `mret`/trap-entry
*swaps* are not: toggling `MIE`↔`MPIE` twice within one logical retirement
is not the same as toggling it once. So the whole state-update block gates
on `bus_stall` first, before even looking at `trap`/`is_mret`/`do_write` —
an explicit `else if (bus_stall) begin end` doing nothing, ahead of
everything else.

### Interrupt injection in `core.v` (step 11)

Taken at the EX stage boundary, against whatever instruction happens to
be there: writes suppressed the same way a synchronous trap's are
(`reg_we_final`/`mem_we_final` already gate on `trap`, and `trap` grows a
new `take_interrupt` term), PC — not PC+4, since the instruction never
actually completed and has to run again after `mret` — saved to `mepc`,
`mcause` set to `0x8000_0007` (the interrupt bit set, cause 7 for the
machine timer), control redirected to `mtvec`. A synchronous trap on that
same instruction always outranks the interrupt, since it genuinely
faulted and that's the more specific fact; neither fires while
`bus_stall` is high, since there's no instruction boundary to inject
against mid-transaction.

This is the step where an *asynchronous* event — one that can land on
literally any instruction, in any pipeline state, on any cycle — first
existed in this design. Every hazard mechanism built in Phase 3 and
Phase 4 had only ever needed to handle events triggered by an
instruction's own decode. Two bugs had been sitting dormant in exactly
that gap since before this phase started, and this is the step that made
them reachable.

**Bug: the PC register's own priority checked `load_use` before `flush`.**
Phase 4's writeup on load-use stalling already flags this exact ordering
as backwards from every other pipeline register — see phase4.md's load-use
section — but calls it harmless there, because nothing at that point could
make `load_use` and a redirect true on the same cycle. A synchronous trap
can't: `illegal`/`ecall` never set `mem_re`, so a faulting instruction is
never also the load half of a load-use hazard. An interrupt can, because
it doesn't care what the instruction in EX is doing — it can land on a
load whose result the *next* instruction depends on, making `load_use` and
`take_interrupt` true at once. When that happened, `load_use` won in PC's
own always-block, so PC simply held — while IF/ID and ID/EX both bubbled
for the flush regardless, since their own priority checks flush first.
`mepc`/`mcause`/`mstatus` all updated as if the trap were taken. PC never
moved. Fixed by giving PC the same order every other register already
had: check `pc_redirect` before `load_use`.

**Bug: `id_ex_mem_re` was never cleared on reset or on a bubble.**
Every other control field zeroed on a `flush || load_use` bubble in
Phase 3/4 — `id_ex_reg_we`, `id_ex_mem_we`, the branch/jump flags — but
`id_ex_mem_re` was missed, and stayed missed for two phases because
nothing downstream read it during a bubble; a bubble's `reg_we`/`mem_we`
being correctly zero was enough to make it inert. The first pass at
misalignment detection (superseded later this same step, see below)
combined `id_ex_mem_re` with `id_ex`'s equally-stale `alu_result` during a
bubble and produced spurious misalignment faults on completely ordinary,
aligned loads — the bubble's leftover `mem_re` from whatever real load had
last occupied that pipeline slot, paired with an `alu_result` that was
never actually a memory address, looked exactly like a genuine
misaligned access to logic that had no way to know the instruction behind
it was a bubble in every way that mattered *except* this one signal. Fixed
by adding it to the same clear list every other control field was
already on.

The common shape of both: a field that a bubble leaves stale rather than
explicitly clearing, invisible for as long as nothing new ever reads it
during a bubble — until something does. Two more instances of the exact
same shape turn up later in this step, on real hardware rather than in
simulation; see below.

### Native misaligned access support (step 12)

The original plan, and what got built and tested first: trap on
misalignment, `mcause` 4 (load) or 6 (store), and stop there. It works at
the RTL level and is exercised by its own directed tests — but it cannot
pass `rv32ui-p-ma_data`. That test issues genuinely misaligned loads and
stores and checks the resulting values directly; it has no trap handler
of its own to catch an exception, because riscv-tests' default `-p`
environment treats anything but a deliberate `ecall`-based exit as an
unhandled failure. `ma_data` isn't testing "does this trap correctly" —
it's testing "does a misaligned access just work," which is also
precisely how Spike behaves: it services misaligned accesses in software
underneath the trace, regardless of what real hardware could or couldn't
do, since Spike's job is being a golden reference, not modeling every
possible microarchitecture's limitations.

Redirected — confirmed as the right call before building it, since it's a
genuine change from the original plan rather than a bug fix — to native
support instead: `axil_master` detects a half or word access that crosses
a word boundary (a half only when its low byte lands in a word's last
byte lane; a word at any nonzero lane, since it can only fit in one word
starting at lane 0) and splits it into two word-aligned AXI transactions,
combining the results transparently on the read side and distributing the
write-strobe pattern's overflow bits onto the second word on the write
side. Software never sees a fault at all — the same as Spike, for the
same underlying reason. See "The bus, steps 1–6" above for why this
logic lives in `axil_master` and not `dmem`: byte-lane handling had
already moved there, and a crossing access is really just two byte-lane
transactions instead of one.

6,038 of `axil_master`'s checks now include crossing accesses at every
lane offset, against a C++ model that independently combines and splits
the same way. 41/42 `rv32ui-p-*`, with `ma_data` now passing.

### `hello.c` over UART (step 13)

The first program run on real hardware that exercises the full path —
core, pipeline, bus, interconnect, `dmem` behind `axil_mem`, and now
`uart_axil` — rather than one peripheral in isolation the way
`gpio_loop.c` did in step 6. It sums 1 through 10, multiplies two
`volatile` operands (forcing a real call to `__mulsi3`, since a constant
product would let GCC fold the whole check away — see phase0.md), and
prints the result over UART at 115200 8N1: `"hello from rv32i-core"` /
`"self-test: PASS"`, read on a real terminal through the Basys 3's
onboard USB-UART bridge. Passed on the first flash. The UART write side
uses no `STATUS` polling at all — `uart_putc` just writes `DATA` and lets
`reg_wr_wait` do the blocking, exactly the asymmetry step 7 built it for.

---

## `soc_demo.c` and the two bugs only hardware found

`basys3_top.v` moved on from `hello.c` to `soc_demo.c` — GPIO, UART, and
the timer/interrupt path all running *concurrently*: the main loop mirrors
switches to LEDs continuously, while a timer interrupt fires once a
second, prints `"tick\r\n"` over UART, and re-arms `mtimecmp`. Two
independent, concurrent sources of bus traffic, rather than one peripheral
tested at a time — and the first time this design's interrupt path ran at
its real, intended timescale instead of a simulation-shortened one.

It found two real bugs immediately, neither of which any test — unit,
riscv-tests, or the directed interrupt tests written earlier in this same
step — had caught.

### Bug: `if_id_pc` capturing the pre-redirect PC on flush

**Symptom.** On the board, `"tick"` printed far faster than once a second,
and the LEDs froze until a manual reset.

**Cause.** On any flush — a resolved jump or branch, a trap, an `mret` —
`if_id_pc <= pc` was capturing the PC *before* redirection, on the exact
edge `pc` itself was being set to `pc_target`. The instruction riding
along with that stale PC is always correctly nulled to a NOP
(`if_id_valid` goes low the same cycle), so the wrong PC value normally
never surfaces anywhere — until an interrupt happens to land on that exact
bubble while it's sitting in `id_ex`, and captures its stale, wrong-path
PC into `mepc`. `mret` then resumes execution at an address that was
never really going to run — typically a couple of instructions past an
unresolved backward branch, mid-loop garbage that reliably decoded as
illegal on the very next cycle. That illegal-instruction trap re-enters
the same handler, which has no way to distinguish it from a real timer
interrupt, so the visible symptom is indistinguishable from the timer
refiring in a tight storm.

This needed the *real* 75,000,000-cycle interrupt interval to catch, not
the shortened intervals used everywhere else in this phase's simulation
for practical runtime: a short interval samples very few pipeline states
before the run ends, and this specific coincidence — an interrupt landing
on an already-flushed bubble sitting a couple of instructions past an
unresolved backward jump — is rare enough that it never fired once across
every shorter test that existed.

**Fix.** Capture `pc_target` instead of `pc` on flush — `if_id_pc` needs to
track where control is actually headed, not the value `pc` held the
instant before the same edge changed it.

### Bug: `id_ex_pc` hardcoded to zero on its own bubble

The first fix wasn't complete. After flashing it, switches worked for
5–10 seconds after reset, then froze until another manual reset — a
different symptom from the same underlying class of problem, one stage
later.

**Cause.** `ID/EX`'s own bubble branch hardcoded `id_ex_pc <= 32'b0`
outright, rather than carrying forward any real address the way the
`if_id_pc` fix now did one stage earlier. An interrupt landing on *this*
bubble captured `mepc = 0` instead of a wrong-path address — the same
failure shape as the first bug, but a different concrete symptom, because
it needs the interrupt to land during this one specific one-cycle window
rather than the wider window the `if_id_pc` fix covered, so it took
longer to hit by chance — hence seconds of correct operation before the
freeze, rather than an immediate storm.

**Fix.** Same principle, but the correct value differs by which condition
caused the bubble: `flush` means control is genuinely headed to
`pc_target`; `load_use` alone means IF/ID is just being held while its
hazard clears, not redirected at all, so the right "next real instruction"
is `if_id_pc` unchanged. `flush` wins when both are true on the same
cycle — an interrupt landing on a load that's simultaneously the source of
a load-use hazard on the instruction behind it — matching the same
priority PC's own register now uses.

### The pattern, now four for four

This is the third and fourth instance of the identical shape within this
one phase — see `id_ex_mem_re` above for the first, and the `load_use`
vs. `flush` priority bug for the second — and by this point it's worth
stating as a rule rather than a coincidence: **a field that a bubble
leaves stale instead of clearing is invisible for as long as nothing new
ever reads it during a bubble.** Every hazard mechanism in this design was
built and verified against triggers that come from an instruction's own
decode — a data dependency, a resolved branch. An asynchronous interrupt
is the first thing in this project that can read *any* pipeline slot on
*any* cycle, bubble or not, and it turned out to be exactly the kind of
reader that finds every field nobody thought needed clearing.

**Verified** in simulation with a harness that varies switch-change timing
randomly across a 900,000,000-cycle (~12 real second) run, deliberately
sampling many different phase relationships between the main loop and the
interrupt rather than the single fixed timing any hand-written test would
use — the same reason the bugs took this long to find in the first place.
Both fixes confirmed on the physical board as of 2026-08-23: LEDs
switching consistently, no more freezing, across runs well past the
5–10 second window the second bug needed. `soc_demo.c` — GPIO, UART, and
timer interrupts running concurrently — is the closing hardware milestone
for this phase.

## Remaining gaps

- **`fence_i`** — out of scope; no instruction cache and a read-only
  `imem` means there's nothing for it to flush (see phase1.md/phase0.md).
- **Supervisor mode, PMP, delegation** — not implemented. This core is
  M/U only, and every trap lands in M-mode regardless of cause.
- **AXI `bresp`/`rresp`** — `axil_master` reads them but doesn't act on
  them (marked `TODO` in the source). A bus error on a bad address
  currently just completes as if it had succeeded.
- **UART overrun/framing recovery** — the sticky `STATUS` bits exist and
  are tested at the RTL level, but no software driver acts on them beyond
  this phase's own test code.
- **One interrupt source.** `mie`/`mip` only implement bit 7
  (`MTIE`/`MTIP`); external and software interrupts aren't wired to
  anything.

---

## Final results

| | |
|---|---|
| riscv-tests | 41 / 42 `rv32ui-p-*` |
| Unit tests | 13 testbenches, 182,437 checks |
| Clock | 75 MHz (MMCM from the board's 100 MHz) |
| Post-route WNS | +1.827 ns |
| Failing endpoints | 0 of 4461 |
| Hold (WHS) | +0.022 ns |
| Achievable Fmax | ~87 MHz |
| On hardware | GPIO, UART and timer interrupts running concurrently |

Timing measured with `soc_demo.hex` loaded — the program that runs the
switch-to-LED loop while a one-second timer interrupt prints over UART. Worth
noting that Fmax is program-dependent: the same design measured +1.092 ns with
`hello.c` and 4,504 endpoints, because constant propagation removes different
logic depending on which instructions a program contains. The honest claim is
that the design closes 75 MHz with margin across the programs tested.

`soc_demo` is also the first program to exercise two independent, concurrent
sources of bus traffic — the main loop's GPIO accesses and the interrupt
handler's UART writes — rather than testing each peripheral in isolation.
