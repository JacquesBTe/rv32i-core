`default_nettype none

// GPIO: two 32-bit registers behind axil_reg_if.
//   0x00  LED output   -- write; reads back last written value
//   0x04  switch input -- read only
//
// A register file, not a memory with any latency -- wait is tied low and
// ack asserted combinationally with en, so every transaction completes in
// the minimum number of cycles axil_reg_if allows.

module gpio (
    input  wire        clk,
    input  wire        rst_n,

    output wire [15:0] led,
    input  wire [15:0] sw,

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
    wire [3:0]  reg_wr_strb;

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

    // ---- LED register ----
    reg [31:0] led_reg;

    always @(posedge clk) begin
        if (!rst_n) begin
            led_reg <= 32'b0;
        end else if (reg_wr_en && reg_wr_addr[2] == 1'b0) begin
            if (reg_wr_strb[0]) led_reg[7:0]   <= reg_wr_data[7:0];
            if (reg_wr_strb[1]) led_reg[15:8]  <= reg_wr_data[15:8];
            if (reg_wr_strb[2]) led_reg[23:16] <= reg_wr_data[23:16];
            if (reg_wr_strb[3]) led_reg[31:24] <= reg_wr_data[31:24];
        end
    end

    assign led = led_reg[15:0];

    // ---- read mux ----
    always @(*) begin
        case (reg_rd_addr[2])
            1'b0:    reg_rd_data = led_reg;
            1'b1:    reg_rd_data = {16'b0, sw};
            default: reg_rd_data = 32'b0;
        endcase
    end

endmodule
