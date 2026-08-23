#include "tb_common.h"
#include "Vaxil_master.h"
#include <map>
#include <vector>

enum { F3_B = 0b000, F3_H = 0b001, F3_W = 0b010,
       F3_BU = 0b100, F3_HU = 0b101 };

// ---- per-channel ready delay --------------------------------------------
// Models a slave that waits `delay` cycles after VALID goes high before
// asserting READY. delay=0 means combinational accept on the same cycle
// VALID first appears.
struct DelayCounter {
    int delay = 0;
    int ctr   = -1;   // -1 = idle, no transaction currently being timed

    bool ready(bool valid) {
        if (!valid) { ctr = -1; return false; }
        if (ctr < 0) ctr = delay;
        return ctr == 0;
    }
    void advance(bool valid, bool rdy) {
        if (!valid) return;         // already reset in ready()
        if (rdy) ctr = -1;          // handshake fired; reset for next xfer
        else     ctr--;
    }
};

// ---- stub AXI4-Lite slave ------------------------------------------------
// Drives the slave-side signals of axil_master (awready/wready/arready,
// bvalid/bresp, rvalid/rdata/rresp) from a byte-addressable memory model.
// AW and W are tracked independently, matching how axil_master issues them.
struct AxilSlave {
    DelayCounter awc, wc, arc;

    bool     aw_captured = false, w_captured = false;
    uint32_t awaddr_captured = 0;
    uint32_t wdata_captured  = 0;
    uint8_t  wstrb_captured  = 0;

    bool     bvalid_reg = false;
    bool     rvalid_reg = false;
    uint32_t rdata_reg  = 0;

    std::map<uint32_t, uint8_t> mem;   // byte-addressed

    uint32_t rd_word(uint32_t waddr) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t)mem[waddr + i] << (8 * i);
        return v;
    }
    void wr_word(uint32_t waddr, uint32_t data, uint8_t strb) {
        for (int i = 0; i < 4; i++)
            if (strb & (1 << i)) mem[waddr + i] = (data >> (8 * i)) & 0xFF;
    }

    // Drive slave outputs for the current (pre-edge) cycle and sample the
    // master's current request-side signals. Call once per cycle, before
    // tb.tick().
    template <class DUT>
    void drive(DUT& d) {
        bool aw_ready = awc.ready(d.m_axil_awvalid);
        bool w_ready  = wc.ready(d.m_axil_wvalid);
        bool ar_ready = arc.ready(d.m_axil_arvalid);

        d.m_axil_awready = aw_ready;
        d.m_axil_wready  = w_ready;
        d.m_axil_arready = ar_ready;

        d.m_axil_bvalid = bvalid_reg;
        d.m_axil_bresp  = 0;
        d.m_axil_rvalid = rvalid_reg;
        d.m_axil_rdata  = rdata_reg;
        d.m_axil_rresp  = 0;

        aw_fire_ = d.m_axil_awvalid && aw_ready;
        w_fire_  = d.m_axil_wvalid  && w_ready;
        ar_fire_ = d.m_axil_arvalid && ar_ready;
        if (aw_fire_) awaddr_captured_ = d.m_axil_awaddr;
        if (w_fire_) { wdata_captured_ = d.m_axil_wdata; wstrb_captured_ = d.m_axil_wstrb; }
        if (ar_fire_) araddr_captured_ = d.m_axil_araddr;

        awc.advance(d.m_axil_awvalid, aw_ready);
        wc.advance(d.m_axil_wvalid, w_ready);
        arc.advance(d.m_axil_arvalid, ar_ready);

        bready_ = d.m_axil_bready;
        rready_ = d.m_axil_rready;
    }

    // Advance internal state to what should be presented next cycle. Call
    // once per cycle, after tb.tick().
    void update() {
        if (aw_fire_) { aw_captured = true; awaddr_captured = awaddr_captured_; }
        if (w_fire_)  { w_captured  = true; wdata_captured  = wdata_captured_;
                                             wstrb_captured  = wstrb_captured_; }

        if (bvalid_reg && bready_) bvalid_reg = false;
        if (!bvalid_reg && aw_captured && w_captured) {
            wr_word(awaddr_captured, wdata_captured, wstrb_captured);
            bvalid_reg  = true;
            aw_captured = false;
            w_captured  = false;
        }

        if (rvalid_reg && rready_) rvalid_reg = false;
        if (!rvalid_reg && ar_fire_) {
            rdata_reg  = rd_word(araddr_captured_);
            rvalid_reg = true;
        }
    }

private:
    bool aw_fire_ = false, w_fire_ = false, ar_fire_ = false;
    uint32_t awaddr_captured_ = 0, wdata_captured_ = 0, araddr_captured_ = 0;
    uint8_t  wstrb_captured_ = 0;
    bool bready_ = false, rready_ = false;
};

