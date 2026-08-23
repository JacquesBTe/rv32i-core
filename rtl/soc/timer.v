`default_nettype none

// RISC-V machine timer: a free-running 64-bit mtime and a 64-bit
// mtimecmp, memory-mapped as four 32-bit registers.
//
//   0x00  mtime[31:0]
//   0x04  mtime[63:32]
//   0x08  mtimecmp[31:0]
//   0x0C  mtimecmp[63:32]
//
// timer_irq is high whenever mtime >= mtimecmp (unsigned). Writing either
// half of mtimecmp moves the threshold and so can clear (or re-raise) the
// interrupt on the next cycle. mtimecmp resets to all-ones so no spurious
// interrupt fires before software configures it.
//
// A register file like gpio -- no real latency, so wait ties low and ack
// mirrors en combinationally.

module timer (
    input  wire        clk,
    input  wire        rst_n,

    output wire        timer_irq,

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
    wire [31:0] reg_wr_data;
    /* verilator lint_off UNUSEDSIGNAL */
    wire [3:0]  reg_wr_strb;
    /* verilator lint_on UNUSEDSIGNAL */

    wire        reg_rd_en;
    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] reg_rd_addr;
    /* verilator lint_on UNUSEDSIGNAL */
    reg  [31:0] reg_rd_data;

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
        .reg_rd_wait (1'b0),
        .reg_rd_ack  (reg_rd_en)
    );

    wire wr_mtime_lo    = reg_wr_en && (reg_wr_addr[3:2] == 2'b00);
    wire wr_mtime_hi    = reg_wr_en && (reg_wr_addr[3:2] == 2'b01);
    wire wr_mtimecmp_lo = reg_wr_en && (reg_wr_addr[3:2] == 2'b10);
    wire wr_mtimecmp_hi = reg_wr_en && (reg_wr_addr[3:2] == 2'b11);

    reg [63:0] mtime;
    always @(posedge clk) begin
        if (!rst_n)          mtime <= 64'b0;
        else if (wr_mtime_lo) mtime[31:0]  <= reg_wr_data;
        else if (wr_mtime_hi) mtime[63:32] <= reg_wr_data;
        else                  mtime        <= mtime + 64'd1;
    end

    reg [63:0] mtimecmp;
    always @(posedge clk) begin
        if (!rst_n)               mtimecmp <= {64{1'b1}};
        else if (wr_mtimecmp_lo)  mtimecmp[31:0]  <= reg_wr_data;
        else if (wr_mtimecmp_hi)  mtimecmp[63:32] <= reg_wr_data;
    end

    assign timer_irq = (mtime >= mtimecmp);

    always @(*) begin
        case (reg_rd_addr[3:2])
            2'b00:   reg_rd_data = mtime[31:0];
            2'b01:   reg_rd_data = mtime[63:32];
            2'b10:   reg_rd_data = mtimecmp[31:0];
            2'b11:   reg_rd_data = mtimecmp[63:32];
            default: reg_rd_data = 32'b0;
        endcase
    end

endmodule
