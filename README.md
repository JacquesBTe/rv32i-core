# rv32i-core

An RV32I RISC-V CPU, written from scratch, wrapped into a small SoC with
GPIO, a UART, and a machine-timer interrupt, running on a Digilent Basys 3
(Xilinx Artix-7 `xc7a35tcpg236-1`). The synthesizable RTL is Verilog-2001
style throughout (`` `default_nettype none``, no SystemVerilog types in
anything that reaches synthesis); a couple of simulation-only conveniences
in `dmem.v`/`imem.v` use SystemVerilog's `string` type, but only inside
`// synthesis translate_off` blocks. Simulated and unit-tested with
Verilator, correctness-checked against Spike's instruction trace,
synthesized and implemented with Vivado 2025.2.

## Results

| | |
|---|---|
| ISA compliance | 41 / 42 `rv32ui-p-*` (riscv-tests). Only failure: `fence_i`, out of scope — no icache, no unified address space (see Known limitations). |
| Unit tests | 13 Verilator testbenches, 182,437 directed + randomized checks, all pass (`sim/verilator`, `make test-all`). |
| Clock | 75 MHz, from the Basys 3's 100 MHz oscillator via an MMCM (`clk_wiz_0`). |
| Post-route timing | WNS +1.092 ns, WHS +0.057 ns, 0 of 4,504 endpoints failing. Achievable Fmax ≈ 82 MHz. |
| Hardware | Confirmed on a real board as of 2026-08-23: GPIO (switches → LEDs), UART (`hello.c`), and the full SoC — GPIO + UART + timer interrupts running concurrently (`soc_demo.c`). See `docs/phase5.md`. |

## Architecture

A classic 5-stage pipeline — IF, ID, EX, MEM, WB — with full forwarding
(EX/MEM and MEM/WB, both operands), a load-use stall, and a 2-cycle flush
on any taken branch, jump, trap, or `mret`. Instruction fetch is a
directly-attached, read-only, synchronous BRAM (`imem`) — the core stays
Harvard-split and `imem` never goes through the bus. Everything else — the
data memory and three peripherals — sits behind a single-outstanding
AXI4-Lite bus:

```
core (MEM stage) -- axil_master -- axil_interconnect (1x4) --+-- dmem   0x8000_0000  64 KB
                                                               +-- gpio   0x1000_0000  4 KB
                                                               +-- uart   0x1000_1000  4 KB
                                                               +-- timer  0x1000_2000  4 KB
```

`axil_master` turns one MEM-stage load or store into one AXI4-Lite
transaction (or two, transparently, if the access is misaligned across a
word boundary — see `docs/phase5.md`), and drives `bus_stall` for as long
as it takes; every pipeline register freezes on `bus_stall`, at higher
priority than a flush or a load-use stall, since a transaction physically
in flight can't be abandoned. The interconnect and the AXI4-Lite register
wrapper (`axil_reg_if`) are vendored from Alex Forencich's `verilog-axi`
(MIT); the UART's RX/TX shift-register logic is vendored from his
`verilog-uart` (MIT) — both under `rtl/vendor/`, each with its own
`LICENSE` file. Everything else under `rtl/core/` and `rtl/soc/` is
original to this project.

CSR/trap support is machine-mode, plus a 1-bit M/U privilege field:
`mstatus` (MIE/MPIE/MPP), `mie`/`mip` (timer bit only), `mtvec`, `mepc`,
`mcause`, `mscratch`, read-only `mhartid`. Every other CSR address either
reads zero and accepts writes (WARL — `misa`, `medeleg`, `mideleg`,
`mtval`, `satp`, `pmpcfg0`, `pmpaddr0`) or traps as illegal. Interrupts are
taken at the EX stage boundary, against whatever instruction is there —
its PC (not PC+4, since it never completed) is saved to `mepc` and control
redirects to `mtvec`; a synchronous trap on that same instruction always
outranks the interrupt.

## Repository layout

```
rtl/
  core/       core.v and everything upstream/downstream of it: alu, regfile,
              decoder, immgen, branch_cmp, csr, imem, dmem
  soc/        the bus-side wrapper: axil_master, the three peripherals
              (gpio, uart_axil, timer), soc.v tying it all together
  vendor/     verilog-axi and verilog-uart (MIT, Alex Forencich)
  include/    rv32i_defs.vh -- ALU opcode macros shared by decoder.v and alu.v
  fpga/       basys3_top.v (board top level), basys3.xdc (pin/timing
              constraints), and the .hex memory images basys3_top.v points at
sim/verilator/ one Makefile driving every testbench (tb_<module>.cpp) plus
              tb_common.h, the shared Tb<DUT>/CHECK_EQ template every
              testbench uses
sw/           bare-metal C examples (sw/examples/*.c) built by sw/Makefile
              into sw/build/*.hex; sw/common/ has the linker script,
              crt0.S, and the ELF->hex conversion script all of them share
tests/        tests/riscv-tests (submodule) + run_tests.sh (build and run
              every rv32ui-p-* test against soc) + trace_diff.py (Spike
              trace vs. core trace, see docs/phase0.md)
formal/       formal/riscv-formal (submodule) -- vendored, not yet
              integrated; see docs/phase6_plan.md
docs/         phase-by-phase design history and forward plans (this project
              was built in named phases; see Phase structure below)
```

One stray note: a top-level `fpga/` directory (distinct from `rtl/fpga/`)
exists in the repo and is empty — it predates `rtl/fpga/` and appears to be
left over rather than in current use.

## Building and running

**Software.** Needs `riscv-none-elf-gcc` (xPack toolchain; targets
`rv32i_zicsr`/`ilp32`) on `PATH`.

```
cd sw && make            # builds every sw/examples/*.c into sw/build/*.hex
```

**Unit tests.** Needs Verilator (this project uses the OSS CAD Suite
build; see `docs/phase0.md` for exact versions).

```
cd sim/verilator
make test-all            # all 13 module testbenches
make run MODULE=core     # one module; MODULE=soc is not in test-all --
                          # its own coverage is tests/run_tests.sh, below
```

**Full ISA compliance sweep.** Needs the `tests/riscv-tests` submodule
initialized (`git submodule update --init tests/riscv-tests`) and
`riscv-none-elf-objcopy`/`riscv-none-elf-nm` on `PATH`, in addition to
Verilator.

```
tests/run_tests.sh       # builds soc's Verilator model if needed, then
                          # runs every rv32ui-p-* test against it
```

**FPGA.** `rtl/fpga/basys3_top.v` is the board top level and
`rtl/fpga/basys3.xdc` the Basys 3 pin/timing constraints, both meant to be
added to a Vivado project. Two things aren't checked into this repository
and have to be supplied by the Vivado project itself:

- A `clk_wiz_0` Clocking Wizard IP core (100 MHz in, 75 MHz out) —
  `basys3_top.v` instantiates it by name but its source isn't vendored
  here; it has to be generated from Vivado's IP catalog.
- `basys3_top.v`'s `soc` instance currently hardcodes `IMEM_INIT`/
  `DMEM_INIT` to an absolute path on the original development machine
  (`C:/Users/jacqu/Desktop/rv32i-core/...`) — this needs to be repointed
  at a real path (one of `rtl/fpga/*.hex`) before it will elaborate
  anywhere else. `core.v`'s own default `IMEM_INIT` parameter has the same
  problem, with a different hardcoded path.

## Verification

Two independent methods, used for different things:

- **Spike trace comparison** (`tests/trace_diff.py`, established in
  `docs/phase0.md`) — converts a Spike `--log-commits` run into this
  project's own retired-instruction CSV format (`pc,instr,rd,wdata`) and
  diffs it against the core's own trace output, reporting the first
  divergence with surrounding context on both sides. This is what
  validated the toolchain and the trace format itself before any RTL
  existed.
- **riscv-tests + directed/randomized unit tests** — `tests/run_tests.sh`
  runs the official `rv32ui-p-*` suite against the full `soc` model via
  its HTIF `tohost` exit protocol; `sim/verilator`'s per-module
  testbenches (built on the shared `Tb<DUT>` template in `tb_common.h`)
  combine hand-written directed cases with randomized runs checked against
  a software shadow model, the same pattern established for `regfile` in
  Phase 1 and reused for every module added since.

Hardware confirmation is tracked separately in `docs/phase5.md` — passing
simulation is not treated as equivalent to hardware-verified in this
project's own history; two real bugs (documented there) were found only
by running on the physical board at realistic timing, after passing every
simulated test that existed at the time.

## Known limitations

- **`fence_i` unimplemented** — this core has no instruction cache and a
  read-only `imem`, so there's nothing for it to flush; the one riscv-tests
  failure.
- **No supervisor mode, no PMP, no delegation** — machine/user privilege
  only, and every trap lands in M-mode regardless of cause.
- **AXI `bresp`/`rresp` unused** — `axil_master` reads them but doesn't
  act on them; a bus error on a bad address currently completes as if it
  had succeeded.
- **One interrupt source** — only the machine timer (`mie`/`mip` implement
  bit 7 only); external and software interrupts aren't wired to anything.
- **UART overrun/framing recovery** — the sticky status bits exist and are
  tested at the RTL level, but no software driver acts on them beyond this
  project's own test code.
- **`formal/riscv-formal` is vendored but unused** — no core integration
  exists yet; see `docs/phase6_plan.md`.
- **No branch prediction, no cache** — every taken branch/jump/trap/`mret`
  costs a fixed 2-cycle flush, and every load/store now costs at least one
  `bus_stall` cycle for the AXI4-Lite handshake since `dmem` moved behind
  the bus in Phase 5; see `docs/phase7_plan.md`.

## Phase structure

This project was built in named phases, each documented after the fact
(`docs/phase0.md` through `docs/phase5.md` are retroactive; `phase6_plan.md`
and `phase7_plan.md` describe work that hasn't started):

| Phase | What | Status |
|---|---|---|
| 0 | Toolchain, sw build flow, Spike trace harness | Complete |
| 1 | Register file | Complete |
| 2 | Single-cycle core | Complete |
| 3 | Pipeline registers | Folded into Phase 4 — no separate commit exists; see `docs/phase3.md` |
| 4 | Forwarding, load-use stall, branch flush | Complete |
| 5 | SoC: AXI4-Lite bus, GPIO, UART, timer, interrupts | Complete |
| 6 | Formal verification (riscv-formal) | Planned, not started |
| 7 | Performance: branch prediction, caching, CoreMark, custom instruction | Planned, not started |
