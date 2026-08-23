#include "tb_common.h"
#include "Vcsr.h"

// CSR addresses
enum {
    CSR_MSTATUS  = 0x300,
    CSR_MISA     = 0x301,
    CSR_MEDELEG  = 0x302,
    CSR_MIDELEG  = 0x303,
    CSR_MIE      = 0x304,
    CSR_MTVEC    = 0x305,
    CSR_MSCRATCH = 0x340,
    CSR_MEPC     = 0x341,
    CSR_MCAUSE   = 0x342,
    CSR_MTVAL    = 0x343,
    CSR_MIP      = 0x344,
    CSR_SATP     = 0x180,
    CSR_PMPCFG0  = 0x3A0,
    CSR_PMPADDR0 = 0x3B0,
    CSR_MHARTID  = 0xF14,
    CSR_MNSTATUS = 0x744,      // deliberately NOT implemented -- must trap
    CSR_BOGUS    = 0x123,      // nonexistent -- must trap
};

// csr_op encodings
enum { OP_NONE = 0b00, OP_RW = 0b01, OP_RS = 0b10, OP_RC = 0b11 };

int main(int argc, char** argv) {
    Tb<Vcsr> tb(argc, argv, "csr");
    auto& d = tb.dut;

    // idle state
    d.clk = 0; d.rst_n = 0;
    d.csr_addr = 0; d.csr_wdata = 0; d.csr_op = OP_NONE;
    d.csr_re = 0; d.csr_we = 0;
    d.trap = 0; d.trap_pc = 0; d.trap_cause = 0;
    tb.tick(); tb.tick();
    d.rst_n = 1;
    tb.tick();

    // Read a CSR without writing. Combinational read, so settle only.
    auto rd = [&](uint32_t addr) -> uint32_t {
        d.csr_addr = addr; d.csr_op = OP_RS; d.csr_wdata = 0;
        d.csr_re = 1; d.csr_we = 0;
        tb.settle();
        uint32_t v = d.csr_rdata;
        d.csr_re = 0; d.csr_op = OP_NONE;
        return v;
    };

    // Perform one CSR access and commit it. Returns the pre-write value,
    // which is what the instruction writes back to rd.
    auto access = [&](uint32_t addr, uint32_t op, uint32_t wdata,
                      bool re, bool we) -> uint32_t {
        d.csr_addr = addr; d.csr_op = op; d.csr_wdata = wdata;
        d.csr_re = re ? 1 : 0; d.csr_we = we ? 1 : 0;
        tb.settle();
        uint32_t old = d.csr_rdata;
        tb.tick();
        d.csr_re = 0; d.csr_we = 0; d.csr_op = OP_NONE;
        return old;
    };

    auto illegal = [&](uint32_t addr, uint32_t op, bool re, bool we) -> bool {
        d.csr_addr = addr; d.csr_op = op; d.csr_wdata = 0;
        d.csr_re = re ? 1 : 0; d.csr_we = we ? 1 : 0;
        tb.settle();
        bool v = d.csr_illegal;
        d.csr_re = 0; d.csr_we = 0; d.csr_op = OP_NONE;
        tb.settle();
        return v;
    };

    tb_begin("1. reset state");
    CHECK_EQ(rd(CSR_MSTATUS),  0);
    CHECK_EQ(rd(CSR_MTVEC),    0);
    CHECK_EQ(rd(CSR_MSCRATCH), 0);
    CHECK_EQ(rd(CSR_MEPC),     0);
    CHECK_EQ(rd(CSR_MCAUSE),   0);

    tb_begin("2. csrrw -- write then read back");
    access(CSR_MSCRATCH, OP_RW, 0xDEADBEEF, true, true);
    CHECK_EQ(rd(CSR_MSCRATCH), 0xDEADBEEF);
    access(CSR_MTVEC, OP_RW, 0x80000100, true, true);
    CHECK_EQ(rd(CSR_MTVEC), 0x80000100);

    tb_begin("3. csrrw returns the OLD value");
    // The instruction writes the pre-write contents to rd, so the value
    // observed on csr_rdata during the access must be what was there
    // before, not what is being written.
    uint32_t old = access(CSR_MSCRATCH, OP_RW, 0x11112222, true, true);
    CHECK_EQ(old, 0xDEADBEEF);
    CHECK_EQ(rd(CSR_MSCRATCH), 0x11112222);

    tb_begin("4. csrrs sets bits, csrrc clears them");
    access(CSR_MSCRATCH, OP_RW, 0x0000FFFF, true, true);
    access(CSR_MSCRATCH, OP_RS, 0x00FF0000, true, true);
    CHECK_EQ(rd(CSR_MSCRATCH), 0x00FFFFFF);
    access(CSR_MSCRATCH, OP_RC, 0x000000FF, true, true);
    CHECK_EQ(rd(CSR_MSCRATCH), 0x00FFFF00);
    // set/clear also return the old value
    old = access(CSR_MSCRATCH, OP_RS, 0xFF000000, true, true);
    CHECK_EQ(old, 0x00FFFF00);
    CHECK_EQ(rd(CSR_MSCRATCH), 0xFFFFFF00);

    tb_begin("5. csr_we low means no write");
    access(CSR_MSCRATCH, OP_RW, 0x55555555, true, true);
    d.csr_addr = CSR_MSCRATCH; d.csr_op = OP_RW; d.csr_wdata = 0xAAAAAAAA;
    d.csr_re = 1; d.csr_we = 0;
    tb.tick();
    d.csr_re = 0;
    CHECK_EQ(rd(CSR_MSCRATCH), 0x55555555);

    tb_begin("6. mhartid reads zero and ignores writes");
    CHECK_EQ(rd(CSR_MHARTID), 0);
    access(CSR_MHARTID, OP_RW, 0xFFFFFFFF, true, true);
    CHECK_EQ(rd(CSR_MHARTID), 0);
    // and it must not trap -- writes are dropped, not faulted
    CHECK_EQ(illegal(CSR_MHARTID, OP_RW, true, true), false);

    tb_begin("7. recognised-but-ignored CSRs accept writes, read zero");
    // riscv-tests init writes all of these; trapping would derail it.
    const uint32_t ignored[] = {CSR_MISA, CSR_MEDELEG, CSR_MIDELEG, CSR_MIE,
                                CSR_MTVAL, CSR_MIP, CSR_SATP,
                                CSR_PMPCFG0, CSR_PMPADDR0};
    for (uint32_t a : ignored) {
        CHECK_EQ(illegal(a, OP_RW, true, true), false);
        access(a, OP_RW, 0xFFFFFFFF, true, true);
        CHECK_EQ(rd(a), 0);
    }

    tb_begin("8. unimplemented CSRs raise illegal");
    // mnstatus is deliberately absent: riscv-tests probes it and relies on
    // the trap to skip the following instruction.
    CHECK_EQ(illegal(CSR_MNSTATUS, OP_RW, true, true),  true);
    CHECK_EQ(illegal(CSR_BOGUS,    OP_RW, true, true),  true);
    CHECK_EQ(illegal(CSR_BOGUS,    OP_RS, true, false), true);

    tb_begin("9. illegal only fires on a real access");
    // Every instruction presents instr[31:20] on csr_addr, so a plain addi
    // whose immediate looks like a bad CSR number must not trap.
    CHECK_EQ(illegal(CSR_BOGUS, OP_NONE, false, false), false);
    CHECK_EQ(illegal(0x7FF,     OP_NONE, false, false), false);

    tb_begin("10. trap entry writes mepc and mcause");
    access(CSR_MEPC,   OP_RW, 0, true, true);
    access(CSR_MCAUSE, OP_RW, 0, true, true);
    d.trap = 1; d.trap_pc = 0x80000234; d.trap_cause = 2;
    tb.tick();
    d.trap = 0;
    CHECK_EQ(rd(CSR_MEPC),   0x80000234);
    CHECK_EQ(rd(CSR_MCAUSE), 2);

    tb_begin("11. trap entry outranks an instruction write");
    // The faulting instruction must not also commit its own CSR update.
    access(CSR_MEPC, OP_RW, 0xAAAAAAAA, true, true);
    d.csr_addr = CSR_MEPC; d.csr_op = OP_RW; d.csr_wdata = 0xBBBBBBBB;
    d.csr_re = 1; d.csr_we = 1;
    d.trap = 1; d.trap_pc = 0x80000500; d.trap_cause = 11;
    tb.tick();
    d.trap = 0; d.csr_re = 0; d.csr_we = 0; d.csr_op = OP_NONE;
    CHECK_EQ(rd(CSR_MEPC),   0x80000500);      // trap won
    CHECK_EQ(rd(CSR_MCAUSE), 11);

    tb_begin("12. mtvec_out and mepc_out track their registers");
    access(CSR_MTVEC, OP_RW, 0x80001000, true, true);
    access(CSR_MEPC,  OP_RW, 0x80002000, true, true);
    tb.settle();
    CHECK_EQ(d.mtvec_out, 0x80001000);
    CHECK_EQ(d.mepc_out,  0x80002000);

    tb_begin("13. random read/write against a shadow model");
    // Only the five writable registers; mhartid and the ignored list have
    // their own sections above.
    const uint32_t rw[] = {CSR_MSTATUS, CSR_MTVEC, CSR_MSCRATCH,
                           CSR_MEPC, CSR_MCAUSE};
    uint32_t model[5];
    for (int i = 0; i < 5; i++) model[i] = rd(rw[i]);

    for (int i = 0; i < 5000; i++) {
        int      k = tb.rnd(0, 4);
        uint32_t v = tb.rnd();
        uint32_t op = OP_RW + tb.rnd(0, 2);        // 01, 10 or 11

        uint32_t got_old = access(rw[k], op, v, true, true);
        CHECK_EQ(got_old, model[k]);               // returns pre-write value

        switch (op) {
            case OP_RW: model[k] = v;              break;
            case OP_RS: model[k] |= v;             break;
            case OP_RC: model[k] &= ~v;            break;
        }
        CHECK_EQ(rd(rw[k]), model[k]);
    }

    return tb.finish();
}
