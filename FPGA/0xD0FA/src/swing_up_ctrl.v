module swing_up_ctrl #(


    parameter integer MAX_ACC_PUMP   = 24,
    parameter integer VOLT_TO_PWM_K  = 18
)(
    input  wire                   clk,
    input  wire                   rst_n,
    input  wire                   calc_en,
    input  wire                   swing_active,
    input  wire signed [31:0]     theta_rad_q16,
    input  wire signed [31:0]     dtheta_rad_s_q16,
    input  wire signed [31:0]     alpha_rad_q16,
    input  wire signed [31:0]     dalpha_rad_s_q16,

    output reg  signed [15:0]     pwm_swing_duty,
    output reg                    energy_deficit,
    output reg                    calc_done
);


    localparam signed [31:0] ACC_LIMIT_Q16 = MAX_ACC_PUMP * 32'sd65536;

    localparam signed [31:0] BRAKE_ACC_LIMIT_Q16 = 32'sd2949120;
    localparam signed [31:0] BRAKE_ANGLE_Q16     = 32'sd34315;
    localparam signed [31:0] BOOST_ANGLE_Q16     = 32'sd137259;
    localparam signed [31:0] PWM_MAX_SWING       = 45 * VOLT_TO_PWM_K;


    localparam signed [31:0] DTH_DEADBAND_Q16 = 32'sd52429;
    wire signed [31:0] dtheta_db = (dtheta_rad_s_q16 >  DTH_DEADBAND_Q16) ? (dtheta_rad_s_q16 - DTH_DEADBAND_Q16) :
                                  (dtheta_rad_s_q16 < -DTH_DEADBAND_Q16) ? (dtheta_rad_s_q16 + DTH_DEADBAND_Q16) :
                                                                           32'sd0;
    wire signed [31:0] abs_theta_q16 = (theta_rad_q16 < 0) ? (-theta_rad_q16) : theta_rad_q16;
    wire [47:0] idx_mult = abs_theta_q16 * 32'd20535;
    wire [5:0]  cos_idx  = (abs_theta_q16 >= 32'sd205887) ? 6'd63 : idx_mult[31:26];

    reg signed [15:0] cos_lut_q14;
    always @(*) begin
        case (cos_idx)
            6'd0:  cos_lut_q14 =  16'sd16384; 6'd1:  cos_lut_q14 =  16'sd16364;
            6'd2:  cos_lut_q14 =  16'sd16303; 6'd3:  cos_lut_q14 =  16'sd16201;
            6'd4:  cos_lut_q14 =  16'sd16059; 6'd5:  cos_lut_q14 =  16'sd15877;
            6'd6:  cos_lut_q14 =  16'sd15656; 6'd7:  cos_lut_q14 =  16'sd15396;
            6'd8:  cos_lut_q14 =  16'sd15097; 6'd9:  cos_lut_q14 =  16'sd14761;
            6'd10: cos_lut_q14 =  16'sd14389; 6'd11: cos_lut_q14 =  16'sd13980;
            6'd12: cos_lut_q14 =  16'sd13537; 6'd13: cos_lut_q14 =  16'sd13060;
            6'd14: cos_lut_q14 =  16'sd12551; 6'd15: cos_lut_q14 =  16'sd12010;
            6'd16: cos_lut_q14 =  16'sd11440; 6'd17: cos_lut_q14 =  16'sd10841;
            6'd18: cos_lut_q14 =  16'sd10215; 6'd19: cos_lut_q14 =  16'sd9564;
            6'd20: cos_lut_q14 =  16'sd8889;  6'd21: cos_lut_q14 =  16'sd8192;
            6'd22: cos_lut_q14 =  16'sd7475;  6'd23: cos_lut_q14 =  16'sd6739;
            6'd24: cos_lut_q14 =  16'sd5986;  6'd25: cos_lut_q14 =  16'sd5218;
            6'd26: cos_lut_q14 =  16'sd4437;  6'd27: cos_lut_q14 =  16'sd3646;
            6'd28: cos_lut_q14 =  16'sd2845;  6'd29: cos_lut_q14 =  16'sd2037;
            6'd30: cos_lut_q14 =  16'sd1224;  6'd31: cos_lut_q14 =  16'sd408;
            6'd32: cos_lut_q14 = -16'sd408;   6'd33: cos_lut_q14 = -16'sd1224;
            6'd34: cos_lut_q14 = -16'sd2037;  6'd35: cos_lut_q14 = -16'sd2845;
            6'd36: cos_lut_q14 = -16'sd3646;  6'd37: cos_lut_q14 = -16'sd4437;
            6'd38: cos_lut_q14 = -16'sd5218;  6'd39: cos_lut_q14 = -16'sd5986;
            6'd40: cos_lut_q14 = -16'sd6739;  6'd41: cos_lut_q14 = -16'sd7475;
            6'd42: cos_lut_q14 = -16'sd8192;  6'd43: cos_lut_q14 = -16'sd8889;
            6'd44: cos_lut_q14 = -16'sd9564;  6'd45: cos_lut_q14 = -16'sd10215;
            6'd46: cos_lut_q14 = -16'sd10841; 6'd47: cos_lut_q14 = -16'sd11440;
            6'd48: cos_lut_q14 = -16'sd12010; 6'd49: cos_lut_q14 = -16'sd12551;
            6'd50: cos_lut_q14 = -16'sd13060; 6'd51: cos_lut_q14 = -16'sd13537;
            6'd52: cos_lut_q14 = -16'sd13980; 6'd53: cos_lut_q14 = -16'sd14389;
            6'd54: cos_lut_q14 = -16'sd14761; 6'd55: cos_lut_q14 = -16'sd15097;
            6'd56: cos_lut_q14 = -16'sd15396; 6'd57: cos_lut_q14 = -16'sd15656;
            6'd58: cos_lut_q14 = -16'sd15877; 6'd59: cos_lut_q14 = -16'sd16059;
            6'd60: cos_lut_q14 = -16'sd16201; 6'd61: cos_lut_q14 = -16'sd16303;
            6'd62: cos_lut_q14 = -16'sd16364; 6'd63: cos_lut_q14 = -16'sd16384;
            default: cos_lut_q14 = 16'sd16384;
        endcase
    end


    wire signed [17:0] dth_in_q12       = (dtheta_db > 32'sd2097151) ? 18'sd131071 :
                                          ((dtheta_db < -32'sd2097152) ? -18'sd131072 : dtheta_db[21:4]);
    reg signed [35:0] dth_sq_s0;


    localparam integer REST_KICK_CYCLES = 300;
    reg [9:0] still_cnt;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            still_cnt <= 10'd0;
        end else if (calc_en) begin
            if (!swing_active) begin
                still_cnt <= 10'd0;
            end else if (dtheta_db != 32'sd0) begin

                if (still_cnt > 10'd2) still_cnt <= still_cnt - 10'd2;
                else                   still_cnt <= 10'd0;
            end else if (still_cnt < 10'd511) begin
                still_cnt <= still_cnt + 10'd1;
            end
        end
    end


    reg signed [15:0] cos_q14_s0;
    reg signed [31:0] dtheta_s0;
    reg signed [31:0] alpha_s0;
    reg signed [31:0] dalpha_s0;
    reg               is_dead_s0;
    reg               pipe_valid0;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cos_q14_s0   <= 16'sd16384;
            dtheta_s0    <= 32'sd0;
            alpha_s0     <= 32'sd0;
            dalpha_s0    <= 32'sd0;
            dth_sq_s0    <= 36'sd0;
            is_dead_s0   <= 1'b0;
            pipe_valid0  <= 1'b0;
        end else if (calc_en) begin
            cos_q14_s0   <= cos_lut_q14;
            dtheta_s0    <= dtheta_db;
            alpha_s0     <= alpha_rad_q16;
            dalpha_s0    <= dalpha_rad_s_q16;
            dth_sq_s0    <= dth_in_q12 * dth_in_q12;
            is_dead_s0   <= (still_cnt >= REST_KICK_CYCLES);
            pipe_valid0  <= 1'b1;
        end else begin
            pipe_valid0  <= 1'b0;
        end
    end


    wire signed [47:0] e_kin_scaled     = dth_sq_s0 * 18'sd22;
    wire signed [47:0] e_kin_shift      = e_kin_scaled >>> 24;
    wire signed [31:0] e_kin_q16        = e_kin_shift[31:0];


    wire signed [31:0] cos_diff_q14     = $signed(cos_q14_s0) - 32'sd16384;
    wire signed [47:0] e_pot_mult       = cos_diff_q14 * 32'sd4340;
    wire signed [47:0] e_pot_shift      = e_pot_mult >>> 14;
    wire signed [31:0] e_pot_q16        = e_pot_shift[31:0];

    wire signed [31:0] e_total_q16      = e_kin_q16 + e_pot_q16;
    wire               e_deficit_calc   = (e_total_q16 < 32'sd0);


    wire signed [47:0] dth_cos_mult     = $signed(dtheta_s0) * $signed(cos_q14_s0);
    wire signed [47:0] dth_cos_shift    = dth_cos_mult >>> 14;
    wire signed [31:0] dth_cos_q16      = dth_cos_shift[31:0];


    wire signed [47:0] dalpha_damp_mult = $signed(dalpha_s0) * 32'sd19661;
    wire signed [47:0] dalpha_damp_shift = dalpha_damp_mult >>> 16;
    wire signed [31:0] dalpha_damp_q16  = dalpha_damp_shift[31:0];


    wire               endgame_brake    = (abs_theta_q16 < BRAKE_ANGLE_Q16);
    wire               bottom_boost     = (abs_theta_q16 > BOOST_ANGLE_Q16);
    wire signed [31:0] arm_limit_q16    = alpha_s0 + (endgame_brake ? dalpha_s0 : dalpha_damp_q16);


    reg signed [31:0] dth_cos_q16_r;
    reg signed [31:0] arm_limit_q16_r;
    reg               endgame_brake_r1;
    reg               bottom_boost_r1;
    reg               is_dead_center_r;
    reg               energy_deficit_r;
    reg               pipe_valid1;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dth_cos_q16_r    <= 32'sd0;
            arm_limit_q16_r  <= 32'sd0;
            endgame_brake_r1 <= 1'b0;
            bottom_boost_r1  <= 1'b0;
            is_dead_center_r <= 1'b0;
            energy_deficit_r <= 1'b1;
            pipe_valid1      <= 1'b0;
        end else if (pipe_valid0) begin
            dth_cos_q16_r    <= dth_cos_q16;
            arm_limit_q16_r  <= arm_limit_q16;
            endgame_brake_r1 <= endgame_brake;
            bottom_boost_r1  <= bottom_boost;
            is_dead_center_r <= is_dead_s0;
            energy_deficit_r <= e_deficit_calc;
            pipe_valid1      <= 1'b1;
        end else begin
            pipe_valid1      <= 1'b0;
        end
    end


    wire signed [31:0] x_mult = (dth_cos_q16_r <<< 4) + (dth_cos_q16_r <<< 2);
    wire signed [31:0] abs_x  = (x_mult < 0) ? (-x_mult) : x_mult;
    wire               x_neg  = (x_mult < 0);


    reg signed [31:0] tanh_abs_q16;
    always @(*) begin
        if (abs_x < 32'sd32768)
            tanh_abs_q16 = abs_x - (abs_x >>> 4) - (abs_x >>> 6);
        else if (abs_x < 32'sd65536)
            tanh_abs_q16 = (abs_x >>> 1) + (abs_x >>> 4) + 32'sd11852;
        else if (abs_x < 32'sd131072)
            tanh_abs_q16 = (abs_x >>> 3) + (abs_x >>> 4) + 32'sd36428;
        else
            tanh_abs_q16 = 32'sd65536;
    end

    reg signed [31:0] tanh_abs_r2;
    reg               x_neg_r2;
    reg signed [31:0] arm_limit_q16_r2;
    reg               endgame_brake_r2;
    reg               bottom_boost_r2;
    reg               is_dead_center_r2;
    reg               energy_deficit_r2;
    reg               pipe_valid2;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tanh_abs_r2       <= 32'sd0;
            x_neg_r2          <= 1'b0;
            arm_limit_q16_r2  <= 32'sd0;
            endgame_brake_r2  <= 1'b0;
            bottom_boost_r2   <= 1'b0;
            is_dead_center_r2 <= 1'b0;
            energy_deficit_r2 <= 1'b1;
            pipe_valid2       <= 1'b0;
        end else if (pipe_valid1) begin
            tanh_abs_r2       <= tanh_abs_q16;
            x_neg_r2          <= x_neg;
            arm_limit_q16_r2  <= arm_limit_q16_r;
            endgame_brake_r2  <= endgame_brake_r1;
            bottom_boost_r2   <= bottom_boost_r1;
            is_dead_center_r2 <= is_dead_center_r;
            energy_deficit_r2 <= energy_deficit_r;
            pipe_valid2       <= 1'b1;
        end else begin
            pipe_valid2       <= 1'b0;
        end
    end


    wire signed [31:0] pump_mag_45 = (tanh_abs_r2 <<< 5) + (tanh_abs_r2 <<< 3) +
                                     (tanh_abs_r2 <<< 2) + tanh_abs_r2;
    wire signed [31:0] pump_mag_base = (MAX_ACC_PUMP == 30) ?
                                          ((tanh_abs_r2 <<< 5) - (tanh_abs_r2 <<< 1)) :
                                          (MAX_ACC_PUMP == 24) ?
                                          ((tanh_abs_r2 <<< 4) + (tanh_abs_r2 <<< 3)) :
                                          (MAX_ACC_PUMP == 21) ?
                                          ((tanh_abs_r2 <<< 4) + (tanh_abs_r2 <<< 2) + tanh_abs_r2) :
                                          (MAX_ACC_PUMP == 36) ?
                                          ((tanh_abs_r2 <<< 5) + (tanh_abs_r2 <<< 2)) :
                                          (tanh_abs_r2 * MAX_ACC_PUMP);
    wire signed [31:0] pump_mag = bottom_boost_r2 ? pump_mag_45 : pump_mag_base;
    wire signed [31:0] pump_acc_q16     = x_neg_r2 ? pump_mag : (-pump_mag);


    wire signed [31:0] pump_gated_q16   = energy_deficit_r2 ? pump_acc_q16 : 32'sd0;

    localparam signed [31:0] KICK_ACC_Q16 = 32'sd1572864;
    wire signed [31:0] base_acc_q16     = is_dead_center_r2 ? (KICK_ACC_Q16 + arm_limit_q16_r2) : pump_gated_q16;

    reg signed [31:0] base_acc_q16_r3;
    reg signed [31:0] arm_limit_q16_r3;
    reg               endgame_brake_r3;
    reg               bottom_boost_r3;
    reg               energy_deficit_r3;
    reg               pipe_valid3;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            base_acc_q16_r3   <= 32'sd0;
            arm_limit_q16_r3  <= 32'sd0;
            endgame_brake_r3  <= 1'b0;
            bottom_boost_r3   <= 1'b0;
            energy_deficit_r3 <= 1'b1;
            pipe_valid3       <= 1'b0;
        end else if (pipe_valid2) begin
            base_acc_q16_r3   <= base_acc_q16;
            arm_limit_q16_r3  <= arm_limit_q16_r2;
            endgame_brake_r3  <= endgame_brake_r2;
            bottom_boost_r3   <= bottom_boost_r2;
            energy_deficit_r3 <= energy_deficit_r2;
            pipe_valid3       <= 1'b1;
        end else begin
            pipe_valid3       <= 1'b0;
        end
    end


    wire signed [31:0] raw_acc_q16 = base_acc_q16_r3 - arm_limit_q16_r3;


    wire signed [31:0] acc_limit_q16 = (endgame_brake_r3 || bottom_boost_r3) ? BRAKE_ACC_LIMIT_Q16 : ACC_LIMIT_Q16;
    wire signed [31:0] clamped_acc_q16 = (raw_acc_q16 > acc_limit_q16)  ? acc_limit_q16 :
                                         ((raw_acc_q16 < -acc_limit_q16) ? -acc_limit_q16 : raw_acc_q16);


    wire signed [47:0] pwm_mult_acc = (VOLT_TO_PWM_K == 18) ?
                                      ((clamped_acc_q16 <<< 4) + (clamped_acc_q16 <<< 1)) :
                                      ((VOLT_TO_PWM_K == 15) ?
                                      ((clamped_acc_q16 <<< 4) - clamped_acc_q16) :
                                      (clamped_acc_q16 * VOLT_TO_PWM_K));
    wire signed [31:0] raw_pwm      = pwm_mult_acc >>> 16;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pwm_swing_duty <= 16'sd0;
            energy_deficit <= 1'b1;
            calc_done      <= 1'b0;
        end else if (pipe_valid3) begin
            energy_deficit <= energy_deficit_r3;
            if (raw_pwm > PWM_MAX_SWING)
                pwm_swing_duty <= PWM_MAX_SWING[15:0];
            else if (raw_pwm < -PWM_MAX_SWING)
                pwm_swing_duty <= -PWM_MAX_SWING[15:0];
            else
                pwm_swing_duty <= raw_pwm[15:0];
            calc_done <= 1'b1;
        end else begin
            calc_done <= 1'b0;
        end
    end

endmodule
