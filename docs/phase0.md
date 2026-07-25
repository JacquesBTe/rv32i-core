# Phase 0 — Toolchain & Verification Harness (complete)

## Toolchain
- GCC: xPack riscv-none-elf-gcc 15.2.0 (~/tools/xpack-riscv-none-elf-gcc-15.2.0-1)
  - Prefix: riscv-none-elf-  |  ARCH=rv32i_zicsr  ABI=ilp32
  - Verified: rv32i/ilp32 multilib present; libgcc resolves to rv32i/ilp32/libgcc.a;
    __mulsi3 disassembles to pure 32-bit RV32I (no compressed, no mul)
  - Rejected: riscv-collab riscv32-elf-ubuntu-24.04 build — single-lib (.;),
    libgcc contained compressed (C-ext) encodings; would trap on an RV32I-only core
- Spike: riscv-isa-sim, built from source, prefix ~/tools/spike
- OSS CAD Suite 2026-03-08 (Verilator 5.047, Yosys, SymbiYosys, GTKWave)
  - Loaded on demand via `eda` alias; shells show a prompt prefix when active

## Conventions
- Link base 0x80000000, 64K RAM (matches Spike default DRAM + riscv-tests)
- Exit via HTIF tohost: (code << 1) | 1
- ISA string everywhere: rv32i_zicsr (GCC>=12 split CSRs out of base ISA)
- Spike run: spike --isa=rv32i_zicsr -m0x80000000:0x10000 --log-commits <elf> 2> log

## Trace harness
- tests/trace_diff.py: convert (Spike log -> core CSV) and diff modes
- Skips Spike boot ROM (drops retires with PC < 0x80000000; core resets at 0x80000000)
- Core trace format: pc,instr,rd,wdata (hex; "-,-" for no GPR writeback); x0 writes dropped
- Validated: Spike-vs-Spike PASS (4995 instrs); mutation detected at correct index
  with +/-5 context and exit code 1; line-number provenance to raw Spike log preserved

## Verified
- sw/ flow: C -> ELF -> bin -> $readmemh hex, disassembly per build; __mulsi3 present
  (note: both multiply operands must be volatile or GCC constant-folds / strength-reduces)
- rv32ui-p-add built with RISCV_PREFIX=riscv-none-elf- XLEN=32, passes on Spike
- Verilator compiles a trivial module (--binary --timing)
