`default_nettype none

// Minimal machine-mode CSR file for riscv-tests -p environment.
// Six registers, three access modes, illegal-address detection, and a
// second write path for hardware trap entry.
//
// NOT implemented (phase 5): mie/mip, interrupt delegation, counters,
// privilege modes, CSR write-permission checks.

module csr (
    input  wire        clk,
    input  wire        rst_n,

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

    output wire [31:0] mtvec_out,
    output wire [31:0] mepc_out
);

    localparam [11:0] CSR_MSTATUS  = 12'h300;
    localparam [11:0] CSR_MTVEC    = 12'h305;
    localparam [11:0] CSR_MSCRATCH = 12'h340;
    localparam [11:0] CSR_MEPC     = 12'h341;
    localparam [11:0] CSR_MCAUSE   = 12'h342;
    localparam [11:0] CSR_MHARTID  = 12'hF14;

    reg [31:0] mstatus, mtvec, mscratch, mepc, mcause;

    assign mtvec_out = mtvec;
    assign mepc_out  = mepc;

    // Recognised but not implemented: accept writes, read as zero.
    // Legal WARL behaviour, and prevents spurious traps in riscv-tests init.
    wire csr_ignored = (csr_addr == 12'h301)   // misa
                    || (csr_addr == 12'h302)   // medeleg
                    || (csr_addr == 12'h303)   // mideleg
                    || (csr_addr == 12'h304)   // mie
                    || (csr_addr == 12'h343)   // mtval
                    || (csr_addr == 12'h344)   // mip
                    || (csr_addr == 12'h180)   // satp
                    || (csr_addr == 12'h3A0)   // pmpcfg0
                    || (csr_addr == 12'h3B0);  // pmpaddr0

    // ---- read -------------------------------------------------------
    always @(*) begin
        csr_illegal = 1'b0;
        case (csr_addr)
            CSR_MSTATUS:  csr_rdata = mstatus;
            CSR_MTVEC:    csr_rdata = mtvec;
            CSR_MSCRATCH: csr_rdata = mscratch;
            CSR_MEPC:     csr_rdata = mepc;
            CSR_MCAUSE:   csr_rdata = mcause;
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
            mstatus  <= 32'b0;
            mtvec    <= 32'b0;
            mscratch <= 32'b0;
            mepc     <= 32'b0;
            mcause   <= 32'b0;
        end else begin
            // Trap entry wins over any instruction-side write, since the
            // faulting instruction must not commit its own CSR update.
            if (trap) begin
                mepc   <= trap_pc;
                mcause <= trap_cause;
            end else if (do_write) begin
                case (csr_addr)
                    CSR_MSTATUS:  mstatus  <= csr_new;
                    CSR_MTVEC:    mtvec    <= csr_new;
                    CSR_MSCRATCH: mscratch <= csr_new;
                    CSR_MEPC:     mepc     <= csr_new;
                    CSR_MCAUSE:   mcause   <= csr_new;
                    default: ;
                endcase
            end
        end
    end

endmodule
