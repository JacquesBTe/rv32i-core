`default_nettype none

module basys3_top (
    input  wire        clk,      // W5, 100 MHz
    input  wire        btnC,     // reset button, active high
    input  wire [15:0] sw,
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

    // ---- soc ----
    soc #(
        .IMEM_INIT ("C:/Users/jacqu/Desktop/rv32i-core/rv32i-core/rtl/fpga/gpio_loop.hex"),
        .DMEM_INIT ("C:/Users/jacqu/Desktop/rv32i-core/rv32i-core/rtl/fpga/gpio_loop.hex")
    ) u_soc (
        .clk   (cpu_clk),
        .rst_n (rst_n),
        .led   (led),
        .sw    (sw)
    );

endmodule
