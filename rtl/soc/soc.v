`default_nettype none

// Phase 5 SoC: core + AXI4-Lite bus + dmem + gpio + uart.
//
// Memory map:
//   0x8000_0000  dmem  (64 KB, addr[31] == 1)
//   0x1000_0000  gpio  (4 KB peripheral window)
//   0x1000_1000  uart  (4 KB peripheral window)

module soc #(
    parameter IMEM_INIT = "",
    parameter DMEM_INIT = "",
    parameter RESET_PC  = 32'h8000_0000
) (
    input  wire        clk,
    input  wire        rst_n,

    output wire [15:0] led,
    input  wire [15:0] sw,

    output wire        uart_txd,
    input  wire        uart_rxd,

    // trace outputs -- passthrough from core, for the phase 0 trace_diff
    // harness and tb_soc
    output wire [31:0] trace_pc,
    output wire [31:0] trace_instr,
    output wire [4:0]  trace_rd,
    output wire [31:0] trace_wdata,
    output wire        trace_we,
    output wire [31:0] trace_mem_addr,
    output wire [31:0] trace_mem_wdata,
    output wire        trace_mem_we,
    output wire        trace_trap
);

    wire        rst = !rst_n;

    // ---- core <-> axil_master ----
    wire [31:0] mem_addr, mem_wdata, mem_rdata;
    wire [2:0]  mem_funct3;
    wire        mem_re, mem_we, bus_stall;

    core #(
        .IMEM_INIT (IMEM_INIT),
        .RESET_PC  (RESET_PC)
    ) u_core (
        .clk       (clk),
        .rst_n     (rst_n),
        .bus_stall (bus_stall),

        .mem_addr   (mem_addr),
        .mem_wdata  (mem_wdata),
        .mem_funct3 (mem_funct3),
        .mem_re     (mem_re),
        .mem_we     (mem_we),
        .mem_rdata  (mem_rdata),

        .trace_pc        (trace_pc),
        .trace_instr     (trace_instr),
        .trace_rd        (trace_rd),
        .trace_wdata     (trace_wdata),
        .trace_we        (trace_we),
        .trace_mem_addr  (trace_mem_addr),
        .trace_mem_wdata (trace_mem_wdata),
        .trace_mem_we    (trace_mem_we),
        .trace_trap      (trace_trap)
    );

    // ---- axil_master: MEM stage -> one AXI4-Lite master port ----
    wire [31:0] m_awaddr, m_araddr, m_wdata, m_rdata;
    wire [2:0]  m_awprot, m_arprot;
    wire [3:0]  m_wstrb;
    wire        m_awvalid, m_awready, m_wvalid, m_wready, m_bvalid, m_bready;
    wire [1:0]  m_bresp, m_rresp;
    wire        m_arvalid, m_arready, m_rvalid, m_rready;

    axil_master u_axil_master (
        .clk   (clk),
        .rst_n (rst_n),

        .mem_addr   (mem_addr),
        .mem_wdata  (mem_wdata),
        .mem_funct3 (mem_funct3),
        .mem_re     (mem_re),
        .mem_we     (mem_we),
        .mem_rdata  (mem_rdata),
        .bus_stall  (bus_stall),

        .m_axil_awaddr  (m_awaddr),
        .m_axil_awprot  (m_awprot),
        .m_axil_awvalid (m_awvalid),
        .m_axil_awready (m_awready),
        .m_axil_wdata   (m_wdata),
        .m_axil_wstrb   (m_wstrb),
        .m_axil_wvalid  (m_wvalid),
        .m_axil_wready  (m_wready),
        .m_axil_bresp   (m_bresp),
        .m_axil_bvalid  (m_bvalid),
        .m_axil_bready  (m_bready),
        .m_axil_araddr  (m_araddr),
        .m_axil_arprot  (m_arprot),
        .m_axil_arvalid (m_arvalid),
        .m_axil_arready (m_arready),
        .m_axil_rdata   (m_rdata),
        .m_axil_rresp   (m_rresp),
        .m_axil_rvalid  (m_rvalid),
        .m_axil_rready  (m_rready)
    );

    // ---- interconnect: 1 master -> {dmem, gpio, uart} --------------
    // Index 0 = dmem @ 0x8000_0000, 64 KB window.
    // Index 1 = gpio @ 0x1000_0000, 4 KB window.
    // Index 2 = uart @ 0x1000_1000, 4 KB window.
    localparam [3*32-1:0] IC_BASE_ADDR  =
        {32'h1000_1000, 32'h1000_0000, 32'h8000_0000};
    localparam [3*32-1:0] IC_ADDR_WIDTH =
        {32'd12,        32'd12,        32'd16};

    wire [95:0] ic_m_awaddr, ic_m_araddr, ic_m_wdata, ic_m_rdata;
    wire [8:0]  ic_m_awprot, ic_m_arprot;
    wire [11:0] ic_m_wstrb;
    wire [2:0]  ic_m_awvalid, ic_m_awready, ic_m_wvalid, ic_m_wready;
    wire [2:0]  ic_m_bvalid, ic_m_bready, ic_m_arvalid, ic_m_arready;
    wire [2:0]  ic_m_rvalid, ic_m_rready;
    wire [5:0]  ic_m_bresp, ic_m_rresp;

    axil_interconnect #(
        .S_COUNT(1), .M_COUNT(3),
        .DATA_WIDTH(32), .ADDR_WIDTH(32), .STRB_WIDTH(4),
        .M_BASE_ADDR(IC_BASE_ADDR),
        .M_ADDR_WIDTH(IC_ADDR_WIDTH)
    ) u_interconnect (
        .clk (clk),
        .rst (rst),

        .s_axil_awaddr  (m_awaddr),
        .s_axil_awprot  (m_awprot),
        .s_axil_awvalid (m_awvalid),
        .s_axil_awready (m_awready),
        .s_axil_wdata   (m_wdata),
        .s_axil_wstrb   (m_wstrb),
        .s_axil_wvalid  (m_wvalid),
        .s_axil_wready  (m_wready),
        .s_axil_bresp   (m_bresp),
        .s_axil_bvalid  (m_bvalid),
        .s_axil_bready  (m_bready),
        .s_axil_araddr  (m_araddr),
        .s_axil_arprot  (m_arprot),
        .s_axil_arvalid (m_arvalid),
        .s_axil_arready (m_arready),
        .s_axil_rdata   (m_rdata),
        .s_axil_rresp   (m_rresp),
        .s_axil_rvalid  (m_rvalid),
        .s_axil_rready  (m_rready),

        .m_axil_awaddr  (ic_m_awaddr),
        .m_axil_awprot  (ic_m_awprot),
        .m_axil_awvalid (ic_m_awvalid),
        .m_axil_awready (ic_m_awready),
        .m_axil_wdata   (ic_m_wdata),
        .m_axil_wstrb   (ic_m_wstrb),
        .m_axil_wvalid  (ic_m_wvalid),
        .m_axil_wready  (ic_m_wready),
        .m_axil_bresp   (ic_m_bresp),
        .m_axil_bvalid  (ic_m_bvalid),
        .m_axil_bready  (ic_m_bready),
        .m_axil_araddr  (ic_m_araddr),
        .m_axil_arprot  (ic_m_arprot),
        .m_axil_arvalid (ic_m_arvalid),
        .m_axil_arready (ic_m_arready),
        .m_axil_rdata   (ic_m_rdata),
        .m_axil_rresp   (ic_m_rresp),
        .m_axil_rvalid  (ic_m_rvalid),
        .m_axil_rready  (ic_m_rready)
    );

    // ---- slave 0: dmem, wrapped behind AXI4-Lite -------------------
    axil_mem #(.INIT_FILE(DMEM_INIT)) u_dmem (
        .clk   (clk),
        .rst_n (rst_n),

        .s_axil_awaddr  (ic_m_awaddr[31:0]),
        .s_axil_awprot  (ic_m_awprot[2:0]),
        .s_axil_awvalid (ic_m_awvalid[0]),
        .s_axil_awready (ic_m_awready[0]),
        .s_axil_wdata   (ic_m_wdata[31:0]),
        .s_axil_wstrb   (ic_m_wstrb[3:0]),
        .s_axil_wvalid  (ic_m_wvalid[0]),
        .s_axil_wready  (ic_m_wready[0]),
        .s_axil_bresp   (ic_m_bresp[1:0]),
        .s_axil_bvalid  (ic_m_bvalid[0]),
        .s_axil_bready  (ic_m_bready[0]),
        .s_axil_araddr  (ic_m_araddr[31:0]),
        .s_axil_arprot  (ic_m_arprot[2:0]),
        .s_axil_arvalid (ic_m_arvalid[0]),
        .s_axil_arready (ic_m_arready[0]),
        .s_axil_rdata   (ic_m_rdata[31:0]),
        .s_axil_rresp   (ic_m_rresp[1:0]),
        .s_axil_rvalid  (ic_m_rvalid[0]),
        .s_axil_rready  (ic_m_rready[0])
    );

    // ---- slave 1: gpio ----------------------------------------------
    gpio u_gpio (
        .clk   (clk),
        .rst_n (rst_n),

        .led (led),
        .sw  (sw),

        .s_axil_awaddr  (ic_m_awaddr[63:32]),
        .s_axil_awprot  (ic_m_awprot[5:3]),
        .s_axil_awvalid (ic_m_awvalid[1]),
        .s_axil_awready (ic_m_awready[1]),
        .s_axil_wdata   (ic_m_wdata[63:32]),
        .s_axil_wstrb   (ic_m_wstrb[7:4]),
        .s_axil_wvalid  (ic_m_wvalid[1]),
        .s_axil_wready  (ic_m_wready[1]),
        .s_axil_bresp   (ic_m_bresp[3:2]),
        .s_axil_bvalid  (ic_m_bvalid[1]),
        .s_axil_bready  (ic_m_bready[1]),
        .s_axil_araddr  (ic_m_araddr[63:32]),
        .s_axil_arprot  (ic_m_arprot[5:3]),
        .s_axil_arvalid (ic_m_arvalid[1]),
        .s_axil_arready (ic_m_arready[1]),
        .s_axil_rdata   (ic_m_rdata[63:32]),
        .s_axil_rresp   (ic_m_rresp[3:2]),
        .s_axil_rvalid  (ic_m_rvalid[1]),
        .s_axil_rready  (ic_m_rready[1])
    );

    // ---- slave 2: uart -----------------------------------------------
    uart_axil u_uart (
        .clk   (clk),
        .rst_n (rst_n),

        .txd (uart_txd),
        .rxd (uart_rxd),

        .s_axil_awaddr  (ic_m_awaddr[95:64]),
        .s_axil_awprot  (ic_m_awprot[8:6]),
        .s_axil_awvalid (ic_m_awvalid[2]),
        .s_axil_awready (ic_m_awready[2]),
        .s_axil_wdata   (ic_m_wdata[95:64]),
        .s_axil_wstrb   (ic_m_wstrb[11:8]),
        .s_axil_wvalid  (ic_m_wvalid[2]),
        .s_axil_wready  (ic_m_wready[2]),
        .s_axil_bresp   (ic_m_bresp[5:4]),
        .s_axil_bvalid  (ic_m_bvalid[2]),
        .s_axil_bready  (ic_m_bready[2]),
        .s_axil_araddr  (ic_m_araddr[95:64]),
        .s_axil_arprot  (ic_m_arprot[8:6]),
        .s_axil_arvalid (ic_m_arvalid[2]),
        .s_axil_arready (ic_m_arready[2]),
        .s_axil_rdata   (ic_m_rdata[95:64]),
        .s_axil_rresp   (ic_m_rresp[5:4]),
        .s_axil_rvalid  (ic_m_rvalid[2]),
        .s_axil_rready  (ic_m_rready[2])
    );

endmodule
