`default_nettype none

// AXI4-Lite master for the MEM stage.
//
// Turns one load or store into one or two AXI transactions and holds
// bus_stall until it completes. Also absorbs the byte-lane logic that used
// to live inside dmem: wstrb and the write shift on the way out, the read
// shift and sign extension on the way back. That belongs here now because
// wstrb is an AXI signal -- the slave shouldn't have to know about lb vs
// lbu.
//
// A misaligned half or word can straddle two words (half only when its low
// byte is the top byte of a word; word for any non-zero lane). Rather than
// trap, this splits that access into two word-aligned transactions -- the
// low bytes from word N, the high bytes from word N+1 -- and combines them
// on the way through, so software never sees a fault. riscv-tests' ma_data
// expects exactly this: it issues genuinely misaligned loads/stores and
// checks the value, with no trap handler of its own to catch one.
//
// Note the byte-lane control needs no registered copies of its inputs,
// unlike dmem's lane_q/funct3_q: bus_stall holds EX/MEM, so mem_addr and
// mem_funct3 stay stable for the whole transaction (both phases of it).

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
// Byte lanes -- and whether this access crosses into the next word
// ====================================================================

    wire [1:0] lane  = mem_addr[1:0];
    wire [4:0] shift = {lane, 3'b000};      // lane * 8

    wire is_half = (mem_funct3[1:0] == 2'b01);
    wire is_word = (mem_funct3[1:0] == 2'b10);

    // Half crosses only when its low byte is a word's last byte (lane 3).
    // Word crosses at any nonzero lane -- it never fits in one word
    // unless it starts at lane 0.
    wire crosses = (is_word && (lane != 2'b00)) || (is_half && (lane == 2'b11));

    wire [31:0] addr_lo = {mem_addr[31:2], 2'b00};
    wire [31:0] addr_hi = addr_lo + 32'd4;

    reg [3:0] byte_en;              // which bytes of a word this access
    always @(*) begin               // touches, before shifting into lane
        case (mem_funct3[1:0])
            2'b00:   byte_en = 4'b0001;   // sb
            2'b01:   byte_en = 4'b0011;   // sh
            2'b10:   byte_en = 4'b1111;   // sw
            default: byte_en = 4'b0000;
        endcase
    end

    // Shifting the byte-enable pattern by up to 3 lanes can push it past
    // bit 3 -- those spilled-over bits are exactly the byte enables that
    // belong to word N+1.
    wire [7:0] byte_en_shifted = {4'b0000, byte_en} << lane;
    wire [3:0] wstrb_lo = byte_en_shifted[3:0];
    wire [3:0] wstrb_hi = byte_en_shifted[7:4];

    wire [63:0] wdata_shifted = {32'b0, mem_wdata} << shift;
    wire [31:0] wdata_lo = wdata_shifted[31:0];
    wire [31:0] wdata_hi = wdata_shifted[63:32];

// ====================================================================
// Transaction FSM
// ====================================================================

    localparam [1:0] ST_IDLE  = 2'd0,
                     ST_WRITE = 2'd1,
                     ST_WRESP = 2'd2,
                     ST_READ  = 2'd3;

    reg [1:0]  state;
    reg        aw_done, w_done;
    reg        phase2;        // 0 = word N (or the only word), 1 = word N+1
    reg [31:0] word_lo_reg;   // word N's read data, captured for phase 2

    assign m_axil_awaddr = phase2 ? addr_hi : addr_lo;
    assign m_axil_araddr = phase2 ? addr_hi : addr_lo;
    assign m_axil_awprot = 3'b000;
    assign m_axil_arprot = 3'b000;

    assign m_axil_wstrb = phase2 ? wstrb_hi : wstrb_lo;
    assign m_axil_wdata = phase2 ? wdata_hi : wdata_lo;

    // High from the cycle the request appears until the cycle the final
    // phase's response arrives. On that last cycle mem_rdata is already
    // valid combinationally, so MEM/WB can capture it at the same edge.
    wire phase_done = (state == ST_WRESP && m_axil_bvalid) ||
                      (state == ST_READ  && m_axil_rvalid);
    wire xfer_done  = phase_done && (!crosses || phase2);

    assign bus_stall = (mem_re || mem_we) && !xfer_done;

// ====================================================================
// Read data: combine both phases (word_lo_reg is unused/don't-care
// when !crosses), shift the wanted lane down, then extend
// ====================================================================

    wire [63:0] combined = crosses ? {m_axil_rdata, word_lo_reg}
                                    : {32'b0, m_axil_rdata};
    /* verilator lint_off UNUSEDSIGNAL */
    wire [63:0] shifted64 = combined >> shift;
    /* verilator lint_on UNUSEDSIGNAL */
    wire [31:0] shifted = shifted64[31:0];

    reg [31:0] rdata_ext;
    always @(*) begin
        case (mem_funct3)
            3'b000:  rdata_ext = {{24{shifted[7]}},  shifted[7:0]};    // lb
            3'b001:  rdata_ext = {{16{shifted[15]}}, shifted[15:0]};   // lh
            3'b010:  rdata_ext = shifted;                              // lw
            3'b100:  rdata_ext = {24'b0, shifted[7:0]};                // lbu
            3'b101:  rdata_ext = {16'b0, shifted[15:0]};               // lhu
            default: rdata_ext = shifted;
        endcase
    end

    assign mem_rdata = rdata_ext;

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
            phase2         <= 1'b0;
            word_lo_reg    <= 32'b0;
        end else begin
            case (state)

                ST_IDLE: begin
                    aw_done <= 1'b0;
                    w_done  <= 1'b0;
                    phase2  <= 1'b0;
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
                        if (crosses && !phase2) begin
                            // Second word: same dance again, at addr_hi.
                            phase2         <= 1'b1;
                            aw_done        <= 1'b0;
                            w_done         <= 1'b0;
                            m_axil_bready  <= 1'b0;
                            m_axil_awvalid <= 1'b1;
                            m_axil_wvalid  <= 1'b1;
                            state          <= ST_WRITE;
                        end else begin
                            m_axil_bready <= 1'b0;
                            state         <= ST_IDLE;
                        end
                    end
                end

                ST_READ: begin
                    if (m_axil_arvalid && m_axil_arready)
                        m_axil_arvalid <= 1'b0;
                    if (m_axil_rvalid) begin
                        if (crosses && !phase2) begin
                            word_lo_reg    <= m_axil_rdata;
                            phase2         <= 1'b1;
                            m_axil_arvalid <= 1'b1;
                            // m_axil_rready stays 1 -- still expecting a
                            // response, just for the second word now.
                        end else begin
                            m_axil_rready <= 1'b0;
                            state         <= ST_IDLE;
                        end
                    end
                end

                default: state <= ST_IDLE;

            endcase
        end
    end

endmodule
