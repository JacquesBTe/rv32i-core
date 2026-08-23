`default_nettype none

// UART: bridges axil_reg_if to the vendored AXI-Stream uart_rx/uart_tx.
//
//   0x00  DATA      write -> transmit byte;  read -> receive byte (pops it)
//   0x04  STATUS    bit 0 tx_busy, bit 1 rx_valid, bit 2 overrun,
//                   bit 3 frame_error
//   0x08  PRESCALE  baud divisor, clk / (baud * 8)
//
// Reading DATA never stalls: if rx_valid is low it just returns whatever
// is in the (stale/undefined) receive register, on the assumption software
// polls STATUS.bit1 first. This is deliberately the simpler of the two
// options in the phase 5 plan (the other being reg_rd_wait until a byte
// arrives) -- safer for bring-up since a wedged RX line can never hang a
// bus read.
//
// Writing DATA does stall (reg_wr_wait) until uart_tx is ready for a new
// byte, so back-to-back writes to a busy transmitter block instead of
// silently dropping bytes -- unlike the read side, there's no polling
// convention that makes dropping acceptable here.
//
// overrun/frame_error from uart_rx are one-cycle pulses; this module
// latches them into sticky STATUS bits that clear when STATUS is read,
// so a slow poller can't miss one.

module uart_axil #(
    parameter [15:0] PRESCALE_INIT = 16'd81   // 75 MHz / (115200 * 8)
) (
    input  wire        clk,
    input  wire        rst_n,

    output wire        txd,
    input  wire        rxd,

    // ---- AXI4-Lite slave ----
    input  wire [31:0] s_axil_awaddr,
    input  wire [2:0]  s_axil_awprot,
    input  wire        s_axil_awvalid,
    output wire        s_axil_awready,
    input  wire [31:0] s_axil_wdata,
    input  wire [3:0]  s_axil_wstrb,
    input  wire        s_axil_wvalid,
    output wire        s_axil_wready,
    output wire [1:0]  s_axil_bresp,
    output wire        s_axil_bvalid,
    input  wire        s_axil_bready,
    input  wire [31:0] s_axil_araddr,
    input  wire [2:0]  s_axil_arprot,
    input  wire        s_axil_arvalid,
    output wire        s_axil_arready,
    output wire [31:0] s_axil_rdata,
    output wire [1:0]  s_axil_rresp,
    output wire        s_axil_rvalid,
    input  wire        s_axil_rready
);

    wire        reg_wr_en;
    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] reg_wr_addr;
    /* verilator lint_on UNUSEDSIGNAL */
    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] reg_wr_data;
    /* verilator lint_on UNUSEDSIGNAL */
    /* verilator lint_off UNUSEDSIGNAL */
    wire [3:0]  reg_wr_strb;
    /* verilator lint_on UNUSEDSIGNAL */

    wire        reg_rd_en;
    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] reg_rd_addr;
    /* verilator lint_on UNUSEDSIGNAL */
    reg  [31:0] reg_rd_data;

    wire wr_sel_data  = (reg_wr_addr[3:2] == 2'b00);
    wire wr_sel_presc = (reg_wr_addr[3:2] == 2'b10);
    wire rd_sel_status = (reg_rd_addr[3:2] == 2'b01);

    // ---- uart_tx side ----
    wire       tx_tready;
    wire       tx_busy;
    wire       tx_tvalid = reg_wr_en && wr_sel_data && tx_tready;

    wire reg_wr_wait = reg_wr_en && wr_sel_data && !tx_tready;
    wire reg_wr_ack  = reg_wr_en && (!wr_sel_data || tx_tready);

    reg [15:0] prescale_reg;
    always @(posedge clk) begin
        if (!rst_n)                            prescale_reg <= PRESCALE_INIT;
        else if (reg_wr_en && wr_sel_presc)    prescale_reg <= reg_wr_data[15:0];
    end

    uart_tx #(.DATA_WIDTH(8)) u_tx (
        .clk (clk),
        .rst (!rst_n),

        .s_axis_tdata  (reg_wr_data[7:0]),
        .s_axis_tvalid (tx_tvalid),
        .s_axis_tready (tx_tready),

        .txd (txd),

        .busy (tx_busy),

        .prescale (prescale_reg)
    );

    // ---- uart_rx side ----
    wire [7:0] rx_tdata;
    wire       rx_tvalid;
    wire       rx_overrun, rx_frame;

    wire data_read_fire   = reg_rd_en && (reg_rd_addr[3:2] == 2'b00);
    wire status_read_fire = reg_rd_en && rd_sel_status;
    wire rx_tready         = data_read_fire && rx_tvalid;

    uart_rx #(.DATA_WIDTH(8)) u_rx (
        .clk (clk),
        .rst (!rst_n),

        .m_axis_tdata  (rx_tdata),
        .m_axis_tvalid (rx_tvalid),
        .m_axis_tready (rx_tready),

        .rxd (rxd),

        /* verilator lint_off PINCONNECTEMPTY */
        .busy          (),
        /* verilator lint_on PINCONNECTEMPTY */
        .overrun_error (rx_overrun),
        .frame_error   (rx_frame),

        .prescale (prescale_reg)
    );

    reg overrun_sticky, frame_sticky;
    always @(posedge clk) begin
        if (!rst_n) begin
            overrun_sticky <= 1'b0;
            frame_sticky   <= 1'b0;
        end else begin
            // Clear-then-set ordering: a pulse landing on the same cycle
            // as the read that would have cleared it survives for the
            // next poll instead of being silently lost.
            if (status_read_fire) overrun_sticky <= 1'b0;
            if (status_read_fire) frame_sticky   <= 1'b0;
            if (rx_overrun)       overrun_sticky <= 1'b1;
            if (rx_frame)         frame_sticky   <= 1'b1;
        end
    end

    // ---- register interface ----
    axil_reg_if #(
        .DATA_WIDTH(32),
        .ADDR_WIDTH(32),
        .STRB_WIDTH(4)
    ) u_reg_if (
        .clk(clk),
        .rst(!rst_n),

        .s_axil_awaddr  (s_axil_awaddr),
        .s_axil_awprot  (s_axil_awprot),
        .s_axil_awvalid (s_axil_awvalid),
        .s_axil_awready (s_axil_awready),
        .s_axil_wdata   (s_axil_wdata),
        .s_axil_wstrb   (s_axil_wstrb),
        .s_axil_wvalid  (s_axil_wvalid),
        .s_axil_wready  (s_axil_wready),
        .s_axil_bresp   (s_axil_bresp),
        .s_axil_bvalid  (s_axil_bvalid),
        .s_axil_bready  (s_axil_bready),
        .s_axil_araddr  (s_axil_araddr),
        .s_axil_arprot  (s_axil_arprot),
        .s_axil_arvalid (s_axil_arvalid),
        .s_axil_arready (s_axil_arready),
        .s_axil_rdata   (s_axil_rdata),
        .s_axil_rresp   (s_axil_rresp),
        .s_axil_rvalid  (s_axil_rvalid),
        .s_axil_rready  (s_axil_rready),

        .reg_wr_addr (reg_wr_addr),
        .reg_wr_data (reg_wr_data),
        .reg_wr_strb (reg_wr_strb),
        .reg_wr_en   (reg_wr_en),
        .reg_wr_wait (reg_wr_wait),
        .reg_wr_ack  (reg_wr_ack),

        .reg_rd_addr (reg_rd_addr),
        .reg_rd_en   (reg_rd_en),
        .reg_rd_data (reg_rd_data),
        .reg_rd_wait (1'b0),
        .reg_rd_ack  (reg_rd_en)
    );

    always @(*) begin
        case (reg_rd_addr[3:2])
            2'b00:   reg_rd_data = {24'b0, rx_tdata};
            2'b01:   reg_rd_data = {28'b0, frame_sticky, overrun_sticky,
                                     rx_tvalid, tx_busy};
            2'b10:   reg_rd_data = {16'b0, prescale_reg};
            default: reg_rd_data = 32'b0;
        endcase
    end

endmodule
