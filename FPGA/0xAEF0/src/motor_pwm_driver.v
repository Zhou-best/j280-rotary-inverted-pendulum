module motor_pwm_driver #(
    parameter integer CLK_FREQ_HZ   = 50_000_000,
    parameter integer PWM_FREQ_HZ   = 20_000,
    parameter integer DUTY_MAX_VAL  = 1000,
    parameter integer DEAD_BAND_VAL = 0
)(
    input  wire                   clk,
    input  wire                   rst_n,
    input  wire                   motor_en,
    input  wire                   brake_mode,
    input  wire signed [15:0]     pwm_duty,


    output reg                    pwm_out,
    output reg                    dir_out,


    output reg                    in1_out,
    output reg                    in2_out,
    output wire                   stby_out
);


    localparam integer TIMER_PERIOD = CLK_FREQ_HZ / PWM_FREQ_HZ;

    localparam integer TIMER_WIDTH  = 16;

    reg [TIMER_WIDTH-1:0] pwm_counter;
    reg [TIMER_WIDTH-1:0] compare_val;
    reg                   dir_reg;
    reg                   pwm_active;


    assign stby_out = motor_en | brake_mode;


    wire [15:0] duty_abs = (pwm_duty < 0) ? (-pwm_duty) : pwm_duty;
    wire [15:0] duty_clamped = (duty_abs > DUTY_MAX_VAL) ? DUTY_MAX_VAL : duty_abs;
    wire        dir_detected = (pwm_duty < 0) ? 1'b1 : 1'b0;


    wire [15:0] duty_deadzoned = (duty_clamped <= DEAD_BAND_VAL) ? 16'd0 : duty_clamped;


    localparam [31:0] SCALE_FACTOR_Q16 = (TIMER_PERIOD * 65536) / DUTY_MAX_VAL;
    wire [31:0] compare_mult = duty_deadzoned * SCALE_FACTOR_Q16;
    wire [31:0] raw_compare  = compare_mult >> 16;


    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_counter <= {TIMER_WIDTH{1'b0}};
            compare_val <= {TIMER_WIDTH{1'b0}};
            dir_reg     <= 1'b0;
        end else begin
            if (pwm_counter >= TIMER_PERIOD - 1) begin
                pwm_counter <= {TIMER_WIDTH{1'b0}};

                compare_val <= raw_compare[TIMER_WIDTH-1:0];
                dir_reg     <= dir_detected;
            end else begin
                pwm_counter <= pwm_counter + 1'b1;
            end
        end
    end


    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_active <= 1'b0;
        end else begin
            if (compare_val == 0) begin
                pwm_active <= 1'b0;
            end else if (pwm_counter < compare_val) begin
                pwm_active <= 1'b1;
            end else begin
                pwm_active <= 1'b0;
            end
        end
    end


    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_out <= 1'b0;
            dir_out <= 1'b0;
            in1_out <= 1'b0;
            in2_out <= 1'b0;
        end else if (!motor_en) begin

            dir_out <= 1'b0;
            if (brake_mode) begin

                pwm_out <= 1'b0;
                in1_out <= 1'b1;
                in2_out <= 1'b1;
            end else begin


                pwm_out <= 1'b1;
                in1_out <= 1'b0;
                in2_out <= 1'b0;
            end
        end else begin


            pwm_out <= pwm_active;
            dir_out <= dir_reg;
            in1_out <= ~dir_reg;
            in2_out <=  dir_reg;
        end
    end

endmodule
