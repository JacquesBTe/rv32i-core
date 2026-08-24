# Phase 4 — Hazard handling: forwarding, load-use stall, branch flush (complete)

One commit, `ffe21a5`, 2026-08-18, titled "Phase 4: forwarding, load-use
stall, branch flush -- 39/42 rv32ui-p pass." As covered in phase3.md, this
is the same commit that introduced the pipeline registers themselves —
there was no prior hazard-free pipelined state to compare against. This
document covers the hazard logic and the CSR/trap groundwork that shipped
alongside it.

## Forwarding

Same three-term shape as `regfile`'s own write-first bypass (phase1.md):
a source stage writes a register (`ex_mem_reg_we` / `mem_wb_reg_we`), its
destination matches the reader's source address, and that address isn't
x0. Two source registers (`rs1`, `rs2`) and two possible sources (EX/MEM,
MEM/WB) give four forwarding signals — `fwd_a_mem`, `fwd_a_wb`,
`fwd_b_mem`, `fwd_b_wb`. Priority is MEM before WB: if both match, MEM
holds the more recently-issued write and is the one the program means.
`fwd_rs1`/`fwd_rs2` feed the ALU, `branch_cmp`, and (see below) the CSR
write-data path — everywhere a register value is needed in EX, it goes
through the forwarding mux rather than reading `id_ex_rs1_data`/
`id_ex_rs2_data` directly.

`ex_mem_wb_value` — what EX/MEM would actually write back if it reached
WB — has to be computed the same way the real writeback mux computes it
(CSR read data, or PC+4 for a jump-and-link, or the ALU result), since a
dependent instruction one stage behind needs the value MEM is *about* to
produce, not a placeholder.

## Load-use stall

```verilog
wire load_use = id_ex_mem_re
            && (id_ex_rd_addr != 5'b0)
            && ((id_ex_rd_addr == rs1_addr) || (id_ex_rd_addr == rs2_addr));
```

A load's result isn't available until it's in MEM — one stage later than
an ALU result, which forwarding can already cover from EX/MEM. When the
instruction currently in ID needs a register that the instruction ahead of
it in ID/EX is loading, there's nothing to forward yet; the pipeline has
to stall for one cycle. `load_use` holds `pc` and `if_id` in place (the
same instruction is re-presented to ID next cycle) and forces `id_ex` into
a bubble — a NOP with every control signal cleared — using the same
`flush || load_use` branch as an actual pipeline flush (see below), since
either way ID/EX must not let a stale or repeated instruction execute.

## Branch flush

```verilog
assign pc_redirect = trap || id_ex_is_mret || id_ex_is_jal || id_ex_is_jalr
                     || (id_ex_is_branch && branch_taken);
```

All five reasons IF should stop fetching sequentially — a resolved taken
branch, `jal`/`jalr` (always taken), `mret`, and a trap — are resolved at
the EX stage boundary and folded into one `pc_redirect` signal.
`pc_target` is `trap ? mtvec_out : id_ex_is_mret ? mepc_out : id_ex_is_jalr
? (alu_result & ~32'd1) : (id_ex_pc + id_ex_imm)`. `flush` (an alias for
`pc_redirect`) nulls both IF/ID (`if_id_instr <= 32'h00000013`, a real
`addi x0,x0,0`) and ID/EX on the same edge PC redirects — two
instructions are always discarded on any taken branch/jump/trap/`mret`,
since IF and ID were both already committed to the pre-redirect path.

One priority detail worth flagging here since it becomes a real bug later:
the PC register's own always-block checks `load_use` and holds on it,
falling through to `pc_next` (which already embeds `pc_redirect`)
otherwise — there's no explicit "flush wins" branch for PC the way IF/ID
and ID/EX both have. That's harmless in this phase: nothing that can cause
`load_use` can also be the source of a `pc_redirect` yet, since traps and
`mret` only come from instructions that don't set `mem_re`, and branches/
jumps don't write a register a *following* load could depend on in the
hazard sense `load_use` checks. It stops being harmless once Phase 5 adds
externally-timed interrupts, which can land on any instruction including
a load — see phase5.md for the bug that surfaced and its fix.

## CSR and trap groundwork

`csr.v` is new this commit: six registers (`mstatus`, `mtvec`, `mscratch`,
`mepc`, `mcause`, plus read-only `mhartid`), three access modes (`csrrw`/
`csrrs`/`csrrc`, and their immediate forms via `csr_use_imm`), and a
`csr_ignored` list — `misa`, `medeleg`, `mideleg`, `mie`, `mtval`, `mip`,
`satp`, `pmpcfg0`, `pmpaddr0` — addresses that are recognized but not
backed by real state: writes are accepted and reads return zero, which is
legal WARL (Write Any values, Read Legal values) behavior and avoids
spurious illegal-instruction traps when riscv-tests' startup code touches
them. Anything else unrecognized sets `csr_illegal`. The file's own
opening comment lists what's deliberately still missing: "mie/mip,
interrupt delegation, counters, privilege modes, CSR write-permission
checks" — explicitly deferred, at the time, to "phase 5."

`decoder.v` gains real `SYSTEM`-opcode decoding in place of Phase 2's
empty case: `funct3 == 000` distinguishes `ecall` (`is_ecall`), `ebreak`
(routed to the same trap path as `ecall`), and `mret` (encoding `12'h302`)
from an otherwise-illegal `SYSTEM` instruction; every other `funct3`
decodes as a CSR instruction, with `csr_use_imm = funct3[2]` selecting the
immediate forms and `funct3[1:0]` selecting `csrrw`/`csrrs`/`csrrc`. Two
narrow optimizations sit in the decode logic directly: `csrrw` with
`rd = x0` sets `csr_re = 0` (nothing needs the old value if it's being
discarded), and `csrrs`/`csrrc` with `rs1 = x0` sets `csr_we = 0` (setting
or clearing bits with an all-zero mask changes nothing, so there's no
write to perform).

`trap = id_ex_illegal || csr_illegal || id_ex_is_ecall`, and `trap_cause`
is `id_ex_is_ecall ? 32'd11 : 32'd2` — 11 is the RISC-V cause code for an
environment call from M-mode (the only mode that exists yet), 2 is illegal
instruction. `reg_we_final`/`mem_we_final` gate the real `reg_we`/`mem_we`
with `!trap`, so a faulting instruction commits no side effects even
though it's already progressed through EX by the time the trap is
detected. In `csr.v` itself, trap entry is given priority over any
instruction-side CSR write reaching the same cycle, since a faulting
instruction must not be allowed to commit its own CSR update on the way
out.

## Verified

39/42 `rv32ui-p-*` pass, per this commit's own message. (Two of the three
remaining failures at this point — `ld_st` and `ma_data` — are fixed in
Phase 5, steps 10 and 12 respectively; `fence_i` remains out of scope
today, see phase5.md.) I did not rebuild this exact historical commit to
re-derive that number independently — it's taken directly from the commit
message, which is as close to primary-source verification as a retroactive
count of a past state can get without altering the working tree to check
out old history.
