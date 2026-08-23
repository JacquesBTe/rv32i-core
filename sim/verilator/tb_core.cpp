#include "tb_common.h"
#include "Vcore.h"
#include <cstdio>

// riscv-tests HTIF exit protocol: the program stores a nonzero word to
// 'tohost'. Value 1 means pass; otherwise (code << 1) | 1, so the failing
// test number is value >> 1.
static const uint32_t TOHOST_ADDR = 0x80001000;
static const int      MAX_CYCLES  = 200000;

int main(int argc, char** argv) {
    const char* csv_path = "core_trace.csv";
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "+trace=", 7)) csv_path = argv[i] + 7;

    Tb<Vcore> tb(argc, argv, "core");
    auto& d = tb.dut;

    FILE* csv = fopen(csv_path, "w");
    if (!csv) { printf("[tb] cannot open %s\n", csv_path); return 2; }

    d.clk = 0; d.rst_n = 0; d.bus_stall = 0;
    tb.tick(); tb.tick();
    d.rst_n = 1;

    int      cycles   = 0;
    uint32_t tohost   = 0;
    bool     finished = false;

    while (cycles < MAX_CYCLES) {
        // Sample the trace ports before the edge -- they reflect the
        // instruction currently executing. Spike omits instructions that
        // trap, so we do the same or the traces drift out of alignment.
        if (!d.trace_trap) {
            if (d.trace_we)
                fprintf(csv, "%08x,%08x,%d,%08x\n",
                        d.trace_pc, d.trace_instr, d.trace_rd, d.trace_wdata);
            else
                fprintf(csv, "%08x,%08x,-,-\n", d.trace_pc, d.trace_instr);
        }

        // Watch for the HTIF exit store.
        if (d.trace_mem_we && d.trace_mem_addr == TOHOST_ADDR
                           && d.trace_mem_wdata != 0) {
            tohost   = d.trace_mem_wdata;
            finished = true;
            tb.tick();          // let the store commit
            break;
        }

        tb.tick();
        cycles++;
    }

    fclose(csv);

    if (!finished) {
        printf("[tb] TIMEOUT after %d cycles, pc=%08x\n", cycles, d.trace_pc);
        printf("[tb] trace written to %s\n", csv_path);
        tb_fails++;
    } else if (tohost == 1) {
        printf("[tb] PASS via tohost after %d instructions\n", cycles);
    } else {
        printf("[tb] FAIL: tohost=0x%08x -> test case %u, after %d instructions\n",
               tohost, tohost >> 1, cycles);
        tb_fails++;
    }

    return tb.finish();
}
