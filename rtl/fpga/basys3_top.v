`default_nettype none

module basys3_top (
    input  wire        clk,      // W5, 100 MHz
    input  wire        btnC,     // reset button, active high
    output wire [15:0] led
);

    wire cpu_clk;
    wire locked;

    clk_wiz_0 u_clk (
        .clk_in1  (clk),
        .clk_out1 (cpu_clk)
    );

    // ---- reset synchroniser ----
    // btnC is asynchronous and bouncy, and active-high; two flops resolve
    // metastability and the inversion gives the core its active-low reset.
    reg [1:0] rst_sync = 2'b0;
    always @(posedge cpu_clk) rst_sync <= {rst_sync[0], ~btnC};
    wire rst_n = rst_sync[1];

    // ---- heartbeat ----
    // Proves the clock is alive independently of whether the core runs.
    reg [25:0] hb = 26'b0;
    always @(posedge cpu_clk) hb <= hb + 1'b1;

    // ---- core ----
    wire [31:0] trace_pc, trace_wdata;

    core #(
        .IMEM_INIT ("C:/Users/jacqu/Desktop/rv32i-core/rv32i-core/rtl/fpga/rv32ui-p-add.hex"),
        .DMEM_INIT ("C:/Users/jacqu/Desktop/rv32i-core/rv32i-core/rtl/fpga/rv32ui-p-add.hex")
    ) u_core (
        .clk      (cpu_clk),
        .rst_n    (rst_n),
        .bus_stall(1'b0),
        .trace_pc (trace_pc),
        .trace_wdata (trace_wdata)
    );

    // led[15] blinks about once a second; led[14:0] show PC activity.
    assign led = {hb[25], trace_wdata[16:2]};

endmodule
