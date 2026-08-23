`default_nettype none

// Machine-mode CSR file for riscv-tests -p environment, plus enough of
// mie/mip/mstatus/privilege-mode to support a real timer interrupt.
//
// NOT implemented: mie/mip bits other than MTIE/MTIP, interrupt
// delegation, counters, supervisor mode, CSR write-permission checks.

module csr (
    input  wire        clk,
    input  wire        rst_n,

    // A bus transaction may still be in flight for an instruction behind
    // this one in EX; while bus_stall holds ID/EX, this module's inputs
    // are the same frozen values every cycle. mepc/mcause capture is
    // naturally idempotent under that repetition, but the mstatus/
    // privilege swaps mret and trap entry perform are not -- toggling
    // MIE<->MPIE twice is not the same as once. Gate everything on it.
    input  wire        bus_stall,

    // instruction-side access
    input  wire [11:0] csr_addr,
    input  wire [31:0] csr_wdata,    // rs1_data, or zero-extended uimm
    input  wire [1:0]  csr_op,       // 00 = none, 01 = rw, 10 = rs, 11 = rc
    input  wire        csr_re,       // this instruction reads a CSR
    input  wire        csr_we,       // this instruction writes a CSR
    output reg  [31:0] csr_rdata,
    output reg         csr_illegal,  // unimplemented address

    // hardware trap entry -- bypasses the instruction path
    input  wire        trap,
    input  wire [31:0] trap_pc,
    input  wire [31:0] trap_cause,

    // mret retirement -- also bypasses the instruction path
    input  wire        is_mret,

    // timer interrupt line from the SoC, wired straight into mip.MTIP
    input  wire        timer_irq,

    output wire [31:0] mtvec_out,
    output wire [31:0] mepc_out,
    output wire        priv_m_out    // 1 = M-mode, 0 = U-mode
);

    localparam [11:0] CSR_MSTATUS  = 12'h300;
    localparam [11:0] CSR_MIE      = 12'h304;
    localparam [11:0] CSR_MTVEC    = 12'h305;
    localparam [11:0] CSR_MSCRATCH = 12'h340;
    localparam [11:0] CSR_MEPC     = 12'h341;
    localparam [11:0] CSR_MCAUSE   = 12'h342;
    localparam [11:0] CSR_MIP      = 12'h344;
    localparam [11:0] CSR_MHARTID  = 12'hF14;

    reg        mstatus_mie, mstatus_mpie;
    reg  [1:0] mstatus_mpp;
    reg        mie_mtie;
    reg        priv_m;               // 1 = M-mode, 0 = U-mode
    reg [31:0] mtvec, mscratch, mepc, mcause;

    assign mtvec_out   = mtvec;
    assign mepc_out    = mepc;
    assign priv_m_out  = priv_m;

    wire [31:0] mstatus_rdata = {19'b0, mstatus_mpp, 3'b0, mstatus_mpie,
                                  3'b0, mstatus_mie, 3'b0};
    wire [31:0] mie_rdata     = {24'b0, mie_mtie, 7'b0};
    wire [31:0] mip_rdata     = {24'b0, timer_irq, 7'b0};

    // Recognised but not implemented: accept writes, read as zero.
    // Legal WARL behaviour, and prevents spurious traps in riscv-tests init.
    wire csr_ignored = (csr_addr == 12'h301)   // misa
                    || (csr_addr == 12'h302)   // medeleg
                    || (csr_addr == 12'h303)   // mideleg
                    || (csr_addr == 12'h343)   // mtval
                    || (csr_addr == 12'h180)   // satp
                    || (csr_addr == 12'h3A0)   // pmpcfg0
                    || (csr_addr == 12'h3B0);  // pmpaddr0

    // ---- read -------------------------------------------------------
    always @(*) begin
        csr_illegal = 1'b0;
        case (csr_addr)
            CSR_MSTATUS:  csr_rdata = mstatus_rdata;
            CSR_MIE:      csr_rdata = mie_rdata;
            CSR_MTVEC:    csr_rdata = mtvec;
            CSR_MSCRATCH: csr_rdata = mscratch;
            CSR_MEPC:     csr_rdata = mepc;
            CSR_MCAUSE:   csr_rdata = mcause;
            CSR_MIP:      csr_rdata = mip_rdata;
            CSR_MHARTID:  csr_rdata = 32'b0;      // single hart
            default: begin
                csr_rdata   = 32'b0;
                csr_illegal = (csr_re || csr_we) && !csr_ignored;
            end
        endcase
    end

    // ---- write value ------------------------------------------------
    reg [31:0] csr_new;
    always @(*) begin
        case (csr_op)
            2'b01:   csr_new = csr_wdata;                // csrrw
            2'b10:   csr_new = csr_rdata |  csr_wdata;   // csrrs: set
            2'b11:   csr_new = csr_rdata & ~csr_wdata;   // csrrc: clear
            default: csr_new = csr_rdata;
        endcase
    end

    // mhartid is read-only; writes to it are dropped rather than trapping.
    wire do_write = csr_we && !csr_illegal && (csr_addr != CSR_MHARTID);

    // ---- state ------------------------------------------------------
    always @(posedge clk) begin
        if (!rst_n) begin
            mstatus_mie  <= 1'b0;
            mstatus_mpie <= 1'b0;
            mstatus_mpp  <= 2'b00;    // MPP unused until the first trap
            mie_mtie     <= 1'b0;
            priv_m       <= 1'b1;     // reset in M-mode
            mtvec        <= 32'b0;
            mscratch     <= 32'b0;
            mepc         <= 32'b0;
            mcause       <= 32'b0;
        end else if (bus_stall) begin
            // hold -- this instruction hasn't actually retired yet
        end else if (trap) begin
            // Trap entry wins over any instruction-side write, since the
            // faulting instruction must not commit its own CSR update.
            mepc         <= trap_pc;
            mcause       <= trap_cause;
            mstatus_mpie <= mstatus_mie;
            mstatus_mie  <= 1'b0;
            mstatus_mpp  <= {2{priv_m}};   // 2'b11 (M) or 2'b00 (U)
            priv_m       <= 1'b1;          // all traps land in M-mode here
        end else if (is_mret) begin
            priv_m       <= mstatus_mpp[1];   // 2'b11->M, 2'b00->U
            mstatus_mie  <= mstatus_mpie;
            mstatus_mpie <= 1'b1;
            mstatus_mpp  <= 2'b00;            // least-privileged mode (U)
        end else if (do_write) begin
            case (csr_addr)
                CSR_MSTATUS: begin
                    mstatus_mie  <= csr_new[3];
                    mstatus_mpie <= csr_new[7];
                    mstatus_mpp  <= csr_new[12:11];
                end
                CSR_MIE:      mie_mtie  <= csr_new[7];
                CSR_MTVEC:    mtvec     <= csr_new;
                CSR_MSCRATCH: mscratch  <= csr_new;
                CSR_MEPC:     mepc      <= csr_new;
                CSR_MCAUSE:   mcause    <= csr_new;
                // CSR_MIP: MTIP is wired straight from the timer and
                // can't be set by a CSR write; silently drop it.
                default: ;
            endcase
        end
    end

endmodule
