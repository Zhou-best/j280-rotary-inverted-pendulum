module adc_3pa1030_reader #(
    parameter integer CLK_FREQ_HZ = 50_000_000,
    parameter integer ADC_DIV     = 4,
    parameter integer AVG_BITS    = 12
)(
    input  wire        clk,
    input  wire        rst_n,
    input  wire        calc_en,


    output wire        adc_clk,
    output wire        adc_oe_n,
    input  wire [9:0]  adc_data_in,


    output reg  [11:0] raw_12bit,
    output reg         raw_valid
);


    reg [1:0] div_cnt;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) div_cnt <= 2'd0;
        else        div_cnt <= div_cnt + 2'd1;
    end

    assign adc_clk  = div_cnt[1];
    assign adc_oe_n = 1'b0;


    wire sample_en = (div_cnt == 2'd3) || (div_cnt == 2'd0);


    reg  [31:0]         sum_accum;
    reg  [AVG_BITS-1:0] samp_cnt;
    reg  [9:0]          avg_code;


    wire [31:0] sum_full = sum_accum + {22'd0, adc_data_in};

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sum_accum <= 32'd0;
            samp_cnt  <= {AVG_BITS{1'b0}};
            avg_code  <= 10'd0;
        end else if (sample_en) begin
            if (samp_cnt == {AVG_BITS{1'b1}}) begin

                avg_code  <= sum_full[AVG_BITS+9 : AVG_BITS];
                sum_accum <= 32'd0;
                samp_cnt  <= {AVG_BITS{1'b0}};
            end else begin
                sum_accum <= sum_full;
                samp_cnt  <= samp_cnt + 1'b1;
            end
        end
    end


    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            raw_12bit <= 12'd0;
            raw_valid <= 1'b0;
        end else begin
            raw_valid <= calc_en;
            if (calc_en)
                raw_12bit <= {avg_code, 2'b00};
        end
    end

endmodule
