`default_nettype none

module imem #(
    parameter INIT_FILE = "imem_test.hex"
) (
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [31:0] addr,
    /* verilator lint_on UNUSEDSIGNAL */
    output wire [31:0] instr
);

    reg [31:0] mem [0:16383];

    initial begin
        if (INIT_FILE != "") $readmemh(INIT_FILE, mem);
    end

    assign instr = mem[addr[15:2]];

endmodule
