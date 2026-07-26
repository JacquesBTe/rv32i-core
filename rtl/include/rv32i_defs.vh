`default_nettype none

module regfile (
    input  wire        clk,

    input  wire [4:0]  rs1_addr,
    output wire [31:0] rs1_data,

    input  wire [4:0]  rs2_addr,
    output wire [31:0] rs2_data,

    input  wire [4:0]  rd_addr,
    input  wire [31:0] rd_data,
    input  wire        rd_we
);

    reg [31:0] regs [0:31];

    integer i;
    initial begin
        // zero all 32 entries
    end

    always @(posedge clk) begin
        // write when enabled and not x0
    end

    wire bypass_rs1 = /* three terms */;
    wire bypass_rs2 = /* three terms */;

    wire [31:0] rs1_muxed = /* bypass ? incoming : array */;
    wire [31:0] rs2_muxed = /* same shape */;

    assign rs1_data = /* x0 gate applied to rs1_muxed */;
    assign rs2_data = /* x0 gate applied to rs2_muxed */;

endmodule