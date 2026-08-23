#include "tb_common.h"
#include "Vbranch_cmp.h"

// funct3 encodings for the branch instructions
enum { F3_BEQ  = 0b000, F3_BNE  = 0b001,
       F3_BLT  = 0b100, F3_BGE  = 0b101,
       F3_BLTU = 0b110, F3_BGEU = 0b111 };

// Reference model. The signed/unsigned split is the whole point of this
// module, so the casts here mirror the $signed() casts in the RTL.
static bool ref(uint32_t f3, uint32_t a, uint32_t b) {
    switch (f3) {
        case F3_BEQ:  return a == b;
        case F3_BNE:  return a != b;
        case F3_BLT:  return (int32_t)a <  (int32_t)b;
        case F3_BGE:  return (int32_t)a >= (int32_t)b;
        case F3_BLTU: return a <  b;
        case F3_BGEU: return a >= b;
        default:      return false;      // 010 and 011 are not branches
    }
}

int main(int argc, char** argv) {
    Tb<Vbranch_cmp> tb(argc, argv, "branch_cmp");
    auto& d = tb.dut;

    auto chk = [&](uint32_t f3, uint32_t a, uint32_t b, bool want) {
        d.funct3 = f3; d.a = a; d.b = b;
        tb.settle();
        CHECK_EQ(d.taken, want ? 1 : 0);
    };

    tb_begin("1. beq / bne");
    chk(F3_BEQ, 5, 5, true);
    chk(F3_BEQ, 5, 6, false);
    chk(F3_BEQ, 0xFFFFFFFF, 0xFFFFFFFF, true);
    chk(F3_BNE, 5, 5, false);
    chk(F3_BNE, 5, 6, true);
    chk(F3_BNE, 0, 0xFFFFFFFF, true);

    tb_begin("2. blt -- signed");
    chk(F3_BLT, 5, 10, true);
    chk(F3_BLT, 10, 5, false);
    chk(F3_BLT, 5, 5, false);                  // strict, not <=
    chk(F3_BLT, 0xFFFFFFFF, 1, true);          // -1 < 1
    chk(F3_BLT, 1, 0xFFFFFFFF, false);         // 1 > -1
    chk(F3_BLT, 0xFFFFFFFE, 0xFFFFFFFF, true); // -2 < -1

    tb_begin("3. bge -- signed, and it is >= not >");
    chk(F3_BGE, 10, 5, true);
    chk(F3_BGE, 5, 10, false);
    chk(F3_BGE, 5, 5, true);                   // equal counts
    chk(F3_BGE, 1, 0xFFFFFFFF, true);          // 1 >= -1
    chk(F3_BGE, 0xFFFFFFFF, 1, false);         // -1 < 1

    tb_begin("4. bltu -- unsigned");
    chk(F3_BLTU, 5, 10, true);
    chk(F3_BLTU, 10, 5, false);
    chk(F3_BLTU, 5, 5, false);
    chk(F3_BLTU, 0xFFFFFFFF, 1, false);        // huge > 1
    chk(F3_BLTU, 1, 0xFFFFFFFF, true);
    chk(F3_BLTU, 0, 1, true);

    tb_begin("5. bgeu -- unsigned");
    chk(F3_BGEU, 10, 5, true);
    chk(F3_BGEU, 5, 10, false);
    chk(F3_BGEU, 5, 5, true);
    chk(F3_BGEU, 0xFFFFFFFF, 1, true);
    chk(F3_BGEU, 0, 0, true);

    tb_begin("6. the sign boundary -- signed and unsigned disagree");
    // 0x80000000 and 0x7FFFFFFF are adjacent patterns across the sign bit,
    // so every signed/unsigned pair gives the opposite answer here. This
    // is what a missing $signed() cast breaks.
    chk(F3_BLT,  0x80000000, 0x7FFFFFFF, true);   // most negative < most positive
    chk(F3_BLTU, 0x80000000, 0x7FFFFFFF, false);  // 2^31 > 2^31-1
    chk(F3_BGE,  0x80000000, 0x7FFFFFFF, false);
    chk(F3_BGEU, 0x80000000, 0x7FFFFFFF, true);
    chk(F3_BLT,  0x7FFFFFFF, 0x80000000, false);
    chk(F3_BLTU, 0x7FFFFFFF, 0x80000000, true);

    tb_begin("7. zero as an operand -- beqz/bnez are built on this");
    chk(F3_BEQ,  0, 0, true);
    chk(F3_BNE,  0, 0, false);
    chk(F3_BLT,  0xFFFFFFFF, 0, true);         // -1 < 0
    chk(F3_BLTU, 0xFFFFFFFF, 0, false);        // huge > 0
    chk(F3_BGE,  0, 0xFFFFFFFF, true);         // 0 >= -1

    tb_begin("8. unused funct3 encodings never take");
    // 010 and 011 are not branch instructions; the default arm must be 0.
    chk(0b010, 5, 5, false);
    chk(0b011, 5, 5, false);
    chk(0b010, 0, 0, false);

    tb_begin("9. random against the reference model");
    static const uint32_t edge[] = {0, 1, 2, 0x7FFFFFFE, 0x7FFFFFFF,
                                    0x80000000, 0x80000001, 0xFFFFFFFE,
                                    0xFFFFFFFF};
    static const uint32_t f3s[] = {F3_BEQ, F3_BNE, F3_BLT,
                                   F3_BGE, F3_BLTU, F3_BGEU};
    for (int i = 0; i < 20000; i++) {
        uint32_t f3 = f3s[tb.rnd(0, 5)];
        uint32_t a  = tb.rnd();
        uint32_t b  = tb.rnd();
        // Bias toward the sign boundary and toward equality, which uniform
        // random almost never produces.
        switch (tb.rnd(0, 3)) {
            case 0: a = edge[tb.rnd(0, 8)]; b = edge[tb.rnd(0, 8)]; break;
            case 1: b = a; break;                        // equality
            case 2: a = edge[tb.rnd(0, 8)]; break;
            default: break;
        }
        chk(f3, a, b, ref(f3, a, b));
    }

    return tb.finish();
}
