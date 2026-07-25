#!/usr/bin/env python3
"""trace_diff.py - compare a Spike commit log against a core trace.

Usage:
  trace_diff.py convert SPIKE_LOG            # emit core-format CSV to stdout
  trace_diff.py diff SPIKE_LOG CORE_CSV      # report first divergence
  trace_diff.py diff --context N ...         # lines of context (default 5)

Core CSV format (one retired instruction per line, hex, no 0x):
  pc,instr,rd,wdata        e.g.  80000010,00100093,1,00000001
  pc,instr,-,-             for instructions with no GPR writeback
"""
import argparse
import re
import sys
from dataclasses import dataclass
from typing import Optional

SPIKE_RE = re.compile(
    r"^core\s+\d+:\s+\d+\s+0x([0-9a-fA-F]+)\s+\(0x([0-9a-fA-F]+)\)"
    r"(?:\s+x\s*(\d+)\s+0x([0-9a-fA-F]+))?"
)

@dataclass
class Retire:
    pc: int
    instr: int
    rd: Optional[int]
    wdata: Optional[int]
    line_no: int
    raw: str

    def brief(self) -> str:
        wb = f"x{self.rd}=0x{self.wdata:08x}" if self.rd is not None else "--"
        return f"pc=0x{self.pc:08x} instr=0x{self.instr:08x} {wb}"


def parse_spike(path: str) -> list[Retire]:
    out = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            m = SPIKE_RE.match(line)
            if not m:
                continue
            pc = int(m.group(1), 16)
            instr = int(m.group(2), 16)
            rd = wdata = None
            if m.group(3) is not None:
                rd = int(m.group(3))
                wdata = int(m.group(4), 16) & 0xFFFFFFFF
                if rd == 0:
                    rd = wdata = None
            out.append(Retire(pc, instr, rd, wdata, n, line.rstrip()))
    # drop Spike's boot ROM (PCs below DRAM base); core resets at 0x80000000
    for i, r in enumerate(out):
        if r.pc >= 0x80000000:
            return out[i:]
    return out


def parse_core_csv(path: str) -> list[Retire]:
    out = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) != 4:
                sys.exit(f"{path}:{n}: expected 4 fields, got {len(parts)}: {line}")
            pc = int(parts[0], 16)
            instr = int(parts[1], 16)
            rd = wdata = None
            if parts[2] != "-":
                rd = int(parts[2], 16) if parts[2].lower().startswith("0x") \
                     else int(parts[2])
                wdata = int(parts[3], 16) & 0xFFFFFFFF
                if rd == 0:
                    rd = wdata = None
            out.append(Retire(pc, instr, rd, wdata, n, line))
    return out


def emit_csv(retires: list[Retire]) -> None:
    for r in retires:
        if r.rd is None:
            print(f"{r.pc:08x},{r.instr:08x},-,-")
        else:
            print(f"{r.pc:08x},{r.instr:08x},{r.rd},{r.wdata:08x}")


def show_context(tag: str, trace: list[Retire], idx: int, ctx: int) -> None:
    lo = max(0, idx - ctx)
    hi = min(len(trace), idx + ctx + 1)
    for i in range(lo, hi):
        marker = ">>" if i == idx else "  "
        print(f"  {marker} [{tag} #{i}, line {trace[i].line_no}] {trace[i].brief()}")


def diff(ref: list[Retire], dut: list[Retire], ctx: int) -> int:
    n = min(len(ref), len(dut))
    for i in range(n):
        r, d = ref[i], dut[i]
        if (r.pc, r.instr, r.rd, r.wdata) != (d.pc, d.instr, d.rd, d.wdata):
            print(f"DIVERGENCE at retired-instruction index {i}:")
            print(f"  ref: {r.brief()}")
            print(f"  dut: {d.brief()}")
            print("\nReference context:")
            show_context("ref", ref, i, ctx)
            print("\nDUT context:")
            show_context("dut", dut, i, ctx)
            return 1
    if len(ref) != len(dut):
        print(f"LENGTH MISMATCH after {n} matching instructions: "
              f"ref has {len(ref)}, dut has {len(dut)}")
        longer, tag = (ref, "ref") if len(ref) > len(dut) else (dut, "dut")
        show_context(tag, longer, n, ctx)
        return 1
    print(f"PASS: {n} retired instructions match")
    return 0


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert")
    c.add_argument("spike_log")

    d = sub.add_parser("diff")
    d.add_argument("spike_log")
    d.add_argument("core_csv")
    d.add_argument("--context", type=int, default=5)

    args = ap.parse_args()
    if args.cmd == "convert":
        emit_csv(parse_spike(args.spike_log))
    else:
        sys.exit(diff(parse_spike(args.spike_log),
                      parse_core_csv(args.core_csv),
                      args.context))

if __name__ == "__main__":
    main()