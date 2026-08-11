`default_nettype none

module regfile #(parameter BYPASS = 1) (
    input  wire        clk, 

    input  wire [4:0]  rs1_addr, //source address of port A
    output wire [31:0] rs1_data, //source data of port A

    input  wire [4:0]  rs2_addr, //source address of port B
    output wire [31:0] rs2_data, //source data of port B

    input  wire [4:0]  rd_addr, //register destination address
    input  wire [31:0] rd_data, //register destination data 
    input  wire        rd_we
);

    reg [31:0] regs [0:31];

    //only have this initial block since FPGA needs to initialize 
    //if designing for silicon add this for synthesis and remove initial block 
    //`ifndef SYNTHESIS
    //integer i;
    //initial for (i = 0; i < 32; i = i + 1) regs[i] = 32'b0;
    //`endif
    integer i;
    initial begin
        // zero all 32 entries
        for(i = 0; i<32; i=i+1)
        begin
            regs[i] = 32'b0; //initialize the regs to be 0 for all bits
        end
    end


    always @(posedge clk) begin
        // write when enabled and not x0
        if (rd_we && rd_addr != 5'b0) begin //if write is enabled and we are no writing to x0 then we write rd_data to our register
            regs[rd_addr] <= rd_data; 
        end
    end

    //bypass flags condition -> if being written to a register in the same cycle that someone is reading that same register.
    wire bypass_rs1 = BYPASS && rd_we && (rd_addr == rs1_addr) && (rs1_addr != 5'b0); 
    wire bypass_rs2 = BYPASS && rd_we && (rd_addr == rs2_addr) && (rs2_addr != 5'b0);

    wire [31:0] rs1_muxed = bypass_rs1 ? rd_data : regs[rs1_addr];
    wire [31:0] rs2_muxed = bypass_rs2 ? rd_data : regs[rs2_addr];

    assign rs1_data = (rs1_addr == 5'b0) ? 32'b0 : rs1_muxed;
    assign rs2_data = (rs2_addr == 5'b0) ? 32'b0 : rs2_muxed;

endmodule
