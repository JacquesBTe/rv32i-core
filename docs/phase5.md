# Phase 5 — SoC: AXI4-Lite bus, GPIO, UART, timer, interrupts

## What was built

The core (Phases 0–4) is wrapped in an SoC: an AXI4-Lite bus, three
memory-mapped peripherals, and the CSR/interrupt plumbing to use them.

- `rtl/soc/axil_master.v` — turns the MEM stage's one load/store into one
  or two AXI4-Lite transactions and drives `bus_stall` until they finish.
  Absorbs the byte-lane logic (`wstrb`, write shift, read shift/sign-extend)
  that used to live in `dmem`. Splits a misaligned access that crosses a
  word boundary into two word-aligned transactions and combines them
  transparently — see "Misaligned access" below.
- `rtl/soc/axil_mem.v` — wraps `dmem` (now a plain word memory behind
  `wstrb`) in `axil_reg_if`, with one extra wait cycle on reads to match
  `dmem`'s registered read latency.
- `rtl/soc/gpio.v`, `rtl/soc/uart_axil.v`, `rtl/soc/timer.v` — peripherals,
  each a thin `axil_reg_if` wrapper. GPIO and timer are register files with
  no real latency (`wait` tied low, `ack` mirrors `en`). UART reads never
  stall (return whatever's in the receive register; software is expected to
  poll `STATUS.rx_valid` first) but writes do stall until the transmitter
  is ready, so a busy UART blocks a write instead of dropping the byte.
- `rtl/soc/soc.v` — wires `core`, `axil_master`, the vendored
  `axil_interconnect` (1 master, 4 slaves), and the three peripherals
  together; routes the timer's IRQ line into `core`.
- `rtl/core/core.v` — MEM stage no longer talks to a local `dmem`; it
  exposes `mem_addr`/`mem_wdata`/`mem_funct3`/`mem_re`/`mem_we`/`mem_rdata`
  as ports that `axil_master` sits behind. Every pipeline register now
  freezes on `bus_stall` (highest priority, ahead of flush/load_use) so an
  in-flight transaction can hold the pipeline for an unbounded number of
  cycles without corrupting anything downstream.
- `rtl/core/csr.v` — `mie`/`mip`/`mstatus` gained real semantics (MTIE,
  MTIP wired from the timer, MIE/MPIE/MPP with proper trap-entry and
  `mret` save/restore) and a 1-bit M/U privilege mode, replacing the old
  "accept the write, read zero" placeholders. `ecall`'s cause is now 11
  from M-mode or 8 from U-mode instead of hardcoded 11.

## Memory map

```
0x8000_0000   dmem   64 KB  (addr[31] == 1)
0x1000_0000   gpio    4 KB
0x1000_1000   uart    4 KB
0x1000_2000   timer   4 KB
```

`imem` is not on the bus — the core stays Harvard-split, instruction fetch
is unchanged from Phase 4.

## Bus decisions and why

**AXI4-Lite, no bursts, no IDs, no out-of-order.** The core issues one
transaction at a time; there was never a reason to pay for anything more.

**The bus can stall the pipeline for an unbounded number of cycles.**
`bus_stall` sits at the top of every pipeline register's priority (above
flush, above load_use) because a transaction physically in flight can't be
abandoned mid-flight. This surfaced two real, previously-dormant bugs while
building the interrupt path (step 11):

- The PC register's own priority checked `load_use` before `flush` —
  backwards from every other pipeline register, which all check flush
  first. It was unreachable before interrupts existed: a synchronous trap
  (illegal instruction, ecall) can never also be the load half of a
  load-use hazard, since neither sets `mem_re`. An interrupt can land on
  *any* instruction, including a load whose result feeds the next one —
  and when that coincided, `load_use` won, PC just held, and the redirect
  to `mtvec` was silently dropped while IF/ID and ID/EX still bubbled for
  the flush. `mepc`/`mcause`/`mstatus` all updated as if the trap were
  taken; PC never moved. Fixed by checking `pc_redirect` before
  `load_use`, matching every other register.
