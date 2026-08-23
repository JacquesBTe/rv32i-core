#include "tb_common.h"
#include "Vtimer.h"
#include <cstdint>

enum { REG_MTIME_LO = 0x0, REG_MTIME_HI = 0x4,
       REG_MTIMECMP_LO = 0x8, REG_MTIMECMP_HI = 0xC };

template <class TB>
static void axil_write(TB& tb, uint32_t addr, uint32_t data, uint8_t strb = 0xF) {
    auto& d = tb.dut;
    d.s_axil_awaddr = addr; d.s_axil_awprot = 0; d.s_axil_awvalid = 1;
    d.s_axil_wdata  = data; d.s_axil_wstrb  = strb; d.s_axil_wvalid = 1;
    d.s_axil_bready = 1;
    bool aw_acc = false, w_acc = false, got_b = false;
    for (int i = 0; i < 20 && !got_b; i++) {
        tb.dut.eval();
        if (!aw_acc && d.s_axil_awready) aw_acc = true;
        if (!w_acc  && d.s_axil_wready)  w_acc  = true;
        bool bv = d.s_axil_bvalid;
        tb.tick();
        if (aw_acc) d.s_axil_awvalid = 0;
        if (w_acc)  d.s_axil_wvalid  = 0;
        if (bv) got_b = true;
    }
    d.s_axil_bready = 0;
}

template <class TB>
static uint32_t axil_read(TB& tb, uint32_t addr) {
    auto& d = tb.dut;
    d.s_axil_araddr = addr; d.s_axil_arprot = 0; d.s_axil_arvalid = 1;
    d.s_axil_rready = 1;
    bool ar_acc = false, got_r = false;
    uint32_t data = 0;
    for (int i = 0; i < 20 && !got_r; i++) {
        tb.dut.eval();
        if (!ar_acc && d.s_axil_arready) ar_acc = true;
        bool rv = d.s_axil_rvalid;
        if (rv) data = d.s_axil_rdata;
        tb.tick();
        if (ar_acc) d.s_axil_arvalid = 0;
        if (rv) got_r = true;
    }
    d.s_axil_rready = 0;
    return data;
}

template <class TB>
static uint64_t read_mtime(TB& tb) {
    // Read the low half first, then high -- there's a rollover race in
    // principle (mtime can carry from lo to hi between the two reads),
    // but at these tick counts it never fires; a real driver would
    // re-read hi/lo/hi and retry on mismatch.
    uint32_t lo = axil_read(tb, REG_MTIME_LO);
    uint32_t hi = axil_read(tb, REG_MTIME_HI);
    return ((uint64_t)hi << 32) | lo;
}

int main(int argc, char** argv) {
    Tb<Vtimer> tb(argc, argv, "timer");
    auto& d = tb.dut;

    d.clk = 0; d.rst_n = 0;
    d.s_axil_awvalid = 0; d.s_axil_wvalid = 0; d.s_axil_bready = 0;
    d.s_axil_arvalid = 0; d.s_axil_rready = 0;
    tb.tick(); tb.tick();
    d.rst_n = 1;

    tb_begin("1. mtime free-runs from 0, mtimecmp resets to all-ones");
    // mtime is already ticking by the time the read completes -- check
    // it's small (a couple of AXI-Lite handshake cycles), not exactly 0.
    CHECK_EQ(axil_read(tb, REG_MTIME_LO) < 10, true);
    CHECK_EQ(axil_read(tb, REG_MTIMECMP_LO), 0xFFFFFFFF);
    CHECK_EQ(axil_read(tb, REG_MTIMECMP_HI), 0xFFFFFFFF);
    CHECK_EQ(d.timer_irq, 0);

    tb_begin("2. mtime advances by roughly the number of cycles elapsed");
    uint64_t t0 = read_mtime(tb);
    for (int i = 0; i < 100; i++) tb.tick();
    uint64_t t1 = read_mtime(tb);
    // Each axil_read/write burns a handful of cycles too, so check a
    // generous window rather than an exact count.
    CHECK_EQ(t1 > t0, true);
    CHECK_EQ(t1 - t0 >= 100, true);
    CHECK_EQ(t1 - t0 < 200, true);

    tb_begin("3. timer_irq asserts when mtime reaches mtimecmp");
    uint64_t now = read_mtime(tb);
    uint64_t target = now + 20;
    axil_write(tb, REG_MTIMECMP_LO, (uint32_t)target);
    axil_write(tb, REG_MTIMECMP_HI, (uint32_t)(target >> 32));
    CHECK_EQ(d.timer_irq, 0);

    int guard = 0;
    while (!d.timer_irq && guard < 200) { tb.tick(); guard++; }
    CHECK_EQ(d.timer_irq, 1);
    uint64_t fired_at = read_mtime(tb);
    CHECK_EQ(fired_at >= target, true);

    tb_begin("4. re-arming mtimecmp forward clears the interrupt");
    uint64_t now2 = read_mtime(tb);
    uint64_t target2 = now2 + 30;
    axil_write(tb, REG_MTIMECMP_LO, (uint32_t)target2);
    axil_write(tb, REG_MTIMECMP_HI, (uint32_t)(target2 >> 32));
    CHECK_EQ(d.timer_irq, 0);
    guard = 0;
    while (!d.timer_irq && guard < 200) { tb.tick(); guard++; }
    CHECK_EQ(d.timer_irq, 1);

    tb_begin("5. writing mtimecmp back below mtime keeps irq asserted");
    axil_write(tb, REG_MTIMECMP_LO, 0);
    axil_write(tb, REG_MTIMECMP_HI, 0);
    CHECK_EQ(d.timer_irq, 1);

    tb_begin("6. writes to mtime itself take effect");
    axil_write(tb, REG_MTIMECMP_LO, 0xFFFFFFFF);
    axil_write(tb, REG_MTIMECMP_HI, 0xFFFFFFFF);
    axil_write(tb, REG_MTIME_HI, 0x00000001);
    axil_write(tb, REG_MTIME_LO, 0x00000000);
    uint64_t after_set = read_mtime(tb);
    CHECK_EQ(after_set >= 0x100000000ULL, true);
    CHECK_EQ(after_set < 0x100000100ULL, true);

    tb_begin("7. high/low halves are independently addressable");
    axil_write(tb, REG_MTIMECMP_LO, 0x12345678);
    axil_write(tb, REG_MTIMECMP_HI, 0x9ABCDEF0);
    CHECK_EQ(axil_read(tb, REG_MTIMECMP_LO), 0x12345678);
    CHECK_EQ(axil_read(tb, REG_MTIMECMP_HI), 0x9ABCDEF0);

    return tb.finish();
}
