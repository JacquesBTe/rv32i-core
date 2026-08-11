#include "tb_common.h"
#include "Vimem.h"
#include <fstream>
#include <iomanip>

int main(int argc, char** argv) {
    // Generate the hex file BEFORE constructing the DUT -- $readmemh runs
    // during elaboration, so the file must exist by then.
    const char* HEX = "imem_test.hex";
    uint32_t golden[16384] = {0};
    {
        std::mt19937 gen(12345);
        std::ofstream f(HEX);
        for (int i = 0; i < 16384; i++) {
            golden[i] = gen();
            f << std::hex << std::setw(8) << std::setfill('0') << golden[i] << "\n";
        }
    }

    Tb<Vimem> tb(argc, argv, "imem");
    auto& d = tb.dut;

    auto fetch = [&](uint32_t byte_addr) -> uint32_t {
        d.addr = byte_addr;
        tb.settle();
        return d.instr;
    };

    tb_begin("1. first few words");
    CHECK_EQ(fetch(0x80000000), golden[0]);
    CHECK_EQ(fetch(0x80000004), golden[1]);
    CHECK_EQ(fetch(0x80000008), golden[2]);
    CHECK_EQ(fetch(0x8000000C), golden[3]);

    tb_begin("2. byte address maps to word index");
    CHECK_EQ(fetch(0x80000040), golden[16]);      // 0x40 / 4 = 16
    CHECK_EQ(fetch(0x80001000), golden[1024]);

    tb_begin("3. low two bits are ignored");
    CHECK_EQ(fetch(0x80000004), golden[1]);
    CHECK_EQ(fetch(0x80000005), golden[1]);       // all four byte addresses
    CHECK_EQ(fetch(0x80000006), golden[1]);       // in the same word return
    CHECK_EQ(fetch(0x80000007), golden[1]);       // that word

    tb_begin("4. base address is ignored");
    CHECK_EQ(fetch(0x00000000), golden[0]);       // no base subtraction
    CHECK_EQ(fetch(0x80000000), golden[0]);       // both hit index 0

    tb_begin("5. last word in range");
    CHECK_EQ(fetch(0x8000FFFC), golden[16383]);

    tb_begin("6. sweep every word");
    for (int i = 0; i < 16384; i++)
        CHECK_EQ(fetch(0x80000000 + i * 4), golden[i]);

    tb_begin("7. random access order");
    for (int i = 0; i < 5000; i++) {
        uint32_t idx = tb.rnd(0, 16383);
        CHECK_EQ(fetch(0x80000000 + idx * 4), golden[idx]);
    }

    return tb.finish();
}