- `id_ex_mem_re` was never cleared on reset or on a flush/load_use bubble
  (unlike `id_ex_mem_we`, which was). Harmless as long as nothing
  downstream looked at it during a bubble — until the first pass at
  misalignment detection combined it with the bubble's equally-stale
  `alu_result` and produced spurious faults on ordinary, aligned loads.

**Read timing changed once `dmem` moved behind the bus.** The old
directly-attached `dmem` had its own one-cycle registered read latency,
which happened to line up exactly with the EX/MEM → MEM/WB pipeline
transition, so `wb_data` could read a live `mem_rdata` wire straight from
WB with no register of its own. `axil_master`'s `mem_rdata` is valid
combinationally on the response cycle instead — still the cycle EX/MEM
holds the load, one cycle *before* MEM/WB. `core.v` now captures it into a
new `mem_wb_rdata` register at the same edge the load moves into MEM/WB,
rather than relying on that coincidence.

**Misaligned access: native support, not a trap.** The original plan was
to trap on misalignment (`mcause` 4/6) and stop there. That's implemented
and tested at the RTL level, but it can't pass `rv32ui-p-ma_data`: that
test issues genuinely misaligned loads/stores and directly checks the
value, with no trap handler of its own to catch an exception. riscv-tests'
default `-p` environment treats anything but `ecall` as unhandled and
fails the test. `ma_data` fundamentally requires the access to just work —
which is also how Spike (the reference this project's traces are checked
against) behaves, since it services misaligned accesses in software
regardless of real hardware limits. Confirmed the redirect with the user
before building it: `axil_master` now splits a load/store that crosses a
word boundary into two word-aligned AXI transactions and combines them
transparently, so software never sees a fault at all.

**Interrupt injection.** Taken at the EX stage boundary against whatever
instruction is currently there — writes suppressed, PC (not PC+4) saved to
`mepc` since the instruction never completed, cause set to `0x8000_0007`,
redirect to `mtvec`. A synchronous trap on that instruction outranks the
interrupt (it genuinely faulted); neither fires while `bus_stall` is high.

## Test count

`tests/run_tests.sh`: **41 / 42** `rv32ui-p-*` tests pass. The only
failure is `fence_i`, which is out of scope for this core: `fence.i` exists
to flush an instruction cache after self-modifying code, and this core has
no instruction cache and a read-only `imem` — there's nothing for it to do.

All unit-level testbenches (`make test-all`) pass: 12 modules, ~176,000
directed and randomized checks combined.

## Timing

**Pending a fresh post-route run.** The design has changed substantially
since the last timing check (that one — WNS +0.538 ns at 75 MHz, 2,956
endpoints — predates the AXI4-Lite bus, all three peripherals, the
interrupt path, and native misalignment splitting). The address-decode
logic the bus adds sits in the MEM path, which was already the tightest
part of the design, so this needs re-verification before treating 75 MHz
as safe. Report the actual WNS/Fmax from the next Vivado implementation
run here rather than assuming the old number still holds.

## Remaining gaps

- **`fence_i`** — out of scope, see above.
- **Supervisor mode, PMP, delegation** — not implemented; this core is
  M/U only and all traps land in M-mode.
- **AXI `bresp`/`rresp` (SLVERR/DECERR)** — `axil_master` reads them but
  doesn't act on them (`TODO` in the source). A bus error on a bad address
  currently just completes as if it succeeded.
- **UART overrun/framing recovery** — the sticky STATUS bits exist and are
  tested, but there's no software driver yet that acts on them beyond the
  tests written for this phase.
- **Interrupt sources** — only the machine timer. `mie`/`mip` only
  implement bit 7 (MTIE/MTIP); external and software interrupts aren't
  wired to anything.
