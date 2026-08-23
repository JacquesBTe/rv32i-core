#include "tb_common.h"
#include "Vgpio.h"

// Minimal AXI4-Lite master BFM: drive one write or one read transaction to
// completion. gpio responds in one cycle (wait tied low, ack == en), so
// these loops terminate quickly; the iteration cap just guards against a
// genuine protocol bug hanging the test.
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
    Tb<Vgpio> tb(argc, argv, "gpio");
    auto& d = tb.dut;

    d.clk = 0; d.rst_n = 0; d.sw = 0;
    d.s_axil_awvalid = 0; d.s_axil_wvalid = 0; d.s_axil_bready = 0;
    d.s_axil_arvalid = 0; d.s_axil_rready = 0;
    tb.tick(); tb.tick();
    d.rst_n = 1;

    tb_begin("1. LED write lands and reads back");
    axil_write(tb, 0x00, 0x0000BEEF, 0xF);
    CHECK_EQ(axil_read(tb, 0x00), 0x0000BEEF);
    CHECK_EQ(d.led, 0xBEEF);

    tb_begin("2. wstrb selects individual byte lanes on LED reg");
    axil_write(tb, 0x00, 0x00000000, 0xF);
    axil_write(tb, 0x00, 0x000000AA, 0x1);
    CHECK_EQ(axil_read(tb, 0x00), 0x000000AA);
    axil_write(tb, 0x00, 0x0000BB00, 0x2);
    CHECK_EQ(axil_read(tb, 0x00), 0x0000BBAA);
    CHECK_EQ(d.led, 0xBBAA);

    tb_begin("3. switch input reads current value, is read-only");
    d.sw = 0xCAFE;
    CHECK_EQ(axil_read(tb, 0x04), 0x0000CAFE);
    axil_write(tb, 0x04, 0xFFFFFFFF, 0xF);   // should have no effect
    CHECK_EQ(axil_read(tb, 0x04), 0x0000CAFE);
    d.sw = 0x1234;
    CHECK_EQ(axil_read(tb, 0x04), 0x00001234);

    tb_begin("4. LED value unaffected by switch writes/reads");
    axil_write(tb, 0x00, 0x00005555, 0xF);
    d.sw = 0xAAAA;
    CHECK_EQ(axil_read(tb, 0x04), 0x0000AAAA);
    CHECK_EQ(axil_read(tb, 0x00), 0x00005555);
    CHECK_EQ(d.led, 0x5555);

    tb_begin("5. back-to-back writes and reads, no idle transaction between");
    for (uint32_t i = 0; i < 50; i++) {
        uint32_t v = (i * 0x01010101u) & 0xFFFF;
        axil_write(tb, 0x00, v, 0xF);
        CHECK_EQ(axil_read(tb, 0x00), v);
    }

    tb_begin("6. random writes against a shadow model");
    uint32_t model = 0;
    for (int i = 0; i < 500; i++) {
        uint32_t v    = tb.rnd();
        uint8_t  strb = (uint8_t)tb.rnd(1, 15);
        axil_write(tb, 0x00, v, strb);
        for (int k = 0; k < 4; k++)
            if (strb & (1 << k))
                model = (model & ~(0xFFu << (8 * k))) | (v & (0xFFu << (8 * k)));
        CHECK_EQ(axil_read(tb, 0x00), model);
        CHECK_EQ(d.led, model & 0xFFFF);

        uint32_t sw = tb.rnd();
        d.sw = sw & 0xFFFF;
        CHECK_EQ(axil_read(tb, 0x04), sw & 0xFFFF);
    }

    return tb.finish();
}
