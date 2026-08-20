`default_nettype none

module imem #(
    parameter INIT_FILE = "imem_test.hex"
) (

    input wire clk,
    input wire en,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire [31:0] addr,
    /* verilator lint_on UNUSEDSIGNAL */
    output reg [31:0] instr
);

    reg [31:0] mem [0:16383];

    // +imem=<file> overrides INIT_FILE, so one build runs every test.
    string hexfile;
    initial begin
        if ($value$plusargs("imem=%s", hexfile))
            $readmemh(hexfile, mem);
        else if (INIT_FILE != "")
            $readmemh(INIT_FILE, mem);
    end

    always @(posedge clk) begin
       if (en) instr <= mem[addr[15:2]];
    end


endmodule
