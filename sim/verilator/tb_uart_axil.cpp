#include "tb_common.h"
#include "Vuart_axil.h"

enum {
    REG_DATA     = 0x00,
    REG_STATUS   = 0x04,
    REG_PRESCALE = 0x08,

    ST_TX_BUSY = 1u << 0,
    ST_RX_VALID = 1u << 1,
    ST_OVERRUN  = 1u << 2,
    ST_FRAME_ERR = 1u << 3,
};

int main(int argc, char** argv) {
    Tb<Vuart_axil> tb(argc, argv, "uart_axil");
    auto& d = tb.dut;

    // txd/rxd loopback: every tick, whatever this cycle's tick() drives
    // through is preceded by copying the current txd onto rxd, so the
    // module always sees its own transmit line unless a test overrides
    // rxd directly (the frame-error test below).
    bool loopback = true;
    auto tick = [&]() {
        if (loopback) d.rxd = d.txd;
        tb.tick();
    };

    auto axil_write = [&](uint32_t addr, uint32_t data, uint8_t strb = 0xF) {
        d.s_axil_awaddr = addr; d.s_axil_awprot = 0; d.s_axil_awvalid = 1;
        d.s_axil_wdata  = data; d.s_axil_wstrb  = strb; d.s_axil_wvalid = 1;
        d.s_axil_bready = 1;
        bool aw_acc = false, w_acc = false, got_b = false;
        for (int i = 0; i < 3000 && !got_b; i++) {
            if (loopback) d.rxd = d.txd;
            tb.dut.eval();
            if (!aw_acc && d.s_axil_awready) aw_acc = true;
            if (!w_acc  && d.s_axil_wready)  w_acc  = true;
            bool bv = d.s_axil_bvalid;
            tb.tick();
            if (aw_acc) d.s_axil_awvalid = 0;
            if (w_acc)  d.s_axil_wvalid  = 0;
            if (bv) got_b = true;
        }
        d.s_axil_bready = 0;
        CHECK_EQ(got_b, true);
    };

    auto axil_read = [&](uint32_t addr) -> uint32_t {
        d.s_axil_araddr = addr; d.s_axil_arprot = 0; d.s_axil_arvalid = 1;
        d.s_axil_rready = 1;
        bool ar_acc = false, got_r = false;
        uint32_t data = 0;
        for (int i = 0; i < 3000 && !got_r; i++) {
            if (loopback) d.rxd = d.txd;
            tb.dut.eval();
            if (!ar_acc && d.s_axil_arready) ar_acc = true;
            bool rv = d.s_axil_rvalid;
            if (rv) data = d.s_axil_rdata;
            tb.tick();
            if (ar_acc) d.s_axil_arvalid = 0;
            if (rv) got_r = true;
        }
        d.s_axil_rready = 0;
        CHECK_EQ(got_r, true);
        return data;
    };

    // Poll STATUS until (status & mask) == want, bounded so a stuck DUT
    // fails the test instead of hanging the run.
    auto wait_status = [&](uint32_t mask, uint32_t want, int max_polls) -> uint32_t {
        uint32_t st = 0;
        for (int i = 0; i < max_polls; i++) {
            st = axil_read(REG_STATUS);
            if ((st & mask) == want) return st;
        }
        return st;
    };

    d.clk = 0; d.rst_n = 0; d.rxd = 1;
    d.s_axil_awvalid = 0; d.s_axil_wvalid = 0; d.s_axil_bready = 0;
    d.s_axil_arvalid = 0; d.s_axil_rready = 0;
    tick(); tick();
    d.rst_n = 1;

    const uint32_t PRESCALE = 4;   // small: 8x oversample x 4 = 32 cyc/bit
    const int BIT_PERIOD    = PRESCALE * 8;

    tb_begin("1. reset state: idle, no errors");
    CHECK_EQ(axil_read(REG_STATUS) & 0xF, 0);

    tb_begin("2. prescale write/readback");
    axil_write(REG_PRESCALE, PRESCALE);
    CHECK_EQ(axil_read(REG_PRESCALE), PRESCALE);

    tb_begin("3. single byte, loopback");
    axil_write(REG_DATA, 0xA5);
    CHECK_EQ(axil_read(REG_STATUS) & ST_TX_BUSY, ST_TX_BUSY);
    uint32_t st = wait_status(ST_RX_VALID, ST_RX_VALID, 200);
    CHECK_EQ(st & ST_RX_VALID, ST_RX_VALID);
    CHECK_EQ(axil_read(REG_DATA) & 0xFF, 0xA5);
    CHECK_EQ(axil_read(REG_STATUS) & ST_RX_VALID, 0);   // consumed by the read

    tb_begin("4. tx_busy clears once the byte is fully sent");
    st = wait_status(ST_TX_BUSY, 0, 200);
    CHECK_EQ(st & ST_TX_BUSY, 0);

    tb_begin("5. several bytes in sequence, read between each");
    for (uint32_t v = 0; v < 8; v++) {
        uint8_t byte = (uint8_t)(0x10 * v + 1);
        axil_write(REG_DATA, byte);
        st = wait_status(ST_RX_VALID, ST_RX_VALID, 200);
        CHECK_EQ(st & ST_RX_VALID, ST_RX_VALID);
        CHECK_EQ(axil_read(REG_DATA) & 0xFF, byte);
    }

    tb_begin("6. back-to-back writes stall on a busy transmitter, not lost");
    axil_write(REG_DATA, 0x11);
    axil_write(REG_DATA, 0x22);          // must block until byte 1 is sent
    st = wait_status(ST_RX_VALID, ST_RX_VALID, 200);
    CHECK_EQ(axil_read(REG_DATA) & 0xFF, 0x11);
    st = wait_status(ST_RX_VALID, ST_RX_VALID, 200);
    CHECK_EQ(axil_read(REG_DATA) & 0xFF, 0x22);

    tb_begin("7. overrun: second byte lands before the first is read");
    axil_write(REG_DATA, 0x33);
    st = wait_status(ST_RX_VALID, ST_RX_VALID, 200);
    CHECK_EQ(st & ST_OVERRUN, 0);
    axil_write(REG_DATA, 0x44);          // sent while 0x33 still unread
    st = wait_status(ST_OVERRUN, ST_OVERRUN, 200);
    CHECK_EQ(st & ST_OVERRUN, ST_OVERRUN);
    CHECK_EQ(st & ST_RX_VALID, ST_RX_VALID);
    CHECK_EQ(axil_read(REG_DATA) & 0xFF, 0x44);   // newer byte survives
    CHECK_EQ(axil_read(REG_STATUS) & ST_OVERRUN, 0);   // cleared by the read

    tb_begin("8. frame error: no stop bit");
    {
        // Bit-bang a byte directly onto rxd with the stop bit held low
        // instead of high. loopback is suspended for this section since
        // it drives rxd itself.
        loopback = false;
        d.rxd = 1;
        for (int i = 0; i < BIT_PERIOD; i++) tick();      // idle margin

        auto hold = [&](int v) { d.rxd = v; for (int i = 0; i < BIT_PERIOD; i++) tick(); };
        hold(0);                              // start bit
        uint8_t byte = 0x5A;
        for (int b = 0; b < 8; b++) hold((byte >> b) & 1);

        // Bad stop bit: low only through the sample point, then release
        // early. Holding it low for a full bit period (like every other
        // bit here) would leave rxd low well past the cycle the
        // receiver's own state machine samples it and returns to idle --
        // it would read that trailing low as the start of a bogus second
        // byte. Releasing partway through avoids that without needing to
        // hand-derive uart_rx's exact internal sample offset.
        d.rxd = 0;
        for (int i = 0; i < BIT_PERIOD * 5 / 8; i++) tick();
        d.rxd = 1;
        for (int i = 0; i < BIT_PERIOD * 3 / 8; i++) tick();

        for (int i = 0; i < BIT_PERIOD; i++) tick();   // idle margin

        st = wait_status(ST_FRAME_ERR, ST_FRAME_ERR, 200);
        CHECK_EQ(st & ST_FRAME_ERR, ST_FRAME_ERR);
        CHECK_EQ(axil_read(REG_STATUS) & ST_FRAME_ERR, 0);   // cleared by read

        loopback = true;
        d.rxd = d.txd;
    }

    tb_begin("9. random bytes through loopback");
    for (int i = 0; i < 60; i++) {
        uint8_t byte = (uint8_t)tb.rnd(0, 255);
        axil_write(REG_DATA, byte);
        st = wait_status(ST_RX_VALID, ST_RX_VALID, 200);
        CHECK_EQ(st & ST_RX_VALID, ST_RX_VALID);
        CHECK_EQ(axil_read(REG_DATA) & 0xFF, byte);
    }

    return tb.finish();
}
