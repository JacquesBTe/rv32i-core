`default_nettype none

// AXI4-Lite wrapper around dmem. Uses axil_reg_if for the AW/W/AR
// handshake bookkeeping, the same way gpio does, but dmem's read is
// synchronous with one cycle of latency (unlike gpio's combinational
// register file), so the read side needs one extra wait cycle: reg_rd_wait
// holds off the ack on the cycle reg_rd_en first appears, and dmem's
// registered rdata is ready by the next cycle, when ack fires.
//
// axil_master (the only master this ever sees) issues either a read or a
// write per transaction, never both at once, so sharing dmem's single
// address port between the two request paths below is safe.

module axil_mem #(
    parameter INIT_FILE = ""
) (
    input  wire        clk,
    input  wire        rst_n,

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
    wire [31:0] reg_wr_addr, reg_wr_data;
    wire [3:0]  reg_wr_strb;

    wire        reg_rd_en;
    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] reg_rd_addr;
    /* verilator lint_on UNUSEDSIGNAL */
    wire [31:0] reg_rd_data;

    // One wait cycle on reads: reg_rd_wait holds off the ack the first
    // cycle reg_rd_en is seen (dmem is only just registering the address
    // that cycle); rd_pending marks that the wait has already happened,
    // so the following cycle acks with dmem's now-valid rdata.
    reg rd_pending;
    always @(posedge clk) begin
        if (!rst_n)                     rd_pending <= 1'b0;
        else if (reg_rd_en && !rd_pending) rd_pending <= 1'b1;
        else                             rd_pending <= 1'b0;
    end
    wire reg_rd_wait = reg_rd_en && !rd_pending;
    wire reg_rd_ack  = reg_rd_en && rd_pending;

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
        .reg_wr_wait (1'b0),
        .reg_wr_ack  (reg_wr_en),

        .reg_rd_addr (reg_rd_addr),
        .reg_rd_en   (reg_rd_en),
        .reg_rd_data (reg_rd_data),
        .reg_rd_wait (reg_rd_wait),
        .reg_rd_ack  (reg_rd_ack)
    );

    dmem #(.INIT_FILE(INIT_FILE)) u_dmem (
        .clk   (clk),
        .en    (reg_rd_en),
        .addr  (reg_rd_en ? reg_rd_addr : reg_wr_addr),
        .wdata (reg_wr_data),
        .wstrb (reg_wr_en ? reg_wr_strb : 4'b0),
        .rdata (reg_rd_data)
    );

endmodule
