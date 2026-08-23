#!/usr/bin/env bash
# Build every rv32ui-p-* test, run it on the core, report pass/fail.
set -u

ISA=~/rv32i-core/tests/riscv-tests/isa
WAVES=~/rv32i-core/sim/waves
EXE=~/rv32i-core/sim/verilator/obj_dir/soc/Vsoc
MAKEHEX=~/rv32i-core/sw/common/makehex.py

pass=0; fail=0; failed=()

for elf in "$ISA"/rv32ui-p-*; do
    case "$elf" in *.dump|*.bin|*.hex) continue;; esac
    name=$(basename "$elf")

    riscv-none-elf-objcopy -O binary "$elf" "$WAVES/$name.bin"
    python3 "$MAKEHEX" "$WAVES/$name.bin" > "$WAVES/$name.hex"

    out=$(cd "$WAVES" && "$EXE" "+imem=$name.hex" "+dmem=$name.hex" "+trace=$name.csv" 2>&1)

    if echo "$out" | grep -q "PASS via tohost"; then
        printf "  PASS  %s\n" "$name"
        pass=$((pass+1))
    else
        printf "  FAIL  %s  %s\n" "$name" "$(echo "$out" | grep -E 'FAIL|TIMEOUT' | head -1)"
        fail=$((fail+1)); failed+=("$name")
    fi
done

echo
echo "$pass passed, $fail failed"
[ $fail -gt 0 ] && printf 'failed: %s\n' "${failed[*]}"
exit $((fail > 0))
