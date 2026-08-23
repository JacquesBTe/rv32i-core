`default_nettype none

// AXI4-Lite master for the MEM stage.
//
// Turns one load or store into one AXI transaction and holds bus_stall
// until it completes. Also absorbs the byte-lane logic that used to live
// inside dmem: wstrb and the write shift on the way out, the read shift
// and sign extension on the way back. That belongs here now because wstrb
// is an AXI signal -- the slave shouldn't have to know about lb vs lbu.
//
// Note the byte-lane control needs no registered copies, unlike dmem's
// lane_q/funct3_q: bus_stall holds EX/MEM, so mem_addr and mem_funct3
// stay stable for the whole transaction.

module axil_master (
    input  wire        clk,
    input  wire        rst_n,

    // ---- MEM stage ----
    input  wire [31:0] mem_addr,
    input  wire [31:0] mem_wdata,
    input  wire [2:0]  mem_funct3,
    input  wire        mem_re,
    input  wire        mem_we,
    output wire [31:0] mem_rdata,
    output wire        bus_stall,

    // ---- AXI4-Lite master ----
    output wire [31:0] m_axil_awaddr,
    output wire [2:0]  m_axil_awprot,
    output reg         m_axil_awvalid,
    input  wire        m_axil_awready,

    output wire [31:0] m_axil_wdata,
    output wire [3:0]  m_axil_wstrb,
    output reg         m_axil_wvalid,
    input  wire        m_axil_wready,

    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [1:0]  m_axil_bresp,     // TODO: trap on SLVERR/DECERR
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire        m_axil_bvalid,
    output reg         m_axil_bready,

    output wire [31:0] m_axil_araddr,
    output wire [2:0]  m_axil_arprot,
    output reg         m_axil_arvalid,
    input  wire        m_axil_arready,

    input  wire [31:0] m_axil_rdata,
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [1:0]  m_axil_rresp,     // TODO: trap on SLVERR/DECERR
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire        m_axil_rvalid,
    output reg         m_axil_rready
);

// ====================================================================
// Byte lanes
// ====================================================================

    wire [1:0] lane  = mem_addr[1:0];
    wire [4:0] shift = {lane, 3'b000};      // lane * 8

    // Address is word-aligned; wstrb selects the bytes within the word.
    assign m_axil_awaddr = {mem_addr[31:2], 2'b00};
    assign m_axil_araddr = {mem_addr[31:2], 2'b00};
    assign m_axil_awprot = 3'b000;
    assign m_axil_arprot = 3'b000;

    reg [3:0] wstrb;
    always @(*) begin
        case (mem_funct3[1:0])
            2'b00:   wstrb = 4'b0001 << lane;   // sb
            2'b01:   wstrb = 4'b0011 << lane;   // sh
            2'b10:   wstrb = 4'b1111;           // sw
            default: wstrb = 4'b0000;
        endcase
    end

    assign m_axil_wstrb = wstrb;
    assign m_axil_wdata = mem_wdata << shift;

// ====================================================================
// Read data: shift the wanted lane down, then extend
// ====================================================================

    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] shifted = m_axil_rdata >> shift;
    /* verilator lint_on UNUSEDSIGNAL */

    reg [31:0] rdata_ext;
    always @(*) begin
        case (mem_funct3)
            3'b000:  rdata_ext = {{24{shifted[7]}},  shifted[7:0]};    // lb
            3'b001:  rdata_ext = {{16{shifted[15]}}, shifted[15:0]};   // lh
            3'b010:  rdata_ext = m_axil_rdata;                         // lw
            3'b100:  rdata_ext = {24'b0, shifted[7:0]};                // lbu
            3'b101:  rdata_ext = {16'b0, shifted[15:0]};               // lhu
            default: rdata_ext = m_axil_rdata;
        endcase
    end

    assign mem_rdata = rdata_ext;

// ====================================================================
// Transaction FSM
// ====================================================================

    localparam [1:0] ST_IDLE  = 2'd0,
                     ST_WRITE = 2'd1,
                     ST_WRESP = 2'd2,
                     ST_READ  = 2'd3;

    reg [1:0] state;
    reg       aw_done, w_done;

    // High from the cycle the request appears until the cycle the
    // response arrives. On that last cycle mem_rdata is already valid
    // combinationally, so MEM/WB can capture it at the same edge.
    wire xfer_done = (state == ST_WRESP && m_axil_bvalid) ||
                     (state == ST_READ  && m_axil_rvalid);

    assign bus_stall = (mem_re || mem_we) && !xfer_done;

    always @(posedge clk) begin
        if (!rst_n) begin
            state          <= ST_IDLE;
            m_axil_awvalid <= 1'b0;
            m_axil_wvalid  <= 1'b0;
            m_axil_bready  <= 1'b0;
            m_axil_arvalid <= 1'b0;
            m_axil_rready  <= 1'b0;
            aw_done        <= 1'b0;
            w_done         <= 1'b0;
        end else begin
            case (state)

                ST_IDLE: begin
                    aw_done <= 1'b0;
                    w_done  <= 1'b0;
                    if (mem_we) begin
                        m_axil_awvalid <= 1'b1;
                        m_axil_wvalid  <= 1'b1;
                        state          <= ST_WRITE;
                    end else if (mem_re) begin
                        m_axil_arvalid <= 1'b1;
                        m_axil_rready  <= 1'b1;
                        state          <= ST_READ;
                    end
                end

                // AW and W are independent channels and may be accepted
                // on different cycles, so each is tracked separately.
                ST_WRITE: begin
                    if (m_axil_awvalid && m_axil_awready) begin
                        m_axil_awvalid <= 1'b0;
                        aw_done        <= 1'b1;
                    end
                    if (m_axil_wvalid && m_axil_wready) begin
                        m_axil_wvalid <= 1'b0;
                        w_done        <= 1'b1;
                    end
                    if ((aw_done || (m_axil_awvalid && m_axil_awready)) &&
                        (w_done  || (m_axil_wvalid  && m_axil_wready))) begin
                        m_axil_bready <= 1'b1;
                        state         <= ST_WRESP;
                    end
                end

                ST_WRESP: begin
                    if (m_axil_bvalid) begin
                        m_axil_bready <= 1'b0;
                        state         <= ST_IDLE;
                    end
                end

                ST_READ: begin
                    if (m_axil_arvalid && m_axil_arready)
                        m_axil_arvalid <= 1'b0;
                    if (m_axil_rvalid) begin
                        m_axil_rready <= 1'b0;
                        state         <= ST_IDLE;
                    end
                end

                default: state <= ST_IDLE;

            endcase
        end
    end

endmodule
