# Phase 3 — Pipeline registers (folded into Phase 4; see note)

## A note on this document

There is no commit in this repository's history titled or scoped as
"Phase 3." The gap between Phase 2 (`0f0afbb`, 2026-08-11, single-cycle
core) and the next core change (`ffe21a5`, 2026-08-18, titled "Phase 4:
forwarding, load-use stall, branch flush -- 39/42 rv32ui-p pass") is a
single commit that introduces the pipeline registers *and* the hazard
logic together. Phase 2's own `core.v` has a comment on the `regfile`
instantiation reading `// flip to 1 in phase 3` — so a separate Phase 3
was the original plan — but whatever happened between 2026-08-11 and
2026-08-18 didn't get committed as its own step, and the work that shipped
skipped straight to a pipeline that already has forwarding, stalling, and
flushing wired in. `grep -ri "phase 3"` across the entire repository
(source, comments, docs, commit messages) turns up nothing except that one
`core.v` comment.

This document describes the pipeline register structure that `ffe21a5`
introduced, as a structural matter — five stages, four register boundaries,
what each boundary carries. phase4.md describes the hazard logic
(forwarding, load-use stall, flush) that arrived in the exact same commit.
The split is for readability, not because these were separable milestones
in this project's actual history.

## Structure

Five stages: IF, ID, EX, MEM, WB, with a pipeline register at each
boundary (`if_id`, `id_ex`, `ex_mem`, `mem_wb`). At this point in the
project's history, `imem` and `dmem` are still the combinational modules
from Phase 2 — no `en` port, no registered read latency of their own —
so IF's own timing hasn't changed; what's new is that everything
downstream of fetch now advances one stage per cycle instead of
completing in the same cycle it started.

- **IF/ID** carries `pc` and the fetched `instr` forward.
- **ID/EX** carries every decoded field the later stages need: both
  register values, the immediate, both source addresses (for hazard
  detection in EX), the destination address, the ALU control signals, and
  — new in this commit — the CSR/trap-related decode fields (`csr_op`,
  `csr_re`, `csr_we`, `csr_use_imm`, `is_mret`, `is_ecall`), since `csr.v`
  and real ecall/mret decoding arrive in this same commit (see phase4.md).
- **EX/MEM** carries the ALU result, the store data, the destination
  address, and the writeback-select control forward for the memory stage
  and, from there, writeback.
- **MEM/WB** carries the final writeback value forward one more stage so
  the register file's write port and the trace outputs see a stable,
  fully-resolved instruction.

**The register file's read and write ports move to different stages.**
In the single-cycle core, `regfile` read rs1/rs2 and wrote rd for the same
instruction in the same cycle. In the pipeline, ID still reads (off
whatever's in IF/ID), but the write port now comes from MEM/WB —
`u_regfile`'s `rd_addr`/`rd_data`/`rd_we` are wired to `mem_wb_rd_addr`/
`wb_data`/`mem_wb_reg_we`, not to the instruction currently in ID. That
means the register a later instruction in ID wants to read may not have
been written yet by the time it gets there — the write is three stages
behind the read for back-to-back dependent instructions — which is
exactly the gap `regfile`'s `BYPASS` parameter (see phase1.md) exists to
cover. This commit is what flips it on: `regfile #(.BYPASS(1))`.

## Verified

Structural only — this document doesn't correspond to an isolated,
separately-tested commit. The 39/42 `rv32ui-p-*` pass count that first
validates this pipeline belongs to Phase 4, once forwarding and hazard
handling are in place; there's no intermediate, hazard-free pipeline state
in this repository's history to report a pass count for.
