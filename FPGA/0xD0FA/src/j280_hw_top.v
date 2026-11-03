module j280_hw_top #(
    parameter integer CLK_FREQ_HZ           = 50_000_000,
    parameter integer TIMER_1MS_LIMIT       = 50_000_000 / 1000,
    parameter integer CALIB_DEBOUNCE_CYCLES = 1_000_000,
    parameter signed [31:0] ARM_SOFT_LIMIT_Q16 = 32'sd823548,


    parameter integer INVERT_BAR_DIR        = 0,

    parameter integer REVERSE_ARM_DIR       = 0,


    parameter integer CPR                   = 1040,


    parameter integer CTRL_MODE_PID        = 1
)(

    input  wire                   clk_50m,
    input  wire                   rst_n,


    input  wire                   key_zero_calib,
    input  wire                   key_pos_clear,
    input  wire                   key_motor_en,
    input  wire                   key_brake_mode,


    output wire                   motor_pwm,
    output wire                   motor_in1,
    output wire                   motor_in2,
    output wire                   motor_pwm_b,
    output wire                   motor_in1_b,
    output wire                   motor_in2_b,


    input  wire                   enc_a,
    input  wire                   enc_b,


    output wire                   adc_clk,
    output wire                   adc_oe_n,
    input  wire [9:0]             adc_data_in,


    output wire                   led_balance,
    output wire                   led_calib_ok,
    output wire                   led_motor_run,
    output wire                   led_enable,


    output wire                   uart_tx
);


    reg [23:0] timer_1ms_cnt;
    reg        calc_en_pulse;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            timer_1ms_cnt <= 24'd0;
            calc_en_pulse <= 1'b0;
        end else begin
            if (timer_1ms_cnt >= TIMER_1MS_LIMIT - 1) begin
                timer_1ms_cnt <= 24'd0;
                calc_en_pulse <= 1'b1;
            end else begin
                timer_1ms_cnt <= timer_1ms_cnt + 1'b1;
                calc_en_pulse <= 1'b0;
            end
        end
    end


    reg [24:0] blink_cnt;
    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n)
            blink_cnt <= 25'd0;
        else
            blink_cnt <= blink_cnt + 1'b1;
    end
    wire blink_1hz  = blink_cnt[24];
    wire blink_4hz  = blink_cnt[22];
    wire blink_10hz = blink_cnt[21];


    wire [11:0] raw_adc_12bit;
    wire        adc_raw_valid;

    adc_3pa1030_reader #(
        .CLK_FREQ_HZ (50_000_000),
        .ADC_DIV     (4),
        .AVG_BITS    (12)
    ) u_adc_reader (
        .clk         (clk_50m),
        .rst_n       (rst_n),
        .calc_en     (calc_en_pulse),
        .adc_clk     (adc_clk),
        .adc_oe_n    (adc_oe_n),
        .adc_data_in (adc_data_in),
        .raw_12bit   (raw_adc_12bit),
        .raw_valid   (adc_raw_valid)
    );


    reg [23:0] calib_debounce_cnt;
    reg        calib_pressed;
    reg [11:0] zero_offset_reg;
    reg        zero_calibrated;
    reg        adc_has_sampled;

    wire [11:0] raw_adc_data;
    wire        sample_done;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            adc_has_sampled <= 1'b0;
        end else if (sample_done) begin
            adc_has_sampled <= 1'b1;
        end
    end

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            calib_debounce_cnt <= 24'd0;
            calib_pressed      <= 1'b0;
            zero_offset_reg    <= 12'd2048;
            zero_calibrated    <= 1'b0;
        end else begin
            if (key_zero_calib == 1'b0) begin
                if (calib_debounce_cnt < CALIB_DEBOUNCE_CYCLES) begin
                    calib_debounce_cnt <= calib_debounce_cnt + 1'b1;
                end else if (!calib_pressed && adc_has_sampled) begin

                    zero_offset_reg <= raw_adc_data;
                    calib_pressed   <= 1'b1;
                    zero_calibrated <= 1'b1;
                end
            end else begin
                calib_debounce_cnt <= 24'd0;
                calib_pressed      <= 1'b0;
            end
        end
    end


    reg [25:0] key2_cnt;
    reg        clear_integral_pulse;
    reg [2:0]  traj_mode;
    reg        arm_zero_req;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            key2_cnt             <= 26'd0;
            clear_integral_pulse <= 1'b0;
            traj_mode            <= 3'd0;
        end else begin
            clear_integral_pulse <= 1'b0;
            if (arm_zero_req) traj_mode <= 3'd0;
            if (key_pos_clear == 1'b0) begin
                if (key2_cnt < 26'd50_000_000) begin
                    key2_cnt <= key2_cnt + 1'b1;
                end

                if (key2_cnt == CALIB_DEBOUNCE_CYCLES * 35) begin
                    clear_integral_pulse <= 1'b1;
                end
            end else begin

                if (key2_cnt >= CALIB_DEBOUNCE_CYCLES && key2_cnt < CALIB_DEBOUNCE_CYCLES * 35) begin
                    traj_mode <= (traj_mode >= 3'd5) ? 3'd0 : (traj_mode + 3'd1);
                end
                key2_cnt <= 26'd0;
            end
        end
    end


    reg [23:0] db_cnt3, db_cnt4;
    reg        key3_stable, key4_stable;
    reg        key4_prev_stable;
    reg [25:0] key4_hold_cnt;
    reg        key4_long_fired;
    reg        clear_arm_zero_pulse;
    reg        sw_motor_en;
    reg        sw_brake_mode;

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            db_cnt3     <= 24'd0;
            key3_stable <= 1'b1;
            sw_motor_en <= 1'b0;
        end else begin
            if (key_motor_en == key3_stable) begin
                db_cnt3 <= 24'd0;
            end else if (db_cnt3 >= CALIB_DEBOUNCE_CYCLES) begin
                key3_stable <= key_motor_en;
                db_cnt3     <= 24'd0;
                if (key_motor_en == 1'b0)
                    sw_motor_en <= ~sw_motor_en;
            end else begin
                db_cnt3 <= db_cnt3 + 1'b1;
            end
        end
    end

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            db_cnt4            <= 24'd0;
            key4_stable        <= 1'b1;
            key4_prev_stable   <= 1'b1;
            key4_hold_cnt      <= 26'd0;
            key4_long_fired    <= 1'b0;
            arm_zero_req       <= 1'b0;
            clear_arm_zero_pulse <= 1'b0;
            sw_brake_mode      <= 1'b0;
        end else begin
            clear_arm_zero_pulse <= 1'b0;
            key4_prev_stable     <= key4_stable;


            if (arm_zero_req && sample_done) begin
                clear_arm_zero_pulse <= 1'b1;
                arm_zero_req         <= 1'b0;
            end


            if (key_brake_mode == key4_stable) begin
                db_cnt4 <= 24'd0;
            end else if (db_cnt4 >= CALIB_DEBOUNCE_CYCLES) begin
                key4_stable <= key_brake_mode;
                db_cnt4     <= 24'd0;
            end else begin
                db_cnt4 <= db_cnt4 + 1'b1;
            end

            if (!key4_stable) begin

                if (key4_hold_cnt < 26'd50_000_000) begin
                    key4_hold_cnt <= key4_hold_cnt + 1'b1;
                end
                if ((key4_hold_cnt == CALIB_DEBOUNCE_CYCLES * 35) && !key4_long_fired) begin
                    arm_zero_req    <= 1'b1;
                    key4_long_fired <= 1'b1;
                end
            end else begin

                if (!key4_prev_stable) begin
                    if (!key4_long_fired && (key4_hold_cnt >= CALIB_DEBOUNCE_CYCLES)) begin
                        sw_brake_mode <= ~sw_brake_mode;
                    end
                    key4_hold_cnt   <= 26'd0;
                    key4_long_fired <= 1'b0;
                end
            end
        end
    end


    wire signed [31:0] theta_err_q16;
    wire signed [31:0] dtheta_q16;

    angle_sensor_reader #(
        .CLK_FREQ_HZ   (50_000_000),
        .SPI_SCLK_HZ   (2_500_000),
        .ADC_RESOLUTION(12),
        .INVERT_DIR    (INVERT_BAR_DIR),
        .USE_SPI       (0)
    ) u_angle_sensor (
        .clk            (clk_50m),
        .rst_n          (rst_n),
        .calc_en        (calc_en_pulse),
        .spi_cs_n       (),
        .spi_sclk       (),
        .spi_miso       (1'b0),
        .zero_offset_raw(zero_offset_reg),
        .ext_raw_valid  (adc_raw_valid),
        .ext_raw_data   (raw_adc_12bit),
        .raw_adc_data   (raw_adc_data),
        .theta_err_q16  (theta_err_q16),
        .dtheta_q16     (dtheta_q16),
        .sample_done    (sample_done)
    );


    wire signed [31:0] pulse_count;
    wire signed [31:0] alpha_rad_q16;
    wire signed [31:0] dalpha_rad_s_q16;

    encoder_quad_reader #(
        .CLK_FREQ_HZ   (50_000_000),
        .FILTER_CYCLES (8),
        .CPR           (CPR),
        .USE_Z_INDEX   (0),
        .REVERSE_DIR   (REVERSE_ARM_DIR)
    ) u_encoder (
        .clk            (clk_50m),
        .rst_n          (rst_n),
        .enc_a_raw      (enc_a),
        .enc_b_raw      (enc_b),
        .enc_z_raw      (1'b0),
        .clear_pos      (clear_arm_zero_pulse),
        .calc_en        (sample_done),
        .pulse_count    (pulse_count),
        .alpha_rad_q16  (alpha_rad_q16),
        .dalpha_rad_s_q16(dalpha_rad_s_q16),
        .speed_pps      (),
        .dir_flag       ()
    );


    wire soft_limit_err = (alpha_rad_q16 > ARM_SOFT_LIMIT_Q16) || (alpha_rad_q16 < -ARM_SOFT_LIMIT_Q16);


    wire signed [31:0] alpha_ref_q16;
    wire signed [31:0] dalpha_ref_q16;
    wire               traj_sync_pulse;
    wire               traj_moving;
    wire               lqr_en;


    pos_traj_gen #(
        .VMAX_Q16  (32768),
        .VSOFT_Q16 (6554),
        .RAMP_Q16  (12),
        .ASTEP_Q16 (131),
        .SINE_INC  (839)
    ) u_pos_traj_gen (
        .clk            (clk_50m),
        .rst_n          (rst_n),
        .calc_en        (sample_done),
        .sync           (traj_sync_pulse),
        .rezero         (clear_arm_zero_pulse),
        .enable         (lqr_en),
        .mode_sel       (traj_mode),
        .alpha_meas_q16 (alpha_rad_q16),
        .alpha_ref_q16  (alpha_ref_q16),
        .dalpha_ref_q16 (dalpha_ref_q16),
        .moving         (traj_moving)
    );


    wire signed [31:0] alpha_err_q16  = alpha_rad_q16 - alpha_ref_q16;
    wire signed [31:0] dalpha_err_q16 = dalpha_rad_s_q16 - dalpha_ref_q16;


    wire signed [31:0] balance_alpha_err_q16 = alpha_err_q16;


    wire [1:0]         fsm_state;
    wire               swing_en;
    wire               fsm_motor_active;
    wire               reset_integral_pulse;
    wire               is_in_balance_zone;
    wire signed [15:0] final_pwm_duty;

    wire signed [15:0] swing_pwm_duty;
    wire               swing_calc_done;
    wire               swing_energy_deficit;

    wire signed [15:0] lqr_pwm_duty;
    wire               lqr_calc_done;


    localparam signed [31:0] INT_LIMIT_Q16      = 32'sd16384;

    localparam signed [31:0] INT_ACCUM_LIMIT    = 32'sd16393000;


    localparam signed [31:0] INT_ERR_THRESH_Q16 = 32'sd1258;
    reg signed [31:0] alpha_int_accum;

    wire int_accum_en = lqr_en && (alpha_err_q16 > -INT_ERR_THRESH_Q16) && (alpha_err_q16 < INT_ERR_THRESH_Q16);

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            alpha_int_accum <= 32'sd0;
        end else if (!fsm_motor_active || !lqr_en || reset_integral_pulse || soft_limit_err) begin
            alpha_int_accum <= 32'sd0;
        end else if (sample_done) begin
            if (int_accum_en) begin
                if (alpha_int_accum + alpha_err_q16 > INT_ACCUM_LIMIT)
                    alpha_int_accum <= INT_ACCUM_LIMIT;
                else if (alpha_int_accum + alpha_err_q16 < -INT_ACCUM_LIMIT)
                    alpha_int_accum <= -INT_ACCUM_LIMIT;
                else
                    alpha_int_accum <= alpha_int_accum + alpha_err_q16;
            end else begin

                alpha_int_accum <= alpha_int_accum;
            end
        end
    end


    wire signed [33:0] accum_ext = {{2{alpha_int_accum[31]}}, alpha_int_accum};
    wire signed [33:0] int_x131 = (accum_ext <<< 7) + (accum_ext <<< 1) + accum_ext;
    wire signed [33:0] int_rounded = int_x131 + 34'sd65536;
    wire signed [31:0] alpha_int_scaled = int_rounded >>> 17;
    wire signed [31:0] alpha_int_q16 = (alpha_int_scaled > INT_LIMIT_Q16)  ? INT_LIMIT_Q16 :
                                       (alpha_int_scaled < -INT_LIMIT_Q16) ? -INT_LIMIT_Q16 :
                                       alpha_int_scaled;


    localparam signed [31:0] PID_INT_GATE_Q16  = 32'sd36045;
    localparam signed [31:0] PID_INT_CLAMP     = 32'sd268435456;
    reg  signed [31:0] pid_alpha_int_q16ms;

    wire pid_int_accum_en = (CTRL_MODE_PID == 1) && lqr_en && !traj_moving &&
                            (balance_alpha_err_q16 > -PID_INT_GATE_Q16) && (balance_alpha_err_q16 < PID_INT_GATE_Q16);

    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            pid_alpha_int_q16ms <= 32'sd0;
        end else if (!fsm_motor_active || !lqr_en || reset_integral_pulse || soft_limit_err ||
                     clear_integral_pulse || clear_arm_zero_pulse) begin

            pid_alpha_int_q16ms <= 32'sd0;
        end else if (sample_done && pid_int_accum_en) begin
            if (pid_alpha_int_q16ms + balance_alpha_err_q16 > PID_INT_CLAMP)
                pid_alpha_int_q16ms <= PID_INT_CLAMP;
            else if (pid_alpha_int_q16ms + balance_alpha_err_q16 < -PID_INT_CLAMP)
                pid_alpha_int_q16ms <= -PID_INT_CLAMP;
            else
                pid_alpha_int_q16ms <= pid_alpha_int_q16ms + balance_alpha_err_q16;
        end
    end

    wire signed [31:0] pid_int_term_q16 = pid_alpha_int_q16ms >>> 13;


    swing_up_ctrl u_swing_ctrl (
        .clk             (clk_50m),
        .rst_n           (rst_n),
        .calc_en         (sample_done),
        .swing_active    (swing_en),
        .theta_rad_q16   (theta_err_q16),
        .dtheta_rad_s_q16(dtheta_q16),
        .alpha_rad_q16   (alpha_rad_q16),
        .dalpha_rad_s_q16(dalpha_rad_s_q16),
        .pwm_swing_duty  (swing_pwm_duty),
        .energy_deficit  (swing_energy_deficit),
        .calc_done       (swing_calc_done)
    );


    reg [9:0] catch_cnt;
    always @(posedge clk_50m or negedge rst_n) begin
        if (!rst_n) begin
            catch_cnt <= 10'd1023;
        end else if (reset_integral_pulse) begin
            catch_cnt <= 10'd0;
        end else if (sample_done && (catch_cnt < 10'd1023)) begin
            catch_cnt <= catch_cnt + 10'd1;
        end
    end


    reg signed [31:0] lqr_alpha_err_q16;
    always @(*) begin
        case (catch_cnt[9:8])
            2'b00:   lqr_alpha_err_q16 = balance_alpha_err_q16 >>> 2;
            2'b01:   lqr_alpha_err_q16 = balance_alpha_err_q16 >>> 1;
            2'b10:   lqr_alpha_err_q16 = (balance_alpha_err_q16 >>> 1) + (balance_alpha_err_q16 >>> 2);
            default: lqr_alpha_err_q16 = balance_alpha_err_q16;
        endcase
    end


    reg signed [31:0] lqr_dalpha_q16;
    always @(*) begin
        case (catch_cnt[9:8])
            2'b00:   lqr_dalpha_q16 = dalpha_err_q16 >>> 2;
            2'b01:   lqr_dalpha_q16 = dalpha_err_q16 >>> 1;
            2'b10:   lqr_dalpha_q16 = (dalpha_err_q16 >>> 1) + (dalpha_err_q16 >>> 2);
            default: lqr_dalpha_q16 = dalpha_err_q16;
        endcase
    end

    generate if (CTRL_MODE_PID == 0) begin : g_lqr
        furuta_lqr_ctrl u_balance_core (
            .clk        (clk_50m),
            .rst_n      (rst_n),
            .calc_en    (sample_done),
            .theta_err  (theta_err_q16),
            .dtheta     (dtheta_q16),
            .alpha_err  (lqr_alpha_err_q16),
            .dalpha     (dalpha_err_q16),
            .alpha_int  (alpha_int_q16),
            .pwm_duty   (lqr_pwm_duty),
            .calc_done  (lqr_calc_done)
        );
    end else begin : g_pid

        furuta_pid_ctrl u_balance_core (
            .clk        (clk_50m),
            .rst_n      (rst_n),
            .calc_en    (sample_done),
            .sync       (reset_integral_pulse),
            .theta_err  (theta_err_q16),
            .alpha_err  (lqr_alpha_err_q16 + pid_int_term_q16),
            .dalpha     (lqr_dalpha_q16),
            .pwm_duty   (lqr_pwm_duty),
            .calc_done  (lqr_calc_done)
        );
    end
    endgenerate


    ctrl_fsm u_ctrl_fsm (
        .clk                 (clk_50m),
        .rst_n               (rst_n),
        .calc_en             (sample_done),
        .motor_en_sw         (sw_motor_en),
        .calib_done          (zero_calibrated),
        .soft_limit_err      (soft_limit_err),
        .theta_err_q16       (theta_err_q16),
        .dtheta_q16          (dtheta_q16),
        .dalpha_q16          (dalpha_rad_s_q16),
        .pwm_swing_duty      (swing_pwm_duty),
        .pwm_lqr_duty        (lqr_pwm_duty),
        .lqr_calc_done       (lqr_calc_done),
        .swing_calc_done     (swing_calc_done),
        .current_state       (fsm_state),
        .final_pwm_duty      (final_pwm_duty),
        .reset_integral_pulse(reset_integral_pulse),
        .traj_sync_pulse     (traj_sync_pulse),
        .is_in_balance_zone  (is_in_balance_zone),
        .lqr_en              (lqr_en),
        .swing_en            (swing_en),
        .motor_active        (fsm_motor_active)
    );


    motor_pwm_driver #(
        .CLK_FREQ_HZ   (50_000_000),
        .PWM_FREQ_HZ   (19_500),


        .DUTY_MAX_VAL  (1000),
        .DEAD_BAND_VAL (0)
    ) u_motor_driver (
        .clk        (clk_50m),
        .rst_n      (rst_n),
        .motor_en   (fsm_motor_active),
        .brake_mode (sw_brake_mode),
        .pwm_duty   (final_pwm_duty),
        .pwm_out    (motor_pwm),
        .dir_out    (),
        .in1_out    (motor_in1),
        .in2_out    (motor_in2),
        .stby_out   ()
    );


    assign motor_pwm_b = motor_pwm;
    assign motor_in1_b = motor_in1;
    assign motor_in2_b = motor_in2;


    uart_telemetry #(
        .CLK_FREQ_HZ (50_000_000),
        .BAUD        (115_200),
        .PERIOD_MS   (10)
    ) u_telemetry (
        .clk            (clk_50m),
        .rst_n          (rst_n),
        .theta_err_q16  (theta_err_q16),
        .dtheta_q16     (dtheta_q16),
        .alpha_q16      (alpha_rad_q16),
        .dalpha_q16     (dalpha_rad_s_q16),
        .duty           (final_pwm_duty),
        .fsm_state      (fsm_state),
        .lqr_en         (lqr_en),
        .swing_en       (swing_en),
        .motor_active   (fsm_motor_active),
        .energy_deficit (swing_energy_deficit),
        .calib_done     (zero_calibrated),
        .traj_moving    (traj_moving),
        .uart_tx        (uart_tx)
    );


    assign led_balance   = (fsm_state == 2'd2) ? 1'b1 :
                           (fsm_state == 2'd1) ? blink_4hz : 1'b0;


    assign led_calib_ok  = zero_calibrated ? 1'b1 : blink_1hz;


    assign led_motor_run = (fsm_state == 2'd3) ? blink_10hz :
                           (fsm_motor_active && (final_pwm_duty != 16'sd0)) ? 1'b1 : 1'b0;


    assign led_enable    = sw_motor_en;


endmodule
