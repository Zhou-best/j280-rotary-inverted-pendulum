module furuta_pid_ctrl (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        calc_en,
    input  wire        sync,


    input  wire signed [31:0] theta_err,
    input  wire signed [31:0] alpha_err,
    input  wire signed [31:0] dalpha,


    output reg  signed [15:0] pwm_duty,
    output reg                calc_done
);


    localparam signed [17:0] KP_18 = -18'sd36861;
    localparam signed [17:0] KD_18 = -18'sd4936;


    localparam signed [17:0] KC_18 = -18'sd4093;


    localparam signed [17:0] KV_18 = -18'sd8180;


    localparam signed [17:0] VOLT_TO_PWM_18 = 18'sd85333;
    localparam signed [15:0] PWM_MAX        = 16'sd1000;
    localparam signed [15:0] PWM_MIN        = -16'sd1000;


    reg signed [31:0] theta_f;
    reg signed [31:0] theta_f_prev;
    reg signed [31:0] delta_r;
    reg signed [31:0] theta_d1;
    reg signed [31:0] alpha_d1, alpha_d2;
    reg signed [31:0] dalpha_d1, dalpha_d2;
    reg               valid0, valid1;

    wire signed [31:0] theta_diff   = theta_err - theta_f;


    wire signed [31:0] theta_f_next = theta_f + (theta_diff >>> 5) - (theta_diff >>> 8) - (theta_diff >>> 9);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            theta_f      <= 32'sd0;
            theta_f_prev <= 32'sd0;
            delta_r      <= 32'sd0;
            theta_d1     <= 32'sd0;
            alpha_d1     <= 32'sd0;
            dalpha_d1    <= 32'sd0;
            valid0       <= 1'b0;
        end else if (sync) begin

            theta_f      <= theta_err;
            theta_f_prev <= theta_err;
            delta_r      <= 32'sd0;
            theta_d1     <= theta_err;


            alpha_d1     <= alpha_err;
            dalpha_d1    <= dalpha;
            valid0       <= 1'b1;
        end else if (calc_en) begin
            theta_f_prev <= theta_f;
            theta_f      <= theta_f_next;
            delta_r      <= theta_f_next - theta_f;
            theta_d1     <= theta_f_next;
            alpha_d1     <= alpha_err;
            dalpha_d1    <= dalpha;
            valid0       <= 1'b1;
        end else begin
            valid0       <= 1'b0;
        end
    end


    reg signed [31:0] dtheta_f;
    reg signed [31:0] theta_d2;

    wire signed [41:0] dth_x1000 = (delta_r <<< 10) - (delta_r <<< 4) - (delta_r <<< 3);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dtheta_f <= 32'sd0;
            theta_d2 <= 32'sd0;
            alpha_d2 <= 32'sd0;
            dalpha_d2 <= 32'sd0;
            valid1   <= 1'b0;
        end else if (valid0) begin
            dtheta_f <= dth_x1000[31:0];
            theta_d2 <= theta_d1;
            alpha_d2 <= alpha_d1;
            dalpha_d2 <= dalpha_d1;
            valid1   <= 1'b1;
        end else begin
            valid1   <= 1'b0;
        end
    end


    reg signed [49:0] prod1, prod2, prod3, prod4;
    reg               valid2;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            prod1  <= 50'sd0;
            prod2  <= 50'sd0;
            prod3  <= 50'sd0;
            prod4  <= 50'sd0;
            valid2 <= 1'b0;
        end else if (valid1) begin
            prod1  <= theta_d2 * KP_18;
            prod2  <= dtheta_f * KD_18;
            prod3  <= alpha_d2 * KC_18;
            prod4  <= dalpha_d2 * KV_18;
            valid2 <= 1'b1;
        end else begin
            valid2 <= 1'b0;
        end
    end


    reg signed [31:0] v_cmd_q16;
    reg               valid3;


    wire signed [49:0] sum_prods = prod1 + prod2 + prod3 + prod4;
    wire signed [49:0] v_cmd_raw = - (sum_prods >>> 10);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v_cmd_q16 <= 32'sd0;
            valid3    <= 1'b0;
        end else if (valid2) begin

            if (v_cmd_raw > 50'sd13107200)
                v_cmd_q16 <= 32'sd13107200;
            else if (v_cmd_raw < -50'sd13107200)
                v_cmd_q16 <= -32'sd13107200;
            else
                v_cmd_q16 <= v_cmd_raw[31:0];
            valid3    <= 1'b1;
        end else begin
            valid3    <= 1'b0;
        end
    end


    reg signed [49:0] pwm_mult_reg;
    reg               valid4;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_mult_reg <= 50'sd0;
            valid4       <= 1'b0;
        end else if (valid3) begin
            pwm_mult_reg <= $signed(v_cmd_q16) * VOLT_TO_PWM_18;
            valid4       <= 1'b1;
        end else begin
            valid4       <= 1'b0;
        end
    end


    wire signed [31:0] pwm_raw = pwm_mult_reg >>> 26;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_duty  <= 16'sd0;
            calc_done <= 1'b0;
        end else if (valid4) begin
            if (pwm_raw > PWM_MAX)
                pwm_duty <= PWM_MAX;
            else if (pwm_raw < PWM_MIN)
                pwm_duty <= PWM_MIN;
            else
                pwm_duty <= pwm_raw[15:0];
            calc_done <= 1'b1;
        end else begin
            calc_done <= 1'b0;
        end
    end

endmodule