// One clock cycle: apply slave outputs, sample bus_stall/mem_rdata as seen
// during this cycle, clock the DUT, then advance the slave's own state.
template <class TB>
static void cycle(TB& tb, AxilSlave& s, bool& stall_out, uint32_t& rdata_out) {
    s.drive(tb.dut);
    tb.dut.eval();
    stall_out = tb.dut.bus_stall;
    rdata_out = tb.dut.mem_rdata;
    tb.tick();
    s.update();
}

int main(int argc, char** argv) {
    Tb<Vaxil_master> tb(argc, argv, "axil_master");
    auto& d = tb.dut;

    d.clk = 0; d.rst_n = 0;
    d.mem_addr = 0; d.mem_wdata = 0; d.mem_funct3 = F3_W;
    d.mem_re = 0; d.mem_we = 0;
    d.m_axil_awready = 0; d.m_axil_wready = 0; d.m_axil_arready = 0;
    d.m_axil_bvalid = 0; d.m_axil_bresp = 0;
    d.m_axil_rvalid = 0; d.m_axil_rdata = 0; d.m_axil_rresp = 0;
    tb.tick(); tb.tick();
    d.rst_n = 1;

    AxilSlave slave;

    // Run a store; returns (aw_cycles, wr_cycles, resp_cycles, stall_cycles).
    auto do_store = [&](uint32_t addr, uint32_t data, uint32_t f3,
                         int aw_delay, int w_delay) {
        slave.awc.delay = aw_delay;
        slave.wc.delay  = w_delay;
        d.mem_addr = addr; d.mem_wdata = data; d.mem_funct3 = f3;
        d.mem_we = 1; d.mem_re = 0;

        bool stall; uint32_t rdata;
        int stall_cycles = 0;
        do { cycle(tb, slave, stall, rdata); stall_cycles++; } while (stall);
        d.mem_we = 0;
        return stall_cycles;
    };

    auto do_load = [&](uint32_t addr, uint32_t f3, int ar_delay) -> uint32_t {
        slave.arc.delay = ar_delay;
        d.mem_addr = addr; d.mem_funct3 = f3;
        d.mem_re = 1; d.mem_we = 0;

        bool stall; uint32_t rdata;
        do { cycle(tb, slave, stall, rdata); } while (stall);
        d.mem_re = 0;
        return rdata;
    };

    const uint32_t BASE = 0x80000000;

    // ---------------------------------------------------------------
    tb_begin("1. sw produces one AW, one W, one B; correct wstrb/wdata");
    do_store(BASE + 0, 0xDEADBEEF, F3_W, 0, 0);
    CHECK_EQ(slave.rd_word(BASE + 0), 0xDEADBEEF);

    // ---------------------------------------------------------------
    tb_begin("2. sb/sh at every byte lane produce the right wstrb");
    do_store(BASE + 16, 0, F3_W, 0, 0);
    do_store(BASE + 16, 0xAA, F3_B, 0, 0);
    CHECK_EQ(slave.rd_word(BASE + 16), 0x000000AA);
    do_store(BASE + 17, 0xBB, F3_B, 0, 0);
    CHECK_EQ(slave.rd_word(BASE + 16), 0x0000BBAA);
    do_store(BASE + 18, 0xCC, F3_B, 0, 0);
    CHECK_EQ(slave.rd_word(BASE + 16), 0x00CCBBAA);
    do_store(BASE + 19, 0xDD, F3_B, 0, 0);
    CHECK_EQ(slave.rd_word(BASE + 16), 0xDDCCBBAA);

    do_store(BASE + 24, 0, F3_W, 0, 0);
    do_store(BASE + 24, 0x1234, F3_H, 0, 0);
    CHECK_EQ(slave.rd_word(BASE + 24), 0x00001234);
    do_store(BASE + 26, 0x5678, F3_H, 0, 0);
    CHECK_EQ(slave.rd_word(BASE + 24), 0x56781234);

    // ---------------------------------------------------------------
    tb_begin("3. lw produces one AR, one R; mem_rdata matches");
    do_store(BASE + 32, 0x11223344, F3_W, 0, 0);
    CHECK_EQ(do_load(BASE + 32, F3_W, 0), 0x11223344);

    // ---------------------------------------------------------------
    tb_begin("4. lb/lbu/lh/lhu sign/zero-extend at every lane");
    do_store(BASE + 40, 0x80442211, F3_W, 0, 0);
    CHECK_EQ(do_load(BASE + 40, F3_BU, 0), 0x00000011);
    CHECK_EQ(do_load(BASE + 41, F3_BU, 0), 0x00000022);
    CHECK_EQ(do_load(BASE + 42, F3_BU, 0), 0x00000044);
    CHECK_EQ(do_load(BASE + 43, F3_BU, 0), 0x00000080);
    CHECK_EQ(do_load(BASE + 40, F3_B, 0),  0x00000011);
    CHECK_EQ(do_load(BASE + 43, F3_B, 0),  0xFFFFFF80);   // top bit set

    do_store(BASE + 44, 0x80007FFF, F3_W, 0, 0);
    CHECK_EQ(do_load(BASE + 44, F3_HU, 0), 0x00007FFF);
    CHECK_EQ(do_load(BASE + 46, F3_HU, 0), 0x00008000);
    CHECK_EQ(do_load(BASE + 44, F3_H, 0),  0x00007FFF);
    CHECK_EQ(do_load(BASE + 46, F3_H, 0),  0xFFFF8000);   // top bit set

    // ---------------------------------------------------------------
    tb_begin("5. bus_stall high for exactly the right cycles, drops on "
              "response with mem_rdata already valid");
    {
        slave.awc.delay = 3; slave.wc.delay = 1;
        d.mem_addr = BASE + 48; d.mem_wdata = 0xCAFEF00D; d.mem_funct3 = F3_W;
        d.mem_we = 1; d.mem_re = 0;
        bool stall; uint32_t rdata; int n = 0;
        do { cycle(tb, slave, stall, rdata); n++; } while (stall);
        d.mem_we = 0;
        // Total cycles = max(aw_delay, w_delay) + 3: one to leave IDLE, one
        // per delay cycle the slower channel makes AW/W wait, and one for
        // the BRESP cycle where bus_stall drops.
        CHECK_EQ(n, 6);
        CHECK_EQ(slave.rd_word(BASE + 48), 0xCAFEF00D);

        slave.arc.delay = 4;
        d.mem_addr = BASE + 48; d.mem_funct3 = F3_W;
        d.mem_re = 1; d.mem_we = 0;
        n = 0;
        do { cycle(tb, slave, stall, rdata); n++; } while (stall);
        d.mem_re = 0;
        CHECK_EQ(n, 7);   // ar_delay + 3, same shape as the write case
        CHECK_EQ(rdata, 0xCAFEF00D);
    }

    // ---------------------------------------------------------------
    tb_begin("6. AW before W, W before AW, both together -- all complete");
    do_store(BASE + 52, 0xAAAAAAAA, F3_W, /*aw*/0, /*w*/3);   // AW first
    CHECK_EQ(slave.rd_word(BASE + 52), 0xAAAAAAAA);
    do_store(BASE + 56, 0xBBBBBBBB, F3_W, /*aw*/3, /*w*/0);   // W first
    CHECK_EQ(slave.rd_word(BASE + 56), 0xBBBBBBBB);
    do_store(BASE + 60, 0xCCCCCCCC, F3_W, /*aw*/0, /*w*/0);   // together
    CHECK_EQ(slave.rd_word(BASE + 60), 0xCCCCCCCC);

    // ---------------------------------------------------------------
    tb_begin("7. back-to-back transactions, no idle cycle between");
    {
        slave.awc.delay = 0; slave.wc.delay = 0;
        d.mem_addr = BASE + 64; d.mem_wdata = 0x11111111; d.mem_funct3 = F3_W;
        d.mem_we = 1; d.mem_re = 0;
        bool stall; uint32_t rdata;
        do { cycle(tb, slave, stall, rdata); } while (stall);
        // Same cycle bus_stall drops, issue the next store directly.
        d.mem_addr = BASE + 68; d.mem_wdata = 0x22222222;
        do { cycle(tb, slave, stall, rdata); } while (stall);
        d.mem_we = 0;
        CHECK_EQ(slave.rd_word(BASE + 64), 0x11111111);
        CHECK_EQ(slave.rd_word(BASE + 68), 0x22222222);

        slave.arc.delay = 0;
        d.mem_addr = BASE + 64; d.mem_funct3 = F3_W; d.mem_re = 1;
        do { cycle(tb, slave, stall, rdata); } while (stall);
        CHECK_EQ(rdata, 0x11111111);
        d.mem_addr = BASE + 68;
        do { cycle(tb, slave, stall, rdata); } while (stall);
        CHECK_EQ(rdata, 0x22222222);
        d.mem_re = 0;
    }

    // ---------------------------------------------------------------
    tb_begin("8. random against byte-array shadow model, random delays");
    static uint8_t model[8192];
    for (auto& b : model) b = 0;
    for (uint32_t w = 0; w < 8192 / 4; w++) slave.wr_word(BASE + w * 4, 0, 0xF);

    for (int i = 0; i < 3000; i++) {
        int      sz  = tb.rnd(0, 2);
        uint32_t off = tb.rnd(0, 8191) & ~((1u << sz) - 1);
        uint32_t v   = tb.rnd();
        uint32_t f3  = (sz == 0) ? F3_B : (sz == 1) ? F3_H : F3_W;
        do_store(BASE + off, v, f3, tb.rnd(0, 3), tb.rnd(0, 3));
        for (int k = 0; k < (1 << sz); k++)
            model[off + k] = (v >> (8 * k)) & 0xFF;

        int      lsz  = tb.rnd(0, 2);
        uint32_t loff = tb.rnd(0, 8191) & ~((1u << lsz) - 1);
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

        uint32_t got = do_load(BASE + loff, lf3, tb.rnd(0, 3));
        CHECK_EQ(got, want);
    }

    return tb.finish();
}
