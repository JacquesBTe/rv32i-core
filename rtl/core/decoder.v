`default_nettype none
`include "rv32i_defs.vh"

module decoder (
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [31:0] instr,
    /* verilator lint_on UNUSEDSIGNAL */

    output wire [4:0]  rs1_addr,
    output wire [4:0]  rs2_addr,
    output wire [4:0]  rd_addr,
    output wire [2:0]  funct3,

    output reg  [3:0]  alu_op, 
    output reg         alu_src,      // 0 = rs2, 1 = immediate
    output reg         alu_a_pc,     // 1 = ALU 'a' input is PC (auipc)
    output reg         alu_a_zero,   // 1 = ALU 'a' input is 0 (lui)

    output reg         reg_we,
    output reg         mem_we,
    output reg         mem_re,
    output reg  [1:0]  wb_sel,       // 0 = ALU, 1 = mem, 2 = PC+4, 3 = CSR

    output reg         is_branch, //conditional jump
    output reg         is_jal, //jump to 
    output reg         is_jalr, //jump to computed address

    output reg         illegal,

    output reg  [1:0]  csr_op,        // 00 none, 01 rw, 10 rs, 11 rc
    output reg         csr_re,
    output reg         csr_we,
    output reg         csr_use_imm,   // rs1 field is a 5-bit uimm, not a register
    output reg         is_mret,
    output reg         is_ecall
);

assign rs1_addr = instr[19:15];
assign rs2_addr = instr[24:20];
assign rd_addr  = instr[11:7];

assign funct3 = instr[14:12];

//ALU operations
always @(*)begin
    //defaults
    alu_op = `ALU_ADD; alu_src = 1'b1; //src imm by default
    alu_a_pc = 1'b0; alu_a_zero = 1'b0;
    reg_we = 1'b0; mem_we = 1'b0; mem_re = 1'b0;
    wb_sel = 2'd0; //ALU is default
    is_branch = 1'b0; is_jal = 1'b0; is_jalr = 1'b0;
    illegal = 1'b0;
    csr_op = 2'b00; csr_re = 1'b0; csr_we = 1'b0;
    csr_use_imm = 1'b0; is_mret = 1'b0; is_ecall = 1'b0;

    case(instr[6:0])
        //U-type instructions
        7'b0110111: begin reg_we = 1'b1; alu_a_pc = 1'b0; alu_a_zero = 1'b1; end  //LUI
        7'b0010111: begin reg_we = 1'b1; alu_a_pc = 1'b1; alu_a_zero = 1'b0; end  //AUIPC
        //J-type instructions
        7'b1101111: begin reg_we = 1'b1; wb_sel = 2'd2; is_jal = 1'b1;  end //JAL
        //B-type instructions
        7'b1100011: begin reg_we = 1'b0; is_branch = 1'b1; end //BRANCH
        //S-type instructions
        7'b0100011: begin reg_we = 1'b0; mem_we = 1'b1; end //STORE
        //R- type instructions
        7'b0110011: begin reg_we = 1'b1; alu_src = 1'b0; alu_op = {instr[30], funct3}; end //OP
        //I-type instructions 
        7'b1100111: begin reg_we = 1'b1; wb_sel = 2'd2; is_jalr = 1'b1; end//JALR
        7'b0000011: begin reg_we = 1'b1; mem_re = 1'b1; wb_sel = 2'd1; end//LOAD
        7'b0010011: begin reg_we = 1'b1; alu_src = 1'b1; alu_op = {(funct3 == 3'b101) ? instr[30] : 1'b0, funct3}; end //OP-IMM
        7'b0001111: begin end //MISC-MEM //fence -- no operation, no memory reordering to prevent
        7'b1110011: begin 
             if (funct3 == 3'b000) begin
                case (instr[31:20])
                    12'h000: is_ecall = 1'b1;                // ecall
                    12'h001: is_ecall = 1'b1;                // ebreak -> same trap path
                    12'h302: is_mret  = 1'b1;                // mret
                    default: illegal  = 1'b1;
                endcase
            end else begin
                reg_we      = 1'b1;
                wb_sel      = 2'd3;                          // CSR read value
                csr_use_imm = funct3[2];                     // 1xx = immediate forms
                case (funct3[1:0])
                    2'b01:   csr_op = 2'b01;                 // csrrw
                    2'b10:   csr_op = 2'b10;                 // csrrs
                    2'b11:   csr_op = 2'b11;                 // csrrc
                    default: illegal = 1'b1;                 // funct3 = x00 invalid here
                endcase
                // csrrw with rd = x0 need not read.
                csr_re = !((funct3[1:0] == 2'b01) && (rd_addr == 5'b0));
                // set/clear with a zero source must not write.
                csr_we = !((funct3[1:0] != 2'b01) && (instr[19:15] == 5'b0));
            end
        end//SYSTEM ecall/ebreak -- until phase 5
        default:    illegal = 1'b1; // if instruction doesnt fit into the cases it doesnt exist
    endcase

end





endmodule
