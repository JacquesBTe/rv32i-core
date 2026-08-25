#include "tb_common.h"
#include "Valu.h"

// Must match rtl/include/rv32i_defs.vh
enum {
    ALU_ADD  = 0b0000,
    ALU_SLL  = 0b0001,
    ALU_SLT  = 0b0010,
    ALU_SLTU = 0b0011,
    ALU_XOR  = 0b0100,
    ALU_SRL  = 0b0101,
    ALU_OR   = 0b0110,
    ALU_AND  = 0b0111,
    ALU_SUB  = 0b1000,
    ALU_SRA  = 0b1101,
};

static uint32_t ref(uint32_t op, uint32_t a, uint32_t b) {
    uint32_t sh = b & 31;              // RV32I: only low 5 bits matter
    switch (op) {
        case ALU_ADD:  return a + b;
        case ALU_SUB:  return a - b;
        case ALU_AND:  return a & b;
        case ALU_OR:   return a | b;
        case ALU_XOR:  return a ^ b;
        case ALU_SLL:  return a << sh;
        case ALU_SRL:  return a >> sh;                        // uint -> logical
        case ALU_SRA:  return (uint32_t)((int32_t)a >> sh);   // int  -> arithmetic
        case ALU_SLT:  return (int32_t)a < (int32_t)b;
        case ALU_SLTU: return a < b;
    }
    return 0;
}

int main(int argc, char** argv) {
    Tb<Valu> tb(argc, argv, "alu");
    auto& d = tb.dut;

    // Apply inputs, settle, compare against an explicit expectation.
    auto chk = [&](uint32_t op, uint32_t a, uint32_t b, uint32_t want) {
        d.a = a; d.b = b; d.alu_op = op;
        tb.settle();
        CHECK_EQ(d.result, want);
    };

    tb_begin("1. add / sub");
    chk(ALU_ADD, 5, 10, 15);
    chk(ALU_ADD, 0xFFFFFFFF, 1, 0);              // wraps to zero
    chk(ALU_SUB, 10, 5, 5);
    // TODO: 0 - 1 should give 0xFFFFFFFF
    chk(ALU_SUB, 0, 1, 0xFFFFFFFF);

    tb_begin("2. bitwise");
    chk(ALU_AND, 0xF0F0F0F0, 0xFF00FF00, 0xF000F000);
    // TODO: OR, XOR
    chk(ALU_OR, 0, 1, 1);
    chk(ALU_OR, 1, 1, 1);
    chk(ALU_OR, 1, 0, 1);
    chk(ALU_OR, 0, 0, 0);
    chk(ALU_XOR, 0, 1, 1);
    chk(ALU_XOR, 0, 0, 0);
    chk(ALU_XOR, 1, 0, 1);
    chk(ALU_XOR, 1, 1, 0);



    tb_begin("3. shifts");
    chk(ALU_SLL, 0x00000001, 4,  0x00000010);
    chk(ALU_SRL, 0x80000000, 4,  0x08000000);    // zero fill
    chk(ALU_SRA, 0x80000000, 4,  0xF8000000);    // sign fill
    // TODO: SRA with 0xFFFFFFFF -- should stay 0xFFFFFFFF for any shamt
    chk(ALU_SRA, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);

    tb_begin("4. shift amount masking");
    chk(ALU_SLL, 0x00000001, 32, 0x00000001);    // 32 masks to 0
    // TODO: shamt 33 behaves as 1; shamt 31 on SLL and SRL
    chk(ALU_SLL, 0x00000001, 33, 0x00000002);    // 33 masks to 1
    chk(ALU_SLL, 0x00000001, 31, 0x80000000);    // max shift left
    chk(ALU_SRL, 0x80000000, 31, 0x00000001);    // max shift right

    chk(ALU_SRA, 0x80000000, 63, 0xFFFFFFFF);


    tb_begin("5. slt vs sltu");
    chk(ALU_SLT,  0xFFFFFFFF, 1, 1);             // -1 < 1 signed
    chk(ALU_SLTU, 0xFFFFFFFF, 1, 0);             // huge > 1 unsigned
    // TODO: equal operands give 0 for both; 0x80000000 vs 0x7FFFFFFF both ways
    chk(ALU_SLT,  5, 5, 0);                      // equal -> 0
    chk(ALU_SLTU, 5, 5, 0);
    chk(ALU_SLT,  0x80000000, 0x7FFFFFFF, 1);    // most negative < most positive
    chk(ALU_SLTU, 0x80000000, 0x7FFFFFFF, 0);    // 2^31 > 2^31-1 unsigned


    tb_begin("6. random against reference model");
    static const uint32_t ops[] = {
        ALU_ADD, ALU_SUB, ALU_AND, ALU_OR,  ALU_XOR,
        ALU_SLL, ALU_SRL, ALU_SRA, ALU_SLT, ALU_SLTU
    };
    for (int i = 0; i < 20000; i++) {
        uint32_t op = ops[tb.rnd(0, 9)];
        uint32_t a  = tb.rnd();
        uint32_t b  = tb.rnd();
        // bias toward edge values a third of the time
        if (tb.rnd(0, 2) == 0) {
            static const uint32_t edge[] = {0, 1, 0x7FFFFFFF, 0x80000000,
                                            0xFFFFFFFF, 0x0000FFFF, 32, 31};
            a = edge[tb.rnd(0, 7)];
            b = edge[tb.rnd(0, 7)];
        }
        chk(op, a, b, ref(op, a, b));
    }

    return tb.finish();
}
