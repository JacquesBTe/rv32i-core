#include "tb_common.h"
#include "Vselftest.h"

int main(int argc, char** argv) {
    Tb<Vselftest> tb(argc, argv, "selftest");
    auto& d = tb.dut;

    tb_begin("reset clears counter");
    d.rst_n = 0; d.en = 1; d.clk = 0;
    tb.tick(); tb.tick();
    CHECK_EQ(d.count, 0);

    tb_begin("counts while enabled");
    d.rst_n = 1;
    for (int i = 1; i <= 5; i++) { tb.tick(); CHECK_EQ(d.count, i); }

    tb_begin("holds while disabled");
    d.en = 0;
    tb.tick(); tb.tick();
    CHECK_EQ(d.count, 5);

    tb_begin("combinational output tracks state");
    CHECK_EQ(d.is_max, 0);

    return tb.finish();
}
