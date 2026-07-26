// Throwaway module to validate the Verilator + FST + GTKWave flow.
module selftest (
    input  wire       clk,
    input  wire       rst_n,
    input  wire       en,
    output reg  [7:0] count,
    output wire       is_max
);
    always @(posedge clk) begin
        if (!rst_n)  count <= 8'd0;
        else if (en) count <= count + 8'd1;
    end

    assign is_max = (count == 8'hFF);
endmodule
