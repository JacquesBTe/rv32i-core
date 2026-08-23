#include "tb_common.h"
#include "Vdmem.h"

int main(int argc, char** argv) {
    Tb<Vdmem> tb(argc, argv, "dmem");
    auto& d = tb.dut;

    d.clk = 0; d.en = 1; d.addr = 0; d.wdata = 0; d.wstrb = 0;

    const uint32_t BASE = 0x80000000;

    auto store = [&](uint32_t a, uint32_t v, uint8_t strb) {
        d.addr = a; d.wdata = v; d.wstrb = strb;
        tb.tick();
        d.wstrb = 0;
    };
    auto load = [&](uint32_t a) -> uint32_t {
        d.addr = a; d.wstrb = 0;
        tb.tick();
        return d.rdata;
    };

    tb_begin("1. word round-trip");
    store(BASE + 0, 0xDEADBEEF, 0xF);
    CHECK_EQ(load(BASE + 0), 0xDEADBEEF);
    store(BASE + 4, 0x12345678, 0xF);
    CHECK_EQ(load(BASE + 4), 0x12345678);
    CHECK_EQ(load(BASE + 0), 0xDEADBEEF);   // neighbour untouched

    tb_begin("2. wstrb selects individual byte lanes");
    store(BASE + 8, 0x00000000, 0xF);
    store(BASE + 8, 0x000000AA, 0x1);
    CHECK_EQ(load(BASE + 8), 0x000000AA);
    store(BASE + 8, 0x0000BB00, 0x2);
    CHECK_EQ(load(BASE + 8), 0x0000BBAA);
    store(BASE + 8, 0x00CC0000, 0x4);
    CHECK_EQ(load(BASE + 8), 0x00CCBBAA);
    store(BASE + 8, 0xDD000000, 0x8);
    CHECK_EQ(load(BASE + 8), 0xDDCCBBAA);

    tb_begin("3. wstrb combinations do not disturb other lanes");
    store(BASE + 12, 0xFFFFFFFF, 0xF);
    store(BASE + 12, 0x00000000, 0x2);      // clear lane 1 only
    CHECK_EQ(load(BASE + 12), 0xFFFF00FF);
    store(BASE + 12, 0x12340000, 0xC);      // set lanes 2,3
    CHECK_EQ(load(BASE + 12), 0x123400FF);

    tb_begin("4. wstrb == 0 means no write");
    store(BASE + 16, 0xCAFEBABE, 0xF);
    d.addr = BASE + 16; d.wdata = 0x00000000; d.wstrb = 0;
    tb.tick();
    CHECK_EQ(load(BASE + 16), 0xCAFEBABE);

    tb_begin("5. en == 0 holds rdata (no new sync read)");
    store(BASE + 20, 0x11223344, 0xF);
    load(BASE + 20);                        // rdata now 0x11223344
    d.en = 0;
    d.addr = BASE + 24;                     // different address
    tb.tick();
    CHECK_EQ(d.rdata, 0x11223344);          // unchanged -- read was gated
    d.en = 1;

    tb_begin("6. random against byte-array model");
    static uint8_t model[65536];
    for (int i = 0; i < 65536; i++) model[i] = 0;
    for (uint32_t w = 0; w < 16384; w++) {
        uint32_t v = load(BASE + w * 4);
        model[w*4+0] = v & 0xFF;        model[w*4+1] = (v >> 8)  & 0xFF;
        model[w*4+2] = (v >> 16) & 0xFF; model[w*4+3] = (v >> 24) & 0xFF;
    }

    for (int i = 0; i < 20000; i++) {
        uint32_t off  = tb.rnd(0, 0xFFFF) & ~3u;   // word-aligned
        uint32_t v    = tb.rnd();
        uint8_t  strb = (uint8_t)tb.rnd(0, 15);
        store(BASE + off, v, strb);
        for (int k = 0; k < 4; k++)
            if (strb & (1 << k)) model[off + k] = (v >> (8 * k)) & 0xFF;

        uint32_t loff = tb.rnd(0, 0xFFFF) & ~3u;
        uint32_t want = 0;
        for (int k = 0; k < 4; k++)
            want |= (uint32_t)model[loff + k] << (8 * k);

        CHECK_EQ(load(BASE + loff), want);
    }

    return tb.finish();
}
