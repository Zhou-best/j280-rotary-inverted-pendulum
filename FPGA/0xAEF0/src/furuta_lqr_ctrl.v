module furuta_lqr_ctrl (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        calc_en,


    input  wire signed [31:0] theta_err,
    input  wire signed [31:0] dtheta,
    input  wire signed [31:0] alpha_err,
    input  wire signed [31:0] dalpha,
    input  wire signed [31:0] alpha_int,


    output reg  signed [15:0] pwm_duty,
    output reg                calc_done
);


    localparam signed [17:0] K1_18 = -18'sd89277;


    localparam signed [17:0] K2_18 = -18'sd4936;
    localparam signed [17:0] K3_18 = -18'sd9996;


    localparam signed [17:0] K4_18 = -18'sd7415;


    localparam signed [17:0] VOLT_TO_PWM_18 = 18'sd85333;
    localparam signed [15:0] PWM_MAX        = 16'sd1000;
    localparam signed [15:0] PWM_MIN        = -16'sd1000;


    reg signed [49:0] prod1, prod2, prod3, prod4, prod5;
    reg               stage1_valid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            prod1        <= 50'sd0;
            prod2        <= 50'sd0;
            prod3        <= 50'sd0;
            prod4        <= 50'sd0;
            prod5        <= 50'sd0;
            stage1_valid <= 1'b0;
        end else if (calc_en) begin
            prod1        <= $signed(theta_err) * K1_18;
            prod2        <= $signed(dtheta)    * K2_18;
            prod3        <= $signed(alpha_err) * K3_18;
            prod4        <= $signed(dalpha)    * K4_18;
            prod5        <= - ($signed({{18{alpha_int[31]}}, alpha_int}) <<< 13);
            stage1_valid <= 1'b1;
        end else begin
            stage1_valid <= 1'b0;
        end
    end


    reg signed [31:0] v_cmd_q16;
    reg               stage2_valid;

    wire signed [49:0] sum_prods = prod1 + prod2 + prod3 + prod4 + prod5;
    wire signed [49:0] v_cmd_raw = - (sum_prods >>> 10);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v_cmd_q16    <= 32'sd0;
            stage2_valid <= 1'b0;
        end else if (stage1_valid) begin

            if (v_cmd_raw > 50'sd13107200)
                v_cmd_q16 <= 32'sd13107200;
            else if (v_cmd_raw < -50'sd13107200)
                v_cmd_q16 <= -32'sd13107200;
            else
                v_cmd_q16 <= v_cmd_raw[31:0];
            stage2_valid <= 1'b1;
        end else begin
            stage2_valid <= 1'b0;
        end
    end


    reg signed [49:0] pwm_mult_reg;
    reg               stage3_valid;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_mult_reg <= 50'sd0;
            stage3_valid <= 1'b0;
        end else if (stage2_valid) begin
            pwm_mult_reg <= $signed(v_cmd_q16) * VOLT_TO_PWM_18;
            stage3_valid <= 1'b1;
        end else begin
            stage3_valid <= 1'b0;
        end
    end


    wire signed [49:0] pwm_calc = pwm_mult_reg >>> 26;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_duty  <= 16'd0;
            calc_done <= 1'b0;
        end else if (stage3_valid) begin
            if (pwm_calc > 50'sd1000)
                pwm_duty <= PWM_MAX;
            else if (pwm_calc < -50'sd1000)
                pwm_duty <= PWM_MIN;
            else
                pwm_duty <= pwm_calc[15:0];
            calc_done <= 1'b1;
        end else begin
            calc_done <= 1'b0;
        end
    end

endmodule
