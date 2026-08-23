`default_nettype none

module dmem #(
    parameter INIT_FILE = ""
) (
    input  wire        clk,
    input  wire        en,
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [31:0] addr,
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire [31:0] wdata,
    input  wire [3:0]  wstrb,
    output reg  [31:0] rdata
);

    // Plain 32-bit word memory, synchronous read, byte-strobed write.
    // Byte-lane selection, shifting, and sign extension now live in
    // axil_master -- wstrb is an AXI concern, not this module's.

    reg [31:0] mem[0:16383]; //memory array -- 64KB of memory

    wire [13:0] word_idx = addr[15:2]; //dropping the first two bits, is dividing by 4, byte 8 becomes word 2.

    always @(posedge clk) if (en) rdata <= mem[word_idx];

    // Four independent lane writes -- Vivado needs this shape to infer
    // byte-write enables.
    always @(posedge clk) begin
        if (wstrb[0]) mem[word_idx][7:0]   <= wdata[7:0];
        if (wstrb[1]) mem[word_idx][15:8]  <= wdata[15:8];
        if (wstrb[2]) mem[word_idx][23:16] <= wdata[23:16];
        if (wstrb[3]) mem[word_idx][31:24] <= wdata[31:24];
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
