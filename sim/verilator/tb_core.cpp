#include "tb_common.h"
#include "Vcore.h"
#include <fstream>
#include <iomanip>

int main(int argc, char** argv) {
    // A tiny program, hand-assembled. imem loads this at elaboration.
    const char* HEX = "core_test.hex";
    {
        std::ofstream f(HEX);
        uint32_t prog[] = {
            0x00500093,   // addi x1, x0, 5
            0x00700113,   // addi x2, x0, 7
            0x002081B3,   // add  x3, x1, x2      -> 12
            0x40208233,   // sub  x4, x1, x2      -> -2
            0x0020F2B3,   // and  x5, x1, x2      -> 5
            0x0020E333,   // or   x6, x1, x2      -> 7
            0x0020C3B3,   // xor  x7, x1, x2      -> 2
            0x0000006F,   // jal  x0, 0           (infinite loop)
        };
        for (uint32_t w : prog)
            f << std::hex << std::setw(8) << std::setfill('0') << w << "\n";
        for (int i = sizeof(prog)/4; i < 16384; i++)
            f << "00000000\n";
    }

    Tb<Vcore> tb(argc, argv, "core");
    auto& d = tb.dut;

    d.clk = 0; d.rst_n = 0;
    tb.tick(); tb.tick();
    d.rst_n = 1;

    // Run, printing a trace line per cycle.
    printf("cycle  pc        instr     rd  wdata\n");
    for (int i = 0; i < 20; i++) {
        printf("%4d   %08x  %08x  ", i, d.trace_pc, d.trace_instr);
        if (d.trace_we) printf("x%-2d %08x\n", d.trace_rd, d.trace_wdata);
        else            printf(" -  --------\n");
        tb.tick();
    }

    return tb.finish();
}
