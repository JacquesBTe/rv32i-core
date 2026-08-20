`default_nettype none

module basys3_top (
    input  wire        clk,      // W5, 100 MHz
    input  wire        btnC,     // reset button, active high
    output wire [15:0] led
);

    // ---- clock divide to 25 MHz for bring-up ----
    reg [1:0] div = 2'b0;
    always @(posedge clk) div <= div + 1'b1;
    wire cpu_clk = div[1];

    // ---- reset synchroniser: two flops, invert to active-low ----
    reg [1:0] rst_sync = 2'b0;
    always @(posedge cpu_clk) rst_sync <= {rst_sync[0], ~btnC};
    wire rst_n = rst_sync[1];

    // ---- core ----
    wire [31:0] trace_pc;

    core #(
        .IMEM_INIT ("count.hex"),
        .DMEM_INIT ("count.hex")
    ) u_core (
        .clk      (cpu_clk),
        .rst_n    (rst_n),
        .trace_pc (trace_pc)
    );

    assign led = trace_pc[17:2];

endmodule
