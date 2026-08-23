#include "tb_common.h"
#include "Vaxil_mem.h"

// Same minimal AXI4-Lite master BFM as tb_gpio.cpp. axil_mem's ack is
// delayed by dmem's one cycle of read latency on the read side, so the
// iteration cap here is a little larger than gpio's.
template <class TB>
static void axil_write(TB& tb, uint32_t addr, uint32_t data, uint8_t strb) {
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

int main(int argc, char** argv) {
    Tb<Vaxil_mem> tb(argc, argv, "axil_mem");
    auto& d = tb.dut;

    d.clk = 0; d.rst_n = 0;
    d.s_axil_awvalid = 0; d.s_axil_wvalid = 0; d.s_axil_bready = 0;
    d.s_axil_arvalid = 0; d.s_axil_rready = 0;
    tb.tick(); tb.tick();
    d.rst_n = 1;

    const uint32_t BASE = 0x80000000;

    tb_begin("1. word round-trip");
    axil_write(tb, BASE + 0, 0xDEADBEEF, 0xF);
    CHECK_EQ(axil_read(tb, BASE + 0), 0xDEADBEEF);
    axil_write(tb, BASE + 4, 0x12345678, 0xF);
    CHECK_EQ(axil_read(tb, BASE + 4), 0x12345678);
    CHECK_EQ(axil_read(tb, BASE + 0), 0xDEADBEEF);   // neighbour untouched

    tb_begin("2. wstrb selects individual byte lanes");
    axil_write(tb, BASE + 8, 0x00000000, 0xF);
    axil_write(tb, BASE + 8, 0x000000AA, 0x1);
    CHECK_EQ(axil_read(tb, BASE + 8), 0x000000AA);
    axil_write(tb, BASE + 8, 0x0000BB00, 0x2);
    CHECK_EQ(axil_read(tb, BASE + 8), 0x0000BBAA);
    axil_write(tb, BASE + 8, 0x00CC0000, 0x4);
    CHECK_EQ(axil_read(tb, BASE + 8), 0x00CCBBAA);
    axil_write(tb, BASE + 8, 0xDD000000, 0x8);
    CHECK_EQ(axil_read(tb, BASE + 8), 0xDDCCBBAA);

    tb_begin("3. reads and writes to different addresses interleave cleanly");
    axil_write(tb, BASE + 100, 0x11111111, 0xF);
    axil_write(tb, BASE + 104, 0x22222222, 0xF);
    CHECK_EQ(axil_read(tb, BASE + 100), 0x11111111);
    CHECK_EQ(axil_read(tb, BASE + 104), 0x22222222);
    axil_write(tb, BASE + 100, 0x33333333, 0xF);
    CHECK_EQ(axil_read(tb, BASE + 100), 0x33333333);
    CHECK_EQ(axil_read(tb, BASE + 104), 0x22222222);

    tb_begin("4. back-to-back transactions, no idle cycle between");
    for (uint32_t i = 0; i < 50; i++) {
        uint32_t v = (i * 0x01010101u);
        axil_write(tb, BASE + 200, v, 0xF);
        CHECK_EQ(axil_read(tb, BASE + 200), v);
    }

    tb_begin("5. random against a byte-array shadow model");
    static uint8_t model[8192];
    // resync with what the directed tests left behind
    for (uint32_t w = 0; w < 8192 / 4; w++) {
        uint32_t v = axil_read(tb, BASE + w * 4);
        model[w*4+0] = v & 0xFF;        model[w*4+1] = (v >> 8)  & 0xFF;
        model[w*4+2] = (v >> 16) & 0xFF; model[w*4+3] = (v >> 24) & 0xFF;
    }

    for (int i = 0; i < 3000; i++) {
        uint32_t off  = tb.rnd(0, 8191) & ~3u;   // word-aligned
        uint32_t v    = tb.rnd();
        uint8_t  strb = (uint8_t)tb.rnd(1, 15);
        axil_write(tb, BASE + off, v, strb);
        for (int k = 0; k < 4; k++)
            if (strb & (1 << k)) model[off + k] = (v >> (8 * k)) & 0xFF;

        uint32_t loff = tb.rnd(0, 8191) & ~3u;
        uint32_t want = 0;
        for (int k = 0; k < 4; k++)
            want |= (uint32_t)model[loff + k] << (8 * k);

        CHECK_EQ(axil_read(tb, BASE + loff), want);
    }

    return tb.finish();
}
