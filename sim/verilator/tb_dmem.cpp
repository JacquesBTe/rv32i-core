#include "tb_common.h"
#include "Vdmem.h"

enum { F3_B = 0b000, F3_H = 0b001, F3_W = 0b010,
       F3_BU = 0b100, F3_HU = 0b101 };

int main(int argc, char** argv) {
    Tb<Vdmem> tb(argc, argv, "dmem");
    auto& d = tb.dut;

    d.clk = 0; d.we = 0; d.addr = 0; d.wdata = 0; d.funct3 = F3_W;

    const uint32_t BASE = 0x80000000;

    auto store = [&](uint32_t a, uint32_t v, uint32_t f3) {
        d.addr = a; d.wdata = v; d.funct3 = f3; d.we = 1;
        tb.tick();
        d.we = 0;
    };
    auto load = [&](uint32_t a, uint32_t f3) -> uint32_t {
        d.addr = a; d.funct3 = f3; tb.settle();
        return d.rdata;
    };

    tb_begin("1. word round-trip");
    store(BASE + 0, 0xDEADBEEF, F3_W);
    CHECK_EQ(load(BASE + 0, F3_W), 0xDEADBEEF);
    store(BASE + 4, 0x12345678, F3_W);
    CHECK_EQ(load(BASE + 4, F3_W), 0x12345678);
    CHECK_EQ(load(BASE + 0, F3_W), 0xDEADBEEF);   // neighbour untouched

    tb_begin("2. byte store hits only its own lane");
    store(BASE + 8, 0x00000000, F3_W);
    store(BASE + 8, 0xAA, F3_B);
    CHECK_EQ(load(BASE + 8, F3_W), 0x000000AA);
    store(BASE + 9, 0xBB, F3_B);
    CHECK_EQ(load(BASE + 8, F3_W), 0x0000BBAA);
    store(BASE + 10, 0xCC, F3_B);
    CHECK_EQ(load(BASE + 8, F3_W), 0x00CCBBAA);
    store(BASE + 11, 0xDD, F3_B);
    CHECK_EQ(load(BASE + 8, F3_W), 0xDDCCBBAA);

    tb_begin("3. byte store does not disturb neighbours");
    store(BASE + 12, 0xFFFFFFFF, F3_W);
    store(BASE + 13, 0x00, F3_B);                 // clear lane 1 only
    CHECK_EQ(load(BASE + 12, F3_W), 0xFFFF00FF);

    tb_begin("4. halfword lanes");
    store(BASE + 16, 0x00000000, F3_W);
    store(BASE + 16, 0x1234, F3_H);
    CHECK_EQ(load(BASE + 16, F3_W), 0x00001234);
    store(BASE + 18, 0x5678, F3_H);
    CHECK_EQ(load(BASE + 16, F3_W), 0x56781234);

    tb_begin("5. byte loads, all four lanes");
    store(BASE + 20, 0x44332211, F3_W);
    CHECK_EQ(load(BASE + 20, F3_BU), 0x11);
    CHECK_EQ(load(BASE + 21, F3_BU), 0x22);
    CHECK_EQ(load(BASE + 22, F3_BU), 0x33);
    CHECK_EQ(load(BASE + 23, F3_BU), 0x44);

    tb_begin("6. lb vs lbu sign extension");
    store(BASE + 24, 0x000000FF, F3_W);
    CHECK_EQ(load(BASE + 24, F3_B),  0xFFFFFFFF);   // -1 signed
    CHECK_EQ(load(BASE + 24, F3_BU), 0x000000FF);   // 255 unsigned
    store(BASE + 24, 0x0000007F, F3_W);
    CHECK_EQ(load(BASE + 24, F3_B),  0x0000007F);   // positive: same either way
    CHECK_EQ(load(BASE + 24, F3_BU), 0x0000007F);
    store(BASE + 24, 0x80000000, F3_W);
    CHECK_EQ(load(BASE + 27, F3_B),  0xFFFFFF80);   // top lane, sign set
    CHECK_EQ(load(BASE + 27, F3_BU), 0x00000080);

    tb_begin("7. lh vs lhu sign extension");
    store(BASE + 28, 0x0000FFFF, F3_W);
    CHECK_EQ(load(BASE + 28, F3_H),  0xFFFFFFFF);
    CHECK_EQ(load(BASE + 28, F3_HU), 0x0000FFFF);
    store(BASE + 28, 0x8000FFFF, F3_W);
    CHECK_EQ(load(BASE + 30, F3_H),  0xFFFF8000);   // upper half, sign set
    CHECK_EQ(load(BASE + 30, F3_HU), 0x00008000);

    tb_begin("8. we low means no write");
    store(BASE + 32, 0xCAFEBABE, F3_W);
    d.addr = BASE + 32; d.wdata = 0x00000000; d.funct3 = F3_W; d.we = 0;
    tb.tick();
    CHECK_EQ(load(BASE + 32, F3_W), 0xCAFEBABE);

    tb_begin("9. random against byte-array model");
    static uint8_t model[65536];
    for (int i = 0; i < 65536; i++) model[i] = 0;
    // resync with what the directed tests left behind
    for (uint32_t w = 0; w < 16384; w++) {
        uint32_t v = load(BASE + w * 4, F3_W);
        model[w*4+0] = v & 0xFF;        model[w*4+1] = (v >> 8)  & 0xFF;
        model[w*4+2] = (v >> 16) & 0xFF; model[w*4+3] = (v >> 24) & 0xFF;
    }

    for (int i = 0; i < 20000; i++) {
        // --- store a random size at a naturally aligned address ---
        int      sz = tb.rnd(0, 2);                  // 0=byte 1=half 2=word
        uint32_t off = tb.rnd(0, 0xFFFF) & ~((1u << sz) - 1);
        uint32_t v  = tb.rnd();
        uint32_t f3 = (sz == 0) ? F3_B : (sz == 1) ? F3_H : F3_W;
        store(BASE + off, v, f3);
        for (int k = 0; k < (1 << sz); k++)
            model[off + k] = (v >> (8 * k)) & 0xFF;

        // --- load a random size at a naturally aligned address ---
        int      lsz  = tb.rnd(0, 2);
        uint32_t loff = tb.rnd(0, 0xFFFF) & ~((1u << lsz) - 1);
        bool     uns  = tb.rnd(0, 1);

        uint32_t want = 0;
        for (int k = 0; k < (1 << lsz); k++)
            want |= (uint32_t)model[loff + k] << (8 * k);

        uint32_t lf3;
        if (lsz == 2)      { lf3 = F3_W; }
        else if (lsz == 0) { lf3 = uns ? F3_BU : F3_B;
                             if (!uns && (want & 0x80))   want |= 0xFFFFFF00; }
        else               { lf3 = uns ? F3_HU : F3_H;
                             if (!uns && (want & 0x8000)) want |= 0xFFFF0000; }

        CHECK_EQ(load(BASE + loff, lf3), want);
    }

    return tb.finish();
}
