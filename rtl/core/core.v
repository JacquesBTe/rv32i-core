`default_nettype none
`include "rv32i_defs.vh"

module core #(
    parameter IMEM_INIT = "core_test.hex",
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
    output wire        trace_we
);

    // ---- PC ------------------------------------------------------
    reg  [31:0] pc;
    wire [31:0] pc_plus4  = pc + 32'd4;
    wire [31:0] pc_target;        // branch/jump destination
    wire [31:0] pc_next;

    // ---- instruction fetch ---------------------------------------
    wire [31:0] instr;

    // ---- decode --------------------------------------------------
    wire [4:0]  rs1_addr, rs2_addr, rd_addr;
    wire [2:0]  funct3;
    wire [3:0]  alu_op;
    wire        alu_src, alu_a_pc, alu_a_zero;
    wire        reg_we, mem_we; 
    /* verilator lint_off UNUSEDSIGNAL */
    wire mem_re;
    /* verilator lint_on UNUSEDSIGNAL */
    wire [1:0]  wb_sel;
    wire        is_branch, is_jal, is_jalr;
    /* verilator lint_off UNUSEDSIGNAL */
    wire        illegal;
    /* verilator lint_on UNUSEDSIGNAL */

    // ---- datapath ------------------------------------------------
    wire [31:0] rs1_data, rs2_data;
    wire [31:0] imm;
    wire [31:0] alu_a, alu_b, alu_result;
    wire [31:0] mem_rdata;
    wire [31:0] wb_data;
    wire        branch_taken;

    // ---- instruction fetch ---------------------------------------
    imem #(
        .INIT_FILE (IMEM_INIT)
    ) u_imem (
        .addr   (pc),
        .instr  (instr)
    );

    // ---- decode --------------------------------------------------
    decoder u_decoder (
        .instr      (instr),
        .rs1_addr   (rs1_addr),
        .rs2_addr   (rs2_addr),
        .rd_addr    (rd_addr),
        .funct3     (funct3),
        .alu_op     (alu_op),
        .alu_src    (alu_src),
        .alu_a_pc   (alu_a_pc),
        .alu_a_zero (alu_a_zero),
        .reg_we     (reg_we),
        .mem_we     (mem_we),
        .mem_re     (mem_re),
        .wb_sel     (wb_sel),
        .is_branch  (is_branch),
        .is_jal     (is_jal),
        .is_jalr    (is_jalr),
        .illegal    (illegal)
    );

    immgen u_immgen (
        .instr  (instr),
        .imm    (imm)
    );

    regfile #(.BYPASS(0)) u_regfile ( //flip to 1 in phase 3
        .clk        (clk),
        .rs1_addr   (rs1_addr),
        .rs1_data   (rs1_data),
        .rs2_addr   (rs2_addr),
        .rs2_data   (rs2_data),
        .rd_addr    (rd_addr),
        .rd_data    (wb_data),
        .rd_we      (reg_we)
    );

    // ---- execute -------------------------------------------------
    alu u_alu (
        .a      (alu_a),
        .b      (alu_b),
        .alu_op (alu_op),
        .result (alu_result)
    );

    branch_cmp u_branch_cmp (
        .a      (rs1_data),
        .b      (rs2_data),
        .funct3 (funct3),
        .taken  (branch_taken)
    );

    // ---- memory --------------------------------------------------
    dmem #(
        .INIT_FILE (DMEM_INIT)
    ) u_dmem (
        .clk    (clk),
        .addr   (alu_result),
        .wdata  (rs2_data),
        .funct3 (funct3),
        .we     (mem_we),
        .rdata  (mem_rdata)
    );

    //alu input a mux
    assign alu_a = alu_a_zero ? 32'b0 : 
                   alu_a_pc ? pc : rs1_data;
    //alu input b mux
    assign alu_b = alu_src ? imm : rs2_data;

    //writeback mux
    assign wb_data = (wb_sel == 2'b10) ? pc_plus4 : 
                     (wb_sel == 2'b01) ? mem_rdata : alu_result;

    // TODO: PC next-address logic and the pc register
    //Program Counter
    wire pc_redirect;
    assign pc_redirect = is_jal || is_jalr || (is_branch && branch_taken);
    assign pc_target = (is_jalr) ?  (alu_result & ~32'd1) : (pc + imm);
    assign pc_next = (pc_redirect) ? pc_target : pc_plus4;

    always @(posedge clk) begin
        if(~rst_n)begin
            pc <= RESET_PC;
        end 
        else begin
            pc <= pc_next;
        end
    end


    assign trace_pc    = pc;
    assign trace_instr = instr;
    assign trace_rd    = rd_addr;
    assign trace_wdata = wb_data;
    assign trace_we    = reg_we && (rd_addr != 5'b0);

endmodule
