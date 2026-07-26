#include "tb_common.h"
#include "Vregfile.h"

int main(int argc, char** argv) {
    Tb<Vregfile> tb(argc, argv, "regfile");
    auto& d = tb.dut;

    // idle state
    d.clk = 0; d.rd_we = 0; d.rd_addr = 0; d.rd_data = 0;
    d.rs1_addr = 0; d.rs2_addr = 0;

    // helper: write a value, then drop the enable
    auto wr = [&](int addr, uint32_t data) {
        d.rd_addr = addr; d.rd_data = data; d.rd_we = 1;
        tb.tick();
        d.rd_we = 0;
    };

    auto rd1 = [&](int addr) -> uint32_t {
        d.rs1_addr = addr; tb.settle(); return d.rs1_data;
    };
    auto rd2 = [&](int addr) -> uint32_t {
        d.rs2_addr = addr; tb.settle(); return d.rs2_data;
    };


    tb_begin("1. write then read back on port A");
    wr(7, 0xDEADBEEF);
    d.rs1_addr = 7; tb.settle();
    CHECK_EQ(d.rs1_data, 0xDEADBEEF);

    tb_begin("2. same register visible on both ports");
    d.rs2_addr = 7; tb.settle();
    CHECK_EQ(d.rs2_data, 0xDEADBEEF);

    // 3. rd_we low -> write does not take effect
    tb_begin("3. rd_we low -> write does not take effect");
    wr(8, 0x12345678);              // seed a known value using the helper

    d.rd_addr = 8;                  // longhand: set up a write...
    d.rd_data = 0x87654321;
    d.rd_we   = 0;                  // ...but leave the enable OFF
    tb.tick();                      // edge passes -- nothing should be stored

    d.rs1_addr = 8; tb.settle();
    CHECK_EQ(d.rs1_data, 0x12345678);   // still the original

    // 4. write to x0 with we high -> x0 still reads 0
    tb_begin("4. write to x0 with we high -> x0 still reads 0");

    d.rd_addr = 0;
    d.rd_data = 0x22446688;
    d.rd_we = 1; //enable writing

    tb.tick();                      // edge passes -- nothing should be stored

    d.rd_we = 0;
    d.rs1_addr = 0; 
    tb.settle();
    CHECK_EQ(d.rs1_data, 0x00000000);   // still the original

    // 5. x0 reads 0 on both ports

    tb_begin("5. x0 reads 0 on both ports");
    d.rs1_addr = 0;
    d.rs2_addr = 0;
    tb.settle();
    CHECK_EQ(d.rs1_data, 0x00000000);
    CHECK_EQ(d.rs2_data, 0x00000000);

    // 6. bypass: write x9 while reading x9 in the same cycle
    
    tb_begin("6. bypass: write x9 while reading x9 in the same cycle");
    wr(9,0x33333333);

    d.rd_addr = 9;
    d.rd_data = 0x11223344;
    d.rd_we = 1;
    d.rs1_addr = 9;
    tb.settle();
    CHECK_EQ(d.rs1_data,0x11223344);

    tb.tick();
    d.rd_we = 0;
    tb.settle();
    CHECK_EQ(d.rs1_data, 0x11223344);


    // 7. bypass respects rd_we: same setup, we=0 -> old value

    tb_begin("7. bypass respects rd_we: same setup, we=0 -> old value");

    wr(9, 0x33333333);                      // re-seed
    d.rd_addr = 9; 
    d.rd_data = 0x11223344; 
    d.rd_we = 0;


    d.rs1_addr = 9; 
    tb.settle();
    CHECK_EQ(d.rs1_data, 0x33333333);      // no write -> no bypass
    d.rd_we = 0;

    // 8. bypass does not resurrect x0
    tb_begin("8. bypass does no resurrect x0");
    
    d.rd_addr = 0; d.rd_data = 0xDEADBEEF; d.rd_we = 1;
    d.rs1_addr = 0; d.rs2_addr = 0; tb.settle();
    CHECK_EQ(d.rs1_data, 0x00000000);
    CHECK_EQ(d.rs2_data, 0x00000000);
    tb.tick(); d.rd_we = 0;

     tb_begin("9. neighbours undisturbed");
    wr(10, 0xAAAAAAAA);
    wr(11, 0xBBBBBBBB);
    wr(12, 0xCCCCCCCC);
    CHECK_EQ(rd1(10), 0xAAAAAAAA);
    CHECK_EQ(rd1(11), 0xBBBBBBBB);
    CHECK_EQ(rd1(12), 0xCCCCCCCC);

    tb_begin("10. random against shadow model");
    uint32_t model[32] = {0};
    for (int i = 0; i < 32; i++) model[i] = 0;
    // resync the model with whatever the directed tests left behind
    for (int i = 0; i < 32; i++) { model[i] = rd1(i); }

    for (int i = 0; i < 5000; i++) {
        int      a = tb.rnd(0, 31);
        uint32_t v = tb.rnd();
        wr(a, v);
        if (a != 0) model[a] = v;

        int b = tb.rnd(0, 31);
        int c = tb.rnd(0, 31);
        CHECK_EQ(rd1(b), b == 0 ? 0 : model[b]);
        CHECK_EQ(rd2(c), c == 0 ? 0 : model[c]);
    }

    
    return tb.finish();
}