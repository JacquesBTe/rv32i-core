#include "tb_common.h"
#include "Vimmgen.h"

// ---- opcodes -----------------------------------------------------------
enum {
    OP_LUI    = 0b0110111,
    OP_AUIPC  = 0b0010111,
    OP_JAL    = 0b1101111,
    OP_JALR   = 0b1100111,
    OP_BRANCH = 0b1100011,
    OP_LOAD   = 0b0000011,
    OP_STORE  = 0b0100011,
    OP_OPIMM  = 0b0010011,
    OP_OP     = 0b0110011,
};

// ---- instruction encoders ----------------------------------------------
// Each takes the immediate as a plain signed value and scatters it into the
// instruction word per the RV32I spec.

static uint32_t enc_i(uint32_t op, uint32_t rd, uint32_t f3,
                      uint32_t rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12)
         | (rd << 7) | op;
}

static uint32_t enc_s(uint32_t op, uint32_t f3, uint32_t rs1,
                      uint32_t rs2, int32_t imm) {
    uint32_t u = imm & 0xFFF;
    return (((u >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15)
         | (f3 << 12) | ((u & 0x1F) << 7) | op;
}

static uint32_t enc_b(uint32_t op, uint32_t f3, uint32_t rs1,
                      uint32_t rs2, int32_t imm) {
    uint32_t u = imm & 0x1FFF;          // 13 bits, bit 0 must be 0
    return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25)
         | (rs2 << 20) | (rs1 << 15) | (f3 << 12)
         | (((u >> 1) & 0xF) << 8) | (((u >> 11) & 1) << 7) | op;
}

static uint32_t enc_u(uint32_t op, uint32_t rd, int32_t imm) {
    return (imm & 0xFFFFF000) | (rd << 7) | op;   // imm already in place
}

static uint32_t enc_j(uint32_t op, uint32_t rd, int32_t imm) {
    uint32_t u = imm & 0x1FFFFF;        // 21 bits, bit 0 must be 0
    return (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3FF) << 21)
         | (((u >> 11) & 1) << 20) | (((u >> 12) & 0xFF) << 12)
         | (rd << 7) | op;
}

// sign-extend the low n bits of v to 32
static uint32_t sext(uint32_t v, int n) {
    uint32_t m = 1u << (n - 1);
    return (v ^ m) - m;
}

int main(int argc, char** argv) {
    Tb<Vimmgen> tb(argc, argv, "immgen");
    auto& d = tb.dut;

    auto chk = [&](uint32_t instr, uint32_t want) {
        d.instr = instr;
        tb.settle();
        CHECK_EQ(d.imm, want);
    };

    // ---- anchors: hand-verified against real encodings -----------------
    tb_begin("1. known instruction words");
    chk(0x02A08293, 42);            // addi x5, x1, 42
    chk(0xFFF08293, 0xFFFFFFFF);    // addi x5, x1, -1
    // TODO: add two more from a .dis file you generated in phase 0 --
    //       ideally one store and one branch, since those are the
    //       formats your own encoder is most likely to get wrong.
    chk(0xf5b50793, 0xFFFFFF5B); //addi	a5,a0,-165
    chk(0x01010113, 16); //addi	sp,sp,16


    tb_begin("2. I-type directed");
    chk(enc_i(OP_OPIMM, 5, 0, 1,     0), 0);
    chk(enc_i(OP_OPIMM, 5, 0, 1,  2047), 2047);          // max positive
    chk(enc_i(OP_OPIMM, 5, 0, 1, -2048), 0xFFFFF800);    // max negative
    chk(enc_i(OP_LOAD,  5, 2, 1,     8), 8);             // lw offset

    tb_begin("3. S-type directed");
    chk(enc_s(OP_STORE, 2, 1, 2,     8), 8);
    chk(enc_s(OP_STORE, 2, 1, 2,    -8), 0xFFFFFFF8);
    // TODO: 2047 and -2048


    tb_begin("4. B-type directed");
    chk(enc_b(OP_BRANCH, 0, 1, 2,     8), 8);
    chk(enc_b(OP_BRANCH, 0, 1, 2,    -8), 0xFFFFFFF8);
    // TODO: 4094 (max positive) and -4096 (max negative)
    chk(enc_b(OP_BRANCH, 0, 1, 2, 4094), 4094);
    chk(enc_b(OP_BRANCH, 0, 1, 2, -4094), 0xFFFFF000);

    tb_begin("5. U-type directed");
    chk(enc_u(OP_LUI,   5, 0x12345000), 0x12345000);
    chk(enc_u(OP_AUIPC, 5, 0xFFFFF000), 0xFFFFF000);   // no sign extension
    // TODO: 0 and 0x00001000
    chk(enc_u(OP_LUI,   5, 0x00000000), 0x00000000);
    chk(enc_u(OP_LUI,   5, 0x00001000), 0x00001000);

    tb_begin("6. J-type directed");
    chk(enc_j(OP_JAL, 1,     8), 8);
    chk(enc_j(OP_JAL, 1,    -8), 0xFFFFFFF8);
    // TODO: 1048574 (max positive) and -1048576 (max negative)
    chk(enc_j(OP_JAL, 1,  1048574), 0x000FFFFE);   // max positive
    chk(enc_j(OP_JAL, 1, -1048576), 0xFFF00000);   // max negative

    tb_begin("7. R-type has no immediate");
    chk((0b0110011) | (5 << 7) | (1 << 15) | (2 << 20), 0);

    tb_begin("8. random round-trip");
    for (int i = 0; i < 20000; i++) {
        uint32_t rd  = tb.rnd(0, 31), rs1 = tb.rnd(0, 31), rs2 = tb.rnd(0, 31);
        uint32_t f3  = tb.rnd(0, 7);
        switch (tb.rnd(0, 4)) {
        case 0: {                                   // I
            int32_t v = (int32_t)sext(tb.rnd(), 12);
            chk(enc_i(OP_OPIMM, rd, f3, rs1, v), (uint32_t)v);
            break; }
        case 1: {                                   // S
            int32_t v = (int32_t)sext(tb.rnd(), 12);
            chk(enc_s(OP_STORE, f3, rs1, rs2, v), (uint32_t)v);
            break; }
        case 2: {                                   // B -- bit 0 always 0
            int32_t v = (int32_t)sext(tb.rnd() & ~1u, 13);
            chk(enc_b(OP_BRANCH, f3, rs1, rs2, v), (uint32_t)v);
            break; }
        case 3: {                                   // U -- no sign extension
            uint32_t v = tb.rnd() & 0xFFFFF000;
            chk(enc_u(OP_LUI, rd, v), v);
            break; }
        case 4: {                                   // J -- bit 0 always 0
            int32_t v = (int32_t)sext(tb.rnd() & ~1u, 21);
            chk(enc_j(OP_JAL, rd, v), (uint32_t)v);
            break; }
        }
    }

    return tb.finish();
}
