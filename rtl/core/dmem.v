`default_nettype none

module dmem #(
    parameter INIT_FILE = ""
) (
    input  wire        clk,
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [31:0] addr,
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire [31:0] wdata,
    input  wire [2:0]  funct3, //instruction bits 14:12, access size 1:0, then 2 has unsigned flag
    input  wire        we,
    output reg  [31:0] rdata
);

    // NOTE: misaligned accesses are not detected. RV32I says they should
    // trap; there is no trap mechanism until phase 5.

    reg [31:0] mem[0:16383]; //memory array -- 64KB of memory


    //address decompostion
    wire [13:0] word_idx = addr[15:2]; //dropping the first two bits, is dividing by 4, byte 8 becomes word 2. 
    wire [1:0]  lane     = addr[1:0]; //remainder of mod 4
    wire [4:0]  shift    = lane << 3;   // lane * 8, each lane is 8 bits. 

    // ---- store -----------------------------------------------------
    reg [3:0] wstrb; //word strobe, allows use to know how many lanes to activat when writing
    always @(*) begin
        case (funct3[1:0])
            2'b00:   wstrb = 4'b0001 << lane;   // sb
            2'b01:   wstrb = 4'b0011 << lane;   // sh
            2'b10:   wstrb = 4'b1111;           // sw
            default: wstrb = 4'b0000;
        endcase
    end

    wire [31:0] wdata_shifted = wdata << shift;

    always @(posedge clk) begin
        if (we) begin
            if (wstrb[0]) mem[word_idx][7:0] <= wdata_shifted[7:0];
            if (wstrb[1]) mem[word_idx][15:8] <= wdata_shifted[15:8];
            if (wstrb[2]) mem[word_idx][23:16] <= wdata_shifted[23:16];
            if (wstrb[3]) mem[word_idx][31:24] <= wdata_shifted[31:24];
        end
    end

    // ---- load ------------------------------------------------------
    wire [31:0] word    = mem[word_idx];
    /* verilator lint_off UNUSEDSIGNAL */
    wire [31:0] shifted = word >> shift;
    /* verilator lint_on UNUSEDSIGNAL */
    always @(*) begin
        case (funct3)
            3'b000:  rdata = {{24{shifted[7]}}, shifted[7:0]};   // lb
            3'b001:  rdata = {{16{shifted[15]}}, shifted[15:0]};   // lh
            3'b010:  rdata = word;                                 // lw
            3'b100:  rdata = {24'b0, shifted[7:0]};               // lbu
            3'b101:  rdata = {16'b0, shifted[15:0]};               // lhu
            default: rdata = word;
        endcase
    end

    // dmem.v
    string hexfile;
    initial begin
        if ($value$plusargs("dmem=%s", hexfile))
            $readmemh(hexfile, mem);
        else if (INIT_FILE != "")
            $readmemh(INIT_FILE, mem);
    end

endmodule
