`default_nettype none
`include "rv32i_defs.vh"

module core #(
    parameter IMEM_INIT = "rv32ui-p-add.hex",
    parameter DMEM_INIT = "",
    parameter RESET_PC  = 32'h80000000
) (
    input  wire        clk,
    input  wire        rst_n,

    // trace outputs -- for the phase 0 trace_diff harness
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

// ====================================================================
// DECLARATIONS -- grouped by stage
// ====================================================================

    // ---- IF ------------------------------------------------------
    reg  [31:0] pc;
    wire [31:0] pc_plus4 = pc + 32'd4;
    wire [31:0] pc_target;
    wire [31:0] pc_next;
    wire        pc_redirect;
    wire [31:0] instr;

    // ---- IF/ID ---------------------------------------------------
    reg  [31:0] if_id_pc;
    reg  [31:0] if_id_instr;

    // ---- ID ------------------------------------------------------
    wire [4:0]  rs1_addr, rs2_addr, rd_addr;
    wire [2:0]  funct3;
    wire [3:0]  alu_op;
    wire        alu_src, alu_a_pc, alu_a_zero;
    wire        reg_we, mem_we;
    wire        mem_re;
    wire [1:0]  wb_sel;
    wire        is_branch, is_jal, is_jalr;
    wire        illegal;
    wire [31:0] rs1_data, rs2_data;
    wire [31:0] imm;

    wire [1:0]  csr_op;
    wire        csr_re, csr_we, csr_use_imm;
    wire        is_mret, is_ecall;

    // ---- ID/EX ---------------------------------------------------
    reg  [31:0] id_ex_pc, id_ex_pc_plus4;
    reg  [31:0] id_ex_rs1_data, id_ex_rs2_data, id_ex_imm;
    reg  [4:0]  id_ex_rs1_addr, id_ex_rs2_addr;  
        /* verilator lint_off UNUSEDSIGNAL */
    reg         id_ex_mem_re;  
        /* verilator lint_on UNUSEDSIGNAL */                 
    reg  [4:0]  id_ex_rd_addr;
    reg  [2:0]  id_ex_funct3;
    reg  [3:0]  id_ex_alu_op;
    reg         id_ex_alu_src, id_ex_alu_a_pc, id_ex_alu_a_zero;
    reg         id_ex_reg_we, id_ex_mem_we;
    reg  [1:0]  id_ex_wb_sel;
    reg         id_ex_is_branch, id_ex_is_jal, id_ex_is_jalr;
    reg  [31:0] id_ex_instr;
    reg         id_ex_illegal, id_ex_is_mret, id_ex_is_ecall;
    reg  [1:0]  id_ex_csr_op;
    reg         id_ex_csr_re, id_ex_csr_we, id_ex_csr_use_imm;

    // ---- EX ------------------------------------------------------
    wire [31:0] alu_a, alu_b, alu_result;
    wire        branch_taken;
    wire        trap;
    wire [31:0] trap_cause;
    wire [31:0] csr_rdata;
    wire        csr_illegal;
    wire [31:0] mtvec_out, mepc_out;

    // ---- EX/MEM --------------------------------------------------
    reg  [31:0] ex_mem_alu_result, ex_mem_rs2_data, ex_mem_pc_plus4;
    reg  [31:0] ex_mem_csr_rdata;
    reg  [4:0]  ex_mem_rd_addr;
    reg  [2:0]  ex_mem_funct3;
    reg         ex_mem_reg_we, ex_mem_mem_we;
    reg  [1:0]  ex_mem_wb_sel;
    reg  [31:0] ex_mem_pc, ex_mem_instr;

    // ---- MEM -----------------------------------------------------
    wire [31:0] mem_rdata;

    // ---- MEM/WB --------------------------------------------------
    reg  [31:0] mem_wb_alu_result, mem_wb_mem_rdata, mem_wb_pc_plus4;
    reg  [31:0] mem_wb_csr_rdata;
    reg  [4:0]  mem_wb_rd_addr;
    reg         mem_wb_reg_we;
    reg  [1:0]  mem_wb_wb_sel;
    reg  [31:0] mem_wb_pc, mem_wb_instr;

    // ---- WB ------------------------------------------------------
    wire [31:0] wb_data;


// ====================================================================
// IF -- instruction fetch
// ====================================================================

    imem #(.INIT_FILE(IMEM_INIT)) u_imem (
        .addr   (pc),
        .instr  (instr)
    );

    assign pc_next = pc_redirect ? pc_target : pc_plus4;

    always @(posedge clk) begin
        if (!rst_n) pc <= RESET_PC;
        else if (load_use) pc <= pc; //hold
        else        pc <= pc_next;
    end

    wire flush = pc_redirect;

    // ---- IF/ID register ----
    always @(posedge clk) begin
        if (!rst_n) begin
            if_id_pc    <= 32'b0;
            if_id_instr <= 32'h00000013; 
        end else if (flush) begin
            if_id_pc    <= pc;
            if_id_instr <= 32'h00000013;   // addi x0,x0,0 -- a real nop
        end else if (load_use)begin
            if_id_pc    <= if_id_pc; // hold
            if_id_instr <= if_id_instr;
        end else begin
            if_id_pc    <= pc;
            if_id_instr <= instr;
        end
    end


// ====================================================================
// ID -- decode and register read
// ====================================================================

    decoder u_decoder (
        .instr       (if_id_instr),
        .rs1_addr    (rs1_addr),
        .rs2_addr    (rs2_addr),
        .rd_addr     (rd_addr),
        .funct3      (funct3),
        .alu_op      (alu_op),
        .alu_src     (alu_src),
        .alu_a_pc    (alu_a_pc),
        .alu_a_zero  (alu_a_zero),
        .reg_we      (reg_we),
        .mem_we      (mem_we),
        .mem_re      (mem_re),
        .wb_sel      (wb_sel),
        .is_branch   (is_branch),
        .is_jal      (is_jal),
        .is_jalr     (is_jalr),
        .illegal     (illegal),
        .csr_op      (csr_op),
        .csr_re      (csr_re),
        .csr_we      (csr_we),
        .csr_use_imm (csr_use_imm),
        .is_mret     (is_mret),
        .is_ecall    (is_ecall)
    );

    immgen u_immgen (
        .instr  (if_id_instr),
        .imm    (imm)
    );

    // Read ports are ID; the write port belongs to WB.
    regfile #(.BYPASS(1)) u_regfile (  // TODO phase 4: BYPASS(1)
        .clk        (clk),
        .rs1_addr   (rs1_addr),
        .rs1_data   (rs1_data),
        .rs2_addr   (rs2_addr),
        .rs2_data   (rs2_data),
        .rd_addr    (mem_wb_rd_addr),
        .rd_data    (wb_data),
        .rd_we      (mem_wb_reg_we)
    );

    wire load_use = id_ex_mem_re
                && (id_ex_rd_addr != 5'b0)
                && ((id_ex_rd_addr == rs1_addr) || (id_ex_rd_addr == rs2_addr));

    // ---- ID/EX register ----
    always @(posedge clk) begin
        if (!rst_n) begin
            id_ex_reg_we    <= 1'b0;
            id_ex_mem_we    <= 1'b0;
            id_ex_is_branch <= 1'b0;
            id_ex_is_jal    <= 1'b0;
            id_ex_is_jalr   <= 1'b0;
            id_ex_illegal   <= 1'b0;
            id_ex_is_mret   <= 1'b0;
            id_ex_is_ecall  <= 1'b0;
            id_ex_csr_re    <= 1'b0;
            id_ex_csr_we    <= 1'b0;
         end else if (flush || load_use) begin
            id_ex_pc          <= 32'b0;
            id_ex_pc_plus4    <= 32'b0;
            id_ex_instr       <= 32'h00000013;
            id_ex_rs1_addr    <= 5'b0;
            id_ex_rs2_addr    <= 5'b0;
            id_ex_rd_addr     <= 5'b0;
            id_ex_wb_sel      <= 2'b0;
            id_ex_reg_we      <= 1'b0;
            id_ex_mem_we      <= 1'b0;
            id_ex_is_branch   <= 1'b0;
            id_ex_is_jal      <= 1'b0;
            id_ex_is_jalr     <= 1'b0;
            id_ex_illegal     <= 1'b0;
            id_ex_is_mret     <= 1'b0;
            id_ex_is_ecall    <= 1'b0;
            id_ex_csr_re      <= 1'b0;
            id_ex_csr_we      <= 1'b0;
        end else begin
            id_ex_pc          <= if_id_pc;
            id_ex_pc_plus4    <= if_id_pc + 32'd4;
            id_ex_instr       <= if_id_instr;

            id_ex_rs1_data    <= rs1_data;
            id_ex_rs2_data    <= rs2_data;
            id_ex_imm         <= imm;

            id_ex_rs1_addr    <= rs1_addr;
            id_ex_rs2_addr    <= rs2_addr;
            id_ex_rd_addr     <= rd_addr;
            id_ex_funct3      <= funct3;

            id_ex_alu_op      <= alu_op;
            id_ex_alu_src     <= alu_src;
            id_ex_alu_a_pc    <= alu_a_pc;
            id_ex_alu_a_zero  <= alu_a_zero;

            id_ex_mem_re      <= mem_re;
            id_ex_wb_sel      <= wb_sel;

            id_ex_reg_we      <= reg_we;
            id_ex_mem_we      <= mem_we;
            id_ex_is_branch   <= is_branch;
            id_ex_is_jal      <= is_jal;
            id_ex_is_jalr     <= is_jalr;

            id_ex_illegal     <= illegal;
            id_ex_is_mret     <= is_mret;
            id_ex_is_ecall    <= is_ecall;
            id_ex_csr_op      <= csr_op;
            id_ex_csr_re      <= csr_re;
            id_ex_csr_we      <= csr_we;
            id_ex_csr_use_imm <= csr_use_imm;
        end
    end


// ====================================================================
// EX -- execute
// ====================================================================


    //Forwarding
    // Three terms, same shape as the regfile bypass: the source stage
    // writes a register, its rd matches the reader's rs, and it isn't x0.
    
    //forward operand a, from MEM
    wire fwd_a_mem = ex_mem_reg_we && (ex_mem_rd_addr == id_ex_rs1_addr) //the instr in MEM is going to write a reg 
                                   && (id_ex_rs1_addr != 5'b0); //f it's a store or a branch, it writes nothing, so there's nothing to forward and this must not fire
   
    wire fwd_a_wb  = mem_wb_reg_we && (mem_wb_rd_addr == id_ex_rs1_addr)
                                   && (id_ex_rs1_addr != 5'b0);

    wire fwd_b_mem = ex_mem_reg_we && (ex_mem_rd_addr == id_ex_rs2_addr)
                                   && (id_ex_rs2_addr != 5'b0);
    wire fwd_b_wb  = mem_wb_reg_we && (mem_wb_rd_addr == id_ex_rs2_addr)
                                   && (id_ex_rs2_addr != 5'b0);

    // Priority: MEM before WB. If both match, MEM holds the more recent
    // write and is the one the program means.
    wire [31:0] fwd_rs1 = fwd_a_mem ? ex_mem_wb_value :
                          fwd_a_wb  ? wb_data         : id_ex_rs1_data;

    wire [31:0] fwd_rs2 = fwd_b_mem ? ex_mem_wb_value :
                          fwd_b_wb  ? wb_data         : id_ex_rs2_data;

    assign alu_a = id_ex_alu_a_zero ? 32'b0 :
                   id_ex_alu_a_pc   ? id_ex_pc : fwd_rs1;
    assign alu_b = id_ex_alu_src ? id_ex_imm : fwd_rs2;

    alu u_alu (
        .a      (alu_a),
        .b      (alu_b),
        .alu_op (id_ex_alu_op),
        .result (alu_result)
    );

    branch_cmp u_branch_cmp (
        .a      (fwd_rs1),
        .b      (fwd_rs2),
        .funct3 (id_ex_funct3),
        .taken  (branch_taken)
    );

    // Branch and jump resolution -- feeds back to IF.
    // Priority: trap outranks everything, so a faulting jalr goes to mtvec
    // rather than to its own computed target.
    assign pc_redirect = trap || id_ex_is_mret || id_ex_is_jal || id_ex_is_jalr
                         || (id_ex_is_branch && branch_taken);
    assign pc_target = trap          ? mtvec_out :
                       id_ex_is_mret ? mepc_out  :
                       id_ex_is_jalr ? (alu_result & ~32'd1)
                                     : (id_ex_pc + id_ex_imm);

    // ---- CSR ----
    // For the immediate forms the rs1 field is a 5-bit uimm, not a register.
    wire [31:0] csr_wdata = id_ex_csr_use_imm ? {27'b0, id_ex_rs1_addr}
                                              : fwd_rs1;

    assign trap       = id_ex_illegal || csr_illegal || id_ex_is_ecall;
    assign trap_cause = id_ex_is_ecall ? 32'd11 : 32'd2;

    // A faulting instruction must have no side effects.
    wire reg_we_final = id_ex_reg_we && !trap;
    wire mem_we_final = id_ex_mem_we && !trap;

    csr u_csr (
        .clk         (clk),
        .rst_n       (rst_n),
        .csr_addr    (id_ex_instr[31:20]),
        .csr_wdata   (csr_wdata),
        .csr_op      (id_ex_csr_op),
        .csr_re      (id_ex_csr_re),
        .csr_we      (id_ex_csr_we),
        .csr_rdata   (csr_rdata),
        .csr_illegal (csr_illegal),
        .trap        (trap),
        .trap_pc     (id_ex_pc),
        .trap_cause  (trap_cause),
        .mtvec_out   (mtvec_out),
        .mepc_out    (mepc_out)
    );

    //forwarding mux
    wire [31:0] ex_mem_wb_value = (ex_mem_wb_sel == 2'b11) ? ex_mem_csr_rdata : //if CSR read, CSR data
                                  (ex_mem_wb_sel == 2'b10) ? ex_mem_pc_plus4  : //if jal or jalr, return address PC +4
                                                             ex_mem_alu_result; //else ALU result

                                                            
    // ---- EX/MEM register ----
    always @(posedge clk) begin
        if (!rst_n) begin
            ex_mem_reg_we <= 1'b0;
            ex_mem_mem_we <= 1'b0;
        end else begin
            ex_mem_reg_we     <= reg_we_final;
            ex_mem_mem_we     <= mem_we_final;

            ex_mem_alu_result <= alu_result;
            ex_mem_rs2_data   <= fwd_rs2;
            ex_mem_pc_plus4   <= id_ex_pc_plus4;
            ex_mem_csr_rdata  <= csr_rdata;

            ex_mem_rd_addr    <= id_ex_rd_addr;
            ex_mem_funct3     <= id_ex_funct3;
            ex_mem_wb_sel     <= id_ex_wb_sel;

            ex_mem_pc         <= id_ex_pc;
            ex_mem_instr      <= id_ex_instr;
        end
    end


// ====================================================================
// MEM -- data memory
// ====================================================================

    dmem #(.INIT_FILE(DMEM_INIT)) u_dmem (
        .clk    (clk),
        .addr   (ex_mem_alu_result),
        .wdata  (ex_mem_rs2_data),
        .funct3 (ex_mem_funct3),
        .we     (ex_mem_mem_we),
        .rdata  (mem_rdata)
    );

    // ---- MEM/WB register ----
    always @(posedge clk) begin
        if (!rst_n) begin
            mem_wb_reg_we <= 1'b0;
        end else begin
            mem_wb_reg_we     <= ex_mem_reg_we;

            mem_wb_alu_result <= ex_mem_alu_result;
            mem_wb_mem_rdata  <= mem_rdata;
            mem_wb_pc_plus4   <= ex_mem_pc_plus4;
            mem_wb_csr_rdata  <= ex_mem_csr_rdata;

            mem_wb_rd_addr    <= ex_mem_rd_addr;
            mem_wb_wb_sel     <= ex_mem_wb_sel;

            mem_wb_pc         <= ex_mem_pc;
            mem_wb_instr      <= ex_mem_instr;
        end
    end


// ====================================================================
// WB -- writeback
// ====================================================================

    assign wb_data = (mem_wb_wb_sel == 2'b11) ? mem_wb_csr_rdata :
                     (mem_wb_wb_sel == 2'b10) ? mem_wb_pc_plus4  :
                     (mem_wb_wb_sel == 2'b01) ? mem_wb_mem_rdata
                                              : mem_wb_alu_result;


// ====================================================================
// Trace -- reports the retiring instruction (WB)
// ====================================================================

    assign trace_pc        = mem_wb_pc;
    assign trace_instr     = mem_wb_instr;
    assign trace_rd        = mem_wb_rd_addr;
    assign trace_wdata     = wb_data;
    assign trace_we        = mem_wb_reg_we && (mem_wb_rd_addr != 5'b0);

    assign trace_mem_addr  = ex_mem_alu_result;
    assign trace_mem_wdata = ex_mem_rs2_data;
    assign trace_mem_we    = ex_mem_mem_we;

    assign trace_trap      = trap;

endmodule
