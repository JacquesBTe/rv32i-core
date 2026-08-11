#include "tb_common.h"
#include "Vdecoder.h"

// must match rv32i_defs.vh
enum { ALU_ADD=0b0000, ALU_SLL=0b0001, ALU_SLT=0b0010, ALU_SLTU=0b0011,
       ALU_XOR=0b0100, ALU_SRL=0b0101, ALU_OR=0b0110,  ALU_AND=0b0111,
       ALU_SUB=0b1000, ALU_SRA=0b1101 };

enum { OP_LUI=0b0110111, OP_AUIPC=0b0010111, OP_JAL=0b1101111,
       OP_JALR=0b1100111, OP_BRANCH=0b1100011, OP_LOAD=0b0000011,
       OP_STORE=0b0100011, OP_OPIMM=0b0010011, OP_OP=0b0110011,
       OP_FENCE=0b0001111, OP_SYSTEM=0b1110011 };

static uint32_t enc_r(uint32_t f7, uint32_t rs2, uint32_t rs1,
                      uint32_t f3, uint32_t rd) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | OP_OP;
}
static uint32_t enc_i(uint32_t op, uint32_t rd, uint32_t f3,
                      uint32_t rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t enc_s(uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
    uint32_t u = imm & 0xFFF;
    return (((u >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15)
         | (f3 << 12) | ((u & 0x1F) << 7) | OP_STORE;
}

int main(int argc, char** argv) {
    Tb<Vdecoder> tb(argc, argv, "decoder");
    auto& d = tb.dut;

    auto decode = [&](uint32_t instr) { d.instr = instr; tb.settle(); };

    tb_begin("1. register fields extracted from fixed positions");
    decode(enc_r(0, 2, 1, 0, 5));          // add x5, x1, x2
    CHECK_EQ(d.rs1_addr, 1);
    CHECK_EQ(d.rs2_addr, 2);
    CHECK_EQ(d.rd_addr,  5);
    CHECK_EQ(d.funct3,   0);
    decode(enc_r(0, 31, 30, 7, 29));       // and x29, x30, x31
    CHECK_EQ(d.rs1_addr, 30);
    CHECK_EQ(d.rs2_addr, 31);
    CHECK_EQ(d.rd_addr,  29);
    CHECK_EQ(d.funct3,   7);

    tb_begin("2. R-type control signals");
    decode(enc_r(0, 2, 1, 0, 5));          // add
    CHECK_EQ(d.alu_op,  ALU_ADD);
    CHECK_EQ(d.alu_src, 0);                // b input is rs2
    CHECK_EQ(d.reg_we,  1);
    CHECK_EQ(d.mem_we,  0);
    CHECK_EQ(d.mem_re,  0);
    CHECK_EQ(d.wb_sel,  0);
    CHECK_EQ(d.illegal, 0);

    tb_begin("3. R-type: bit 30 selects sub and sra");
    decode(enc_r(0b0100000, 2, 1, 0b000, 5));   // sub
    CHECK_EQ(d.alu_op, ALU_SUB);
    decode(enc_r(0b0000000, 2, 1, 0b101, 5));   // srl
    CHECK_EQ(d.alu_op, ALU_SRL);
    decode(enc_r(0b0100000, 2, 1, 0b101, 5));   // sra
    CHECK_EQ(d.alu_op, ALU_SRA);
    decode(enc_r(0, 2, 1, 0b010, 5));           // slt
    CHECK_EQ(d.alu_op, ALU_SLT);

    tb_begin("4. OP-IMM: bit 30 must be masked");
    decode(enc_i(OP_OPIMM, 5, 0b000, 1, 42));
    CHECK_EQ(d.alu_op,  ALU_ADD);
    CHECK_EQ(d.alu_src, 1);
    CHECK_EQ(d.reg_we,  1);
    decode(enc_i(OP_OPIMM, 5, 0b000, 1, -1));   // imm 0xFFF sets bit 30
    CHECK_EQ(d.alu_op, ALU_ADD);                // must NOT become SUB
    decode(enc_i(OP_OPIMM, 5, 0b100, 1, -1));   // xori with negative imm
    CHECK_EQ(d.alu_op, ALU_XOR);
    decode(enc_i(OP_OPIMM, 5, 0b010, 1, -1));   // slti
    CHECK_EQ(d.alu_op, ALU_SLT);

    tb_begin("5. OP-IMM: srai is the exception");
    decode((0b0000000 << 25) | (4 << 20) | (1 << 15) | (0b101 << 12) | (5 << 7) | OP_OPIMM);
    CHECK_EQ(d.alu_op, ALU_SRL);                // srli
    decode((0b0100000 << 25) | (4 << 20) | (1 << 15) | (0b101 << 12) | (5 << 7) | OP_OPIMM);
    CHECK_EQ(d.alu_op, ALU_SRA);                // srai
    decode((0b0000000 << 25) | (4 << 20) | (1 << 15) | (0b001 << 12) | (5 << 7) | OP_OPIMM);
    CHECK_EQ(d.alu_op, ALU_SLL);                // slli

    tb_begin("6. loads");
    decode(enc_i(OP_LOAD, 5, 0b010, 1, 8));     // lw
    CHECK_EQ(d.alu_op,  ALU_ADD);               // address arithmetic
    CHECK_EQ(d.alu_src, 1);
    CHECK_EQ(d.reg_we,  1);
    CHECK_EQ(d.mem_re,  1);
    CHECK_EQ(d.mem_we,  0);
    CHECK_EQ(d.wb_sel,  1);                     // from memory
    CHECK_EQ(d.funct3,  0b010);                 // forwarded to dmem
    decode(enc_i(OP_LOAD, 5, 0b100, 1, 8));     // lbu
    CHECK_EQ(d.funct3, 0b100);
    CHECK_EQ(d.alu_op, ALU_ADD);                // still ADD, not derived from f3

    tb_begin("7. stores");
    decode(enc_s(0b010, 1, 2, 8));              // sw x2, 8(x1)
    CHECK_EQ(d.alu_op,   ALU_ADD);
    CHECK_EQ(d.alu_src,  1);                    // immediate, for the address
    CHECK_EQ(d.reg_we,   0);
    CHECK_EQ(d.mem_we,   1);
    CHECK_EQ(d.mem_re,   0);
    CHECK_EQ(d.rs1_addr, 1);
    CHECK_EQ(d.rs2_addr, 2);                    // data source

    tb_begin("8. branches");
    decode((1 << 15) | (2 << 20) | (0b000 << 12) | OP_BRANCH);   // beq
    CHECK_EQ(d.is_branch, 1);
    CHECK_EQ(d.reg_we,    0);
    CHECK_EQ(d.mem_we,    0);
    CHECK_EQ(d.is_jal,    0);
    CHECK_EQ(d.is_jalr,   0);

    tb_begin("9. jumps");
    decode((1 << 7) | OP_JAL);
    CHECK_EQ(d.is_jal,  1);
    CHECK_EQ(d.is_jalr, 0);
    CHECK_EQ(d.reg_we,  1);
    CHECK_EQ(d.wb_sel,  2);                     // PC+4
    decode(enc_i(OP_JALR, 1, 0, 1, 0));
    CHECK_EQ(d.is_jalr, 1);
    CHECK_EQ(d.is_jal,  0);
    CHECK_EQ(d.reg_we,  1);
    CHECK_EQ(d.wb_sel,  2);
    CHECK_EQ(d.alu_op,  ALU_ADD);               // target = rs1 + imm
    CHECK_EQ(d.alu_src, 1);

    tb_begin("10. lui and auipc");
    decode((0x12345 << 12) | (5 << 7) | OP_LUI);
    CHECK_EQ(d.alu_a_zero, 1);
    CHECK_EQ(d.alu_a_pc,   0);
    CHECK_EQ(d.reg_we,     1);
    CHECK_EQ(d.wb_sel,     0);
    CHECK_EQ(d.alu_op,     ALU_ADD);
    decode((0x12345 << 12) | (5 << 7) | OP_AUIPC);
    CHECK_EQ(d.alu_a_pc,   1);
    CHECK_EQ(d.alu_a_zero, 0);
    CHECK_EQ(d.reg_we,     1);

    tb_begin("11. fence and system are nops");
    decode(OP_FENCE);
    CHECK_EQ(d.reg_we,  0);
    CHECK_EQ(d.mem_we,  0);
    CHECK_EQ(d.illegal, 0);
    decode(OP_SYSTEM);
    CHECK_EQ(d.reg_we,  0);
    CHECK_EQ(d.mem_we,  0);

    tb_begin("12. illegal opcodes flagged");
    decode(0x00000000);
    CHECK_EQ(d.illegal, 1);
    CHECK_EQ(d.reg_we,  0);                     // must not write on garbage
    CHECK_EQ(d.mem_we,  0);
    decode(0xFFFFFFFF);
    CHECK_EQ(d.illegal, 1);
    decode(0x0000007F);
    CHECK_EQ(d.illegal, 1);

    tb_begin("13. register fields across random instructions");
    for (int i = 0; i < 5000; i++) {
        uint32_t rd = tb.rnd(0,31), rs1 = tb.rnd(0,31), rs2 = tb.rnd(0,31);
        uint32_t f3 = tb.rnd(0,7),  f7  = tb.rnd(0,1) ? 0b0100000 : 0;
        decode(enc_r(f7, rs2, rs1, f3, rd));
        CHECK_EQ(d.rs1_addr, rs1);
        CHECK_EQ(d.rs2_addr, rs2);
        CHECK_EQ(d.rd_addr,  rd);
        CHECK_EQ(d.funct3,   f3);
        CHECK_EQ(d.alu_op,   ((f7 >> 5) << 3) | f3);
        CHECK_EQ(d.reg_we,   1);
        CHECK_EQ(d.alu_src,  0);
    }

    tb_begin("14. OP-IMM never produces SUB, across all immediates");
    for (int i = 0; i < 5000; i++) {
        int32_t  imm = (int32_t)(tb.rnd() & 0xFFF) - 2048;
        uint32_t f3  = tb.rnd(0,7);
        if (f3 == 0b001 || f3 == 0b101) continue;     // shifts use the field
        decode(enc_i(OP_OPIMM, 5, f3, 1, imm));
        CHECK_EQ(d.alu_op & 0b1000, 0);               // bit 3 always clear
        CHECK_EQ(d.alu_op & 0b0111, f3);
    }

    return tb.finish();
}
