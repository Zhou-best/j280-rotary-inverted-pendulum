`timescale 1ns / 1ps

module furuta_hil_tb;


    localparam integer SWING_MAX     = 5000;


    localparam integer BAL_HOLD_PLAN = 1000;
    localparam integer DIST_PLAN     = 1100;


    localparam integer TRAJ_PLAN     = 5000;
    localparam integer RECOVER_PLAN  = 2000;
    localparam integer DIST_PUSH_MS  = 50;
    localparam integer DIST_TAU_uNm  = 40000;
    localparam integer TAIL_AVG_MS   = 200;
    localparam integer TAIL_SETTLE_MS= 3600;


    localparam integer SINE_PLAN     = 5000;
    localparam real    SINE_TH_LIMIT = 8.0;
    localparam real    SINE_RMS_LIMIT= 10.0;

    localparam integer CAPTURE_TH_Q16= 17188;

    localparam real RAD2DEG          = 57.29577951308232087680;
    localparam real BAL_ANG_LIMIT    = 22.0;
    localparam real DIST_DEF_LIMIT   = 15.0;
    localparam real DIST_REC_TH      = 0.5;
    localparam integer DIST_REC_LIMIT= 800;


    localparam real TRAJ_POS_LIMIT   = 0.20;
    localparam real TRAJ_TH_SS_LIMIT = 0.5;
    localparam real TRAJ_TH_LIMIT    = 22.0;

    localparam integer PI_Q16        = 205887;
    localparam integer CALIB_DEB     = 100;
    localparam integer KEY_PRESS_NS  = 20000;
    localparam integer KEY_LONG_NS   = 100000;


    localparam real DTH_SPIKE_RAD_S  = 100.0;


    reg  clk_50m;
    reg  rst_n;
    reg  sw_motor_en;
    reg  sw_brake_mode;
    reg  key_zero_calib;
    reg  key_pos_clear;
    reg  enc_z;

    wire motor_pwm;
    wire motor_dir;
    wire motor_in1;
    wire motor_in2;
    wire motor_stby;

    wire enc_a;
    wire enc_b;

    wire adc_cs_n;
    wire adc_sclk;
    reg  adc_miso;

    wire led_balance;
    wire led_calib_ok;
    wire led_motor_run;


    j280_hw_top #(
        .CLK_FREQ_HZ          (50_000_000),
        .TIMER_1MS_LIMIT      (50_000),
        .CALIB_DEBOUNCE_CYCLES(CALIB_DEB)

    ) u_dut (
        .clk_50m       (clk_50m),
        .rst_n         (rst_n),
        .sw_motor_en   (sw_motor_en),
        .sw_brake_mode (sw_brake_mode),
        .key_zero_calib(key_zero_calib),
        .key_pos_clear (key_pos_clear),
        .motor_pwm     (motor_pwm),
        .motor_dir     (motor_dir),
        .motor_in1     (motor_in1),
        .motor_in2     (motor_in2),
        .motor_stby    (motor_stby),
        .enc_a         (enc_a),
        .enc_b         (enc_b),
        .enc_z         (enc_z),
        .adc_cs_n      (adc_cs_n),
        .adc_sclk      (adc_sclk),
        .adc_miso      (adc_miso),
        .led_balance   (led_balance),
        .led_calib_ok  (led_calib_ok),
        .led_motor_run (led_motor_run)
    );


    reg                state_load;
    reg  signed [31:0] init_alpha_q16;
    reg  signed [31:0] init_dalpha_q16;
    reg  signed [31:0] init_theta_q16;
    reg  signed [31:0] init_dtheta_q16;
    reg  signed [31:0] disturb_pend_uNm;
    reg  signed [31:0] disturb_arm_uNm;
    reg  signed [31:0] sensor_noise_urad;

    wire [11:0]        plant_raw_adc;
    wire signed [31:0] plant_theta_q16;
    wire signed [31:0] plant_dtheta_q16;
    wire signed [31:0] plant_alpha_q16;
    wire signed [31:0] plant_dalpha_q16;
    wire signed [31:0] plant_vmotor_mv;
    wire signed [31:0] plant_energy_mj;
    wire signed [31:0] plant_rk4_steps;
    wire               plant_phys_step;
    wire               plant_ctrl_tick;
    wire               plant_diverged;

    furuta_plant_model #(
        .CLK_FREQ_HZ    (50_000_000),
        .PHYS_RATE_HZ   (5_000),
        .CTRL_RATE_HZ   (1_000),
        .OUT_REFRESH_DIV(100),
        .ADC_ZERO_CODE  (2048),
        .ADC_BITS       (12),
        .ENC_CPR        (4000),
        .V_SUPPLY       (12.0)
    ) u_plant (
        .clk                  (clk_50m),
        .rst_n                (rst_n),
        .motor_in1            (motor_in1),
        .motor_in2            (motor_in2),
        .motor_stby           (motor_stby),
        .disturb_tau_pend_uNm (disturb_pend_uNm),
        .disturb_tau_arm_uNm  (disturb_arm_uNm),
        .sensor_noise_urad    (sensor_noise_urad),
        .state_load           (state_load),
        .init_alpha_q16       (init_alpha_q16),
        .init_dalpha_q16      (init_dalpha_q16),
        .init_theta_q16       (init_theta_q16),
        .init_dtheta_q16      (init_dtheta_q16),
        .raw_adc              (plant_raw_adc),
        .enc_a                (enc_a),
        .enc_b                (enc_b),
        .theta_q16            (plant_theta_q16),
        .dtheta_q16           (plant_dtheta_q16),
        .alpha_q16            (plant_alpha_q16),
        .dalpha_q16           (plant_dalpha_q16),
        .v_motor_mV           (plant_vmotor_mv),
        .energy_rel_mJ        (plant_energy_mj),
        .phys_step            (plant_phys_step),
        .ctrl_tick            (plant_ctrl_tick),
        .diverged             (plant_diverged),
        .rk4_step_count       (plant_rk4_steps)
    );


    reg [15:0] spi_tx_word;
    reg [15:0] spi_slave_shift;

    always @(negedge adc_cs_n) begin
        spi_tx_word     = {4'b0000, plant_raw_adc};
        adc_miso        <= spi_tx_word[15];
        spi_slave_shift <= {spi_tx_word[14:0], 1'b0};
    end

    always @(negedge adc_sclk) begin
        if (!adc_cs_n) begin
            adc_miso        <= spi_slave_shift[15];
            spi_slave_shift <= {spi_slave_shift[14:0], 1'b0};
        end
    end


    initial begin
        clk_50m = 1'b0;
        forever #10 clk_50m = ~clk_50m;
    end


    reg     mon_en;
    integer tick_idx;
    integer trace_div;
    reg [1:0] mon_prev_state;
    reg [1:0] mon_state_now;


    integer dtheta_spike_n;
    real    dtheta_spike_peak;
    reg     spike_active;
    real    raw_dth_now, abs_raw_dth;

    integer catch_tick;
    integer catch_count;
    integer fall_count;
    integer balance_ticks;
    integer protect_ticks;
    integer softlimit_ticks;
    integer max_abs_pwm;
    integer max_abs_pwm_bal;
    integer swing_pwm_sum;
    integer swing_pwm_n;
    real    min_abs_th_swing_deg;
    real    max_abs_th_bal_deg;
    real    max_abs_th_all_deg;
    real    max_abs_alpha_deg;
    integer min_energy_mj;
    integer max_energy_mj;


    real    th_deg_now, al_deg_now, dth_now, dal_now, abs_th_now, abs_al_now;
    integer abs_pwm_now;


    reg     dist_win_open;
    integer dist_push_end_tick;
    real    dist_max_deg;
    integer dist_recover_ms;


    reg     traj_win_open;
    integer traj_start_tick;
    integer traj_tail_start;
    real    traj_max_deg;
    real    traj_max_pos_err_deg;
    real    traj_final_pos_err_deg;
    real    traj_pos_err_now;
    real    traj_ss_pos_err_deg;
    real    traj_ss_th_deg;
    real    traj_ss_sum_pos, traj_ss_sum_th;
    integer traj_ss_cnt;


    reg     sine_win_open;
    real    sine_pos_err_now;
    real    sine_max_pos_err_deg;
    real    sine_max_deg;
    real    sine_ss_sum_sq;
    integer sine_ss_cnt;
    real    sine_rms_deg;


    real    sync_exp_pulse_r;
    integer sync_exp_pulse;
    integer sync_pulse_err;
    integer sync_th_diff;


    integer sync_pulse_peak    = 0;
    integer sync_desync_n      = 0;
    integer sync_th_peak       = 0;
    integer sync_th_desync_n   = 0;
    localparam integer SYNC_PULSE_TOL  = 2;
    localparam integer SYNC_TH_Q16_TOL = 20000;
    integer ramp_guard;
    integer settle_guard;


    integer theta_wrap_n       = 0;
    integer mon_prev_th_q16    = 0;


    always @(posedge clk_50m) begin

        if (rst_n === 1'b1 && mon_en === 1'b1) begin
            raw_dth_now = u_dut.u_angle_sensor.raw_dtheta_q16 / 65536.0;
            abs_raw_dth = (raw_dth_now < 0.0) ? -raw_dth_now : raw_dth_now;
            if (abs_raw_dth > DTH_SPIKE_RAD_S) begin
                if (spike_active !== 1'b1) begin
                    dtheta_spike_n = dtheta_spike_n + 1;
                    spike_active   = 1'b1;
                end
                if (abs_raw_dth > dtheta_spike_peak) dtheta_spike_peak = abs_raw_dth;
            end else begin
                spike_active = 1'b0;
            end
        end

        if (rst_n === 1'b1 && plant_ctrl_tick === 1'b1 && mon_en === 1'b1) begin
            tick_idx = tick_idx + 1;

            th_deg_now  = plant_theta_q16  * (RAD2DEG / 65536.0);
            al_deg_now  = plant_alpha_q16  * (RAD2DEG / 65536.0);
            dth_now     = plant_dtheta_q16 / 65536.0;
            dal_now     = plant_dalpha_q16 / 65536.0;
            abs_th_now  = (th_deg_now < 0.0) ? -th_deg_now : th_deg_now;
            abs_al_now  = (al_deg_now < 0.0) ? -al_deg_now : al_deg_now;
            abs_pwm_now = (u_dut.final_pwm_duty < 16'sd0) ? -u_dut.final_pwm_duty
                                                          :  u_dut.final_pwm_duty;

            if (abs_th_now > max_abs_th_all_deg) max_abs_th_all_deg = abs_th_now;
            if (abs_al_now > max_abs_alpha_deg)  max_abs_alpha_deg  = abs_al_now;
            if (abs_pwm_now > max_abs_pwm)       max_abs_pwm        = abs_pwm_now;
            if (plant_energy_mj < min_energy_mj) min_energy_mj      = plant_energy_mj;
            if (plant_energy_mj > max_energy_mj) max_energy_mj      = plant_energy_mj;

            case (u_dut.fsm_state)
                2'd1: begin
                    if (abs_th_now < min_abs_th_swing_deg) min_abs_th_swing_deg = abs_th_now;
                    swing_pwm_sum = swing_pwm_sum + abs_pwm_now;
                    swing_pwm_n   = swing_pwm_n + 1;
                end
                2'd2: begin
                    balance_ticks = balance_ticks + 1;
                    if (abs_th_now  > max_abs_th_bal_deg) max_abs_th_bal_deg = abs_th_now;
                    if (abs_pwm_now > max_abs_pwm_bal)    max_abs_pwm_bal    = abs_pwm_now;
                end
                2'd3: protect_ticks = protect_ticks + 1;
                default: ;
            endcase


            if (mon_prev_state != u_dut.fsm_state) begin
                $display("  [FSM] t=%6d ms  state %0d -> %0d  (th=%9.3f deg, dth=%8.3f rad/s, al=%9.3f deg, pwm=%5d)",
                         tick_idx, mon_prev_state, u_dut.fsm_state,
                         th_deg_now, dth_now, al_deg_now, u_dut.final_pwm_duty);
            end
            if (mon_prev_state != 2'd2 && u_dut.fsm_state == 2'd2) begin
                catch_count = catch_count + 1;
                if (catch_tick < 0) catch_tick = tick_idx;
            end
            if (mon_prev_state == 2'd2 && u_dut.fsm_state == 2'd1) fall_count = fall_count + 1;
            mon_prev_state = u_dut.fsm_state;
            mon_state_now  = u_dut.fsm_state;

            if (u_dut.soft_limit_err === 1'b1) softlimit_ticks = softlimit_ticks + 1;


            if ((mon_prev_th_q16 > 150000) && (plant_theta_q16 < -150000))
                theta_wrap_n = theta_wrap_n + 1;
            if ((mon_prev_th_q16 < -150000) && (plant_theta_q16 > 150000))
                theta_wrap_n = theta_wrap_n + 1;
            mon_prev_th_q16 = plant_theta_q16;


            sync_exp_pulse_r = plant_alpha_q16 / 65536.0 * 636.61977;
            sync_exp_pulse   = (sync_exp_pulse_r >= 0.0) ?  $rtoi(sync_exp_pulse_r + 0.5)
                                                        : -$rtoi(-sync_exp_pulse_r + 0.5);
            sync_pulse_err   = u_dut.pulse_count - sync_exp_pulse;
            if (sync_pulse_err < 0) sync_pulse_err = -sync_pulse_err;
            if (sync_pulse_err > SYNC_PULSE_TOL) begin
                sync_desync_n = sync_desync_n + 1;
                if (sync_pulse_err > sync_pulse_peak) sync_pulse_peak = sync_pulse_err;
            end


            sync_th_diff = plant_theta_q16 - u_dut.theta_err_q16;
            if (sync_th_diff >  205887) sync_th_diff = sync_th_diff - 411775;
            if (sync_th_diff < -205888) sync_th_diff = sync_th_diff + 411775;
            if (sync_th_diff < 0) sync_th_diff = -sync_th_diff;
            if (sync_th_diff > SYNC_TH_Q16_TOL) begin
                sync_th_desync_n = sync_th_desync_n + 1;
                if (sync_th_diff > sync_th_peak) sync_th_peak = sync_th_diff;
            end


            if (dist_win_open === 1'b1) begin
                if (abs_th_now > dist_max_deg) dist_max_deg = abs_th_now;
                if (dist_recover_ms < 0 && tick_idx > dist_push_end_tick &&
                    abs_th_now < DIST_REC_TH && u_dut.fsm_state == 2'd2)
                    dist_recover_ms = tick_idx - dist_push_end_tick;
            end


            if (traj_win_open === 1'b1) begin
                traj_pos_err_now = (plant_alpha_q16 - u_dut.alpha_ref_q16) * (RAD2DEG / 65536.0);
                if (traj_pos_err_now < 0.0) traj_pos_err_now = -traj_pos_err_now;
                if (traj_pos_err_now > traj_max_pos_err_deg) traj_max_pos_err_deg = traj_pos_err_now;
                traj_final_pos_err_deg = traj_pos_err_now;
                if (abs_th_now > traj_max_deg) traj_max_deg = abs_th_now;
                if (tick_idx >= traj_tail_start) begin
                    traj_ss_sum_pos = traj_ss_sum_pos + traj_pos_err_now;
                    traj_ss_sum_th  = traj_ss_sum_th  + abs_th_now;
                    traj_ss_cnt     = traj_ss_cnt + 1;
                end
            end


            if (sine_win_open === 1'b1) begin
                sine_pos_err_now = (plant_alpha_q16 - u_dut.alpha_ref_q16) * (RAD2DEG / 65536.0);
                if (sine_pos_err_now < 0.0) sine_pos_err_now = -sine_pos_err_now;
                if (sine_pos_err_now > sine_max_pos_err_deg) sine_max_pos_err_deg = sine_pos_err_now;
                if (abs_th_now > sine_max_deg) sine_max_deg = abs_th_now;
                sine_ss_sum_sq = sine_ss_sum_sq + (sine_pos_err_now * sine_pos_err_now);
                sine_ss_cnt    = sine_ss_cnt + 1;
            end


            if (trace_div > 0 && (tick_idx % trace_div) == 0) begin
                $display("  [TRACE] t=%6d ms st=%0d th=%9.3f deg dth=%8.3f rad/s al=%10.3f deg dal=%8.3f pwm=%5d V=%8.1f mV adc=%4d E=%7d mJ aref=%7d",
                         tick_idx, u_dut.fsm_state, th_deg_now, dth_now, al_deg_now, dal_now,
                         u_dut.final_pwm_duty, plant_vmotor_mv * 1.0, plant_raw_adc,
                         plant_energy_mj, u_dut.alpha_ref_q16);
            end
        end
    end


    task sync_ctrl_tick;
        begin
            @(posedge plant_ctrl_tick);
            @(posedge clk_50m);
            #1;
        end
    endtask

    task wait_ctrl_ticks;
        input integer n;
        integer k;
        begin
            for (k = 0; k < n; k = k + 1) sync_ctrl_tick;
        end
    endtask

    task reset_stats;
        begin
            tick_idx             = 0;
            mon_prev_state       = 2'd0;
            mon_state_now        = u_dut.fsm_state;
            dtheta_spike_n       = 0;
            dtheta_spike_peak    = 0.0;
            spike_active         = 1'b0;
            catch_tick           = -1;
            catch_count          = 0;
            fall_count           = 0;
            balance_ticks        = 0;
            protect_ticks        = 0;
            softlimit_ticks      = 0;
            max_abs_pwm          = 0;
            max_abs_pwm_bal      = 0;
            swing_pwm_sum        = 0;
            swing_pwm_n          = 0;
            min_abs_th_swing_deg = 1.0e9;
            max_abs_th_bal_deg   = 0.0;
            max_abs_th_all_deg   = 0.0;
            max_abs_alpha_deg    = 0.0;
            min_energy_mj        = 2000000000;
            max_energy_mj        = -2000000000;
            dist_win_open        = 1'b0;
            dist_push_end_tick   = 0;
            dist_max_deg         = 0.0;
            dist_recover_ms      = -1;
            traj_win_open        = 1'b0;
            traj_start_tick      = 0;
            traj_tail_start      = 1000000000;
            traj_max_deg         = 0.0;
            traj_max_pos_err_deg = 0.0;
            traj_final_pos_err_deg = 0.0;
            traj_ss_pos_err_deg  = 0.0;
            traj_ss_th_deg       = 0.0;
            traj_ss_sum_pos      = 0.0;
            traj_ss_sum_th       = 0.0;
            traj_ss_cnt          = 0;
            sine_win_open        = 1'b0;
            sine_max_deg         = 0.0;
            sine_max_pos_err_deg = 0.0;
            sine_ss_sum_sq       = 0.0;
            sine_ss_cnt          = 0;
            sine_rms_deg         = 0.0;
        end
    endtask


    integer hil_ctrl_cycles;
    integer arg_i;
    integer setup_ticks;
    integer ticks_used;
    integer avail, n_ticks, elapsed, bal_held;
    integer phase_start_tick;
    integer trace_div_save;
    integer test_a_pass, test_b_pass, test_c_pass, test_d_pass, test_e_pass, test_cal_pass;
    integer test_b_run, test_c_run, test_d_run, test_e_run, test_rec_run;
    integer test_alt_used;
    integer n_checks, n_fail;
    integer freeze_ticks;
    integer expected_rk4;
    real    swing_avg_pwm;
    real    wd_ns;


    initial begin

        hil_ctrl_cycles = 20000;
        if ($value$plusargs("HIL_CTRL_CYCLES=%d", arg_i)) hil_ctrl_cycles = arg_i;
        trace_div = 100;
        if ($value$plusargs("HIL_TRACE_DIV=%d", arg_i)) trace_div = arg_i;

        test_a_pass = 0; test_b_pass = 0; test_c_pass = 0; test_d_pass = 0; test_e_pass = 0; test_cal_pass = 0;
        test_b_run  = 0; test_c_run  = 0; test_d_run  = 0; test_e_run  = 0; test_rec_run = 0;
        test_alt_used = 0;
        n_checks    = 0; n_fail      = 0;
        setup_ticks = 0;  ticks_used  = 0; bal_held = 0;
        phase_start_tick = 0;
        freeze_ticks = 0; expected_rk4 = 0;

        $display("=================================================================");
        $display("   J280 旋转倒立摆 闭环硬件在环 (HIL) 仿真   F13-b");
        $display("   DUT   : j280_hw_top  (TIMER_1MS_LIMIT = 50000, 真实 1ms 控制周期)");
        $display("   PLANT : furuta_plant_model (RK4, dt_phys = 0.2ms, 5 步/控制周期)");
        $display("   预算  : %0d 个控制周期 = %0d.%03d 秒物理时间",
                 hil_ctrl_cycles, hil_ctrl_cycles/1000, hil_ctrl_cycles%1000);
        $display("   拉长  : vsim ... work.furuta_hil_tb +HIL_CTRL_CYCLES=65000 (65s 完整验收)");
        $display("=================================================================");


        rst_n          = 1'b0;
        sw_motor_en    = 1'b0;
        sw_brake_mode  = 1'b1;
        key_zero_calib = 1'b1;
        key_pos_clear  = 1'b1;
        enc_z          = 1'b0;
        adc_miso       = 1'b0;

        state_load        = 1'b1;
        init_alpha_q16    = 32'sd0;
        init_dalpha_q16   = 32'sd0;
        init_theta_q16    = 32'sd0;
        init_dtheta_q16   = 32'sd0;
        disturb_pend_uNm  = 32'sd0;
        disturb_arm_uNm   = 32'sd0;
        sensor_noise_urad = 32'sd0;

        reset_stats;
        mon_en = 1'b0;

        #200;
        rst_n = 1'b1;
        #40;


        $display("\n[PHASE 0] 摆杆置于倒立位 (theta=0, raw_adc=%0d), 执行一键零点标定...",
                 plant_raw_adc);
        wait_ctrl_ticks(3);
        setup_ticks = setup_ticks + 3;
        key_zero_calib = 1'b0;
        #KEY_PRESS_NS;
        key_zero_calib = 1'b1;
        wait_ctrl_ticks(2);
        setup_ticks = setup_ticks + 2;

        $display("  zero_offset_reg = %0d (期望 2048), zero_calibrated = %b, led_calib_ok = %b (0=点亮)",
                 u_dut.zero_offset_reg, u_dut.zero_calibrated, led_calib_ok);
        $display("  DUT 解算 theta_err_q16 = %0d (期望 ~0), dtheta_q16 = %0d",
                 u_dut.theta_err_q16, u_dut.dtheta_q16);
        n_checks = n_checks + 1;
        if (led_calib_ok === 1'b0 && u_dut.zero_calibrated === 1'b1 &&
            u_dut.zero_offset_reg === 12'd2048) begin
            test_cal_pass = 1;
            $display("  [PASS] 零点标定成功, ctrl_fsm 的 calib_done 前置条件已满足");
        end else begin
            n_fail = n_fail + 1;
            $display("  [FAIL] 零点标定失败 —— FSM 将永久停留在 HANGING, 后续测试无意义");
        end


        $display("\n[PHASE 0b] 将摆杆搬迁至自由下垂位 (theta = pi, raw_adc = 0) 并静置...");
        init_theta_q16 = PI_Q16;
        state_load     = 1'b1;
        wait_ctrl_ticks(60);
        setup_ticks = setup_ticks + 60;
        freeze_ticks = setup_ticks;
        state_load = 1'b0;
        mon_en     = 1'b1;
        wait_ctrl_ticks(10);
        setup_ticks = setup_ticks + 10;
        $display("  静置后: PLANT theta=%0d Q16, DUT theta_err_q16=%0d (期望 ~-205888 = -180deg), dtheta_q16=%0d",
                 plant_theta_q16, u_dut.theta_err_q16, u_dut.dtheta_q16);
        $display("  静置后: PLANT alpha=%0d Q16, DUT pulse_count=%0d, alpha_rad_q16=%0d (三者应一致为 ~0)",
                 plant_alpha_q16, u_dut.pulse_count, u_dut.alpha_rad_q16);
        $display("  静置后: fsm_state=%0d (期望 0=HANGING), RK4 累计步数=%0d (期望 ~%0d)",
                 u_dut.fsm_state, plant_rk4_steps, (setup_ticks - freeze_ticks) * 5);


        n_checks = n_checks + 1;
        if (u_dut.fsm_state !== 2'd0) begin
            n_fail = n_fail + 1;
            $display("  [FAIL] 电机未使能时 FSM 不在 HANGING 态!");
        end else begin
            $display("  [PASS] 待机态正确 (HANGING), 摆杆静止下垂");
        end


        n_checks = n_checks + 1;
        if (sync_desync_n == 0 && sync_th_desync_n == 0) begin
            $display("  [PASS] 传感器与 PLANT 同步 (pulse 偏差峰值 %0d <= %0d, theta 偏差峰值 %0d <= %0d Q16)",
                     sync_pulse_peak, SYNC_PULSE_TOL, sync_th_peak, SYNC_TH_Q16_TOL);
        end else begin
            n_fail = n_fail + 1;
            $display("  [FAIL] 传感器与 PLANT 失步: pulse 失步 %0d 次 (峰值偏差 %0d, 容差 %0d), theta 失步 %0d 次 (峰值 %0d Q16, 容差 %0d)",
                     sync_desync_n, sync_pulse_peak, SYNC_PULSE_TOL,
                     sync_th_desync_n, sync_th_peak, SYNC_TH_Q16_TOL);
        end


        $display("\n[PHASE A / TEST A] 闭合电机使能, 观测能量泵起摆 -> 倒立捕获...");
        reset_stats;
        mon_en      = 1'b1;
        sw_motor_en = 1'b1;

        avail   = hil_ctrl_cycles - ticks_used - setup_ticks;
        n_ticks = (SWING_MAX < avail) ? SWING_MAX : avail;
        if (n_ticks < 0) n_ticks = 0;


        while (tick_idx < n_ticks && catch_tick < 0) sync_ctrl_tick;
        elapsed    = tick_idx;
        ticks_used = ticks_used + elapsed;

        swing_avg_pwm = (swing_pwm_n > 0) ? (swing_pwm_sum * 1.0 / swing_pwm_n) : 0.0;
        $display("  ---- TEST A 起摆结果 ----");
        $display("  观测窗口         : %0d ms (计划 %0d ms)", elapsed, n_ticks);
        $display("  是否进入 BALANCE : %s", (catch_tick >= 0) ? "YES" : "NO");
        $display("  起摆耗时         : %0d ms", (catch_tick >= 0) ? catch_tick : -1);
        $display("  SWINGUP 期 |th| 最小值 : %8.3f deg (需显著小于 180 才说明真的抬起来了)",
                 (min_abs_th_swing_deg > 1.0e8) ? -1.0 : min_abs_th_swing_deg);
        $display("  SWINGUP 期 |pwm| 平均值 : %8.2f (占空比计数, 满量程 1000)", swing_avg_pwm);
        $display("  全程 |pwm| 峰值  : %0d", max_abs_pwm);
        $display("  全程 |alpha| 峰值: %8.3f deg (软限位 ±720 deg)", max_abs_alpha_deg);
        $display("  机械能区间       : [%0d, %0d] mJ (下垂静止 ~ -98 mJ, 倒立静止 0 mJ)",
                 min_energy_mj, max_energy_mj);
        $display("  [诊断] raw_dtheta 解卷绕假尖峰: %0d 次, |峰值| = %0.1f rad/s (门限 %.0f rad/s)",
                 dtheta_spike_n, dtheta_spike_peak, DTH_SPIKE_RAD_S);
        $display("         该探测器验证 angle_sensor_reader.v 的最短角位移解卷绕是否生效:");
        $display("         若无解卷绕, theta 每跨越 ±180° 会注入 -2*pi*1000 = -6283 rad/s 假值;");
        $display("         本 PHASE 内 theta 实际跨越 ±180° 共 %0d 次 (跨越数为 0 则探测器未被覆盖)",
                 theta_wrap_n);

        n_checks = n_checks + 1;
        if (catch_tick >= 0) begin
            test_a_pass = 1;
            $display("  [PASS] TEST A: 静止下垂的摆杆被抬起并在 %0d ms 内切入倒立自平衡 (基础要求 1)",
                     catch_tick);
        end else begin
            n_fail = n_fail + 1;
            $display("  [FAIL] TEST A: 在 %0d ms 内未能切入 BALANCE —— 起摆失败 (基础要求 1 未达成)",
                     elapsed);
            $display("         诊断: SWINGUP 期 |th| 最小值 = %8.3f deg, |pwm| 平均 = %8.2f, 峰值 = %0d",
                     (min_abs_th_swing_deg > 1.0e8) ? -1.0 : min_abs_th_swing_deg,
                     swing_avg_pwm, max_abs_pwm);
        end


        n_checks = n_checks + 1;
        if (dtheta_spike_n != 0) begin
            n_fail = n_fail + 1;
            $display("  [FAIL] TEST A2: 检测到 %0d 次解卷绕假尖峰, |峰值| = %0.1f rad/s —— angle_sensor_reader.v 的最短角位移解卷绕可能已失效",
                     dtheta_spike_n, dtheta_spike_peak);
        end else if (theta_wrap_n == 0) begin
            n_fail = n_fail + 1;
            $display("  [FAIL] TEST A2: 全程未发生 ±180° 跨越, 解卷绕路径未被覆盖 —— 0 次尖峰不构成证据");
        end else begin
            $display("  [PASS] TEST A2: 最短角位移解卷绕有效 —— theta 跨越 ±180° 共 %0d 次, 假尖峰 0 次",
                     theta_wrap_n);
        end


        if (catch_tick < 0) begin
            test_alt_used = 1;
            $display("\n[PHASE A-alt] 起摆失败 -> 启用**诊断性回退投放** (theta=15deg, theta_dot=0, alpha=0, alpha_dot=0)");
            $display("            注: 此举仅为隔离缺陷、继续采集 B/C/D 证据, **TEST A 仍为 FAIL**");
            $display("            注: 投放后 tick_idx 归零, 下方 [FSM]/TRACE 的时间轴以投放点为原点");
            state_load      = 1'b1;
            init_theta_q16  = CAPTURE_TH_Q16;
            init_dtheta_q16 = 32'sd0;
            init_alpha_q16  = 32'sd0;
            init_dalpha_q16 = 32'sd0;
            wait_ctrl_ticks(5);
            ticks_used   = ticks_used + 5;
            freeze_ticks = freeze_ticks + 5;
            key_pos_clear = 1'b0;
            #KEY_LONG_NS;
            key_pos_clear = 1'b1;
            wait_ctrl_ticks(60);
            ticks_used   = ticks_used + 60;
            freeze_ticks = freeze_ticks + 60;
            $display("  投放后(仍冻结): PLANT theta=%0d Q16, DUT theta_err_q16=%0d, dtheta_q16=%0d",
                     plant_theta_q16, u_dut.theta_err_q16, u_dut.dtheta_q16);
            $display("  投放后(仍冻结): PLANT alpha=%0d Q16, DUT pulse_count=%0d, alpha_rad_q16=%0d, traj_mode=%0d",
                     plant_alpha_q16, u_dut.pulse_count, u_dut.alpha_rad_q16, u_dut.traj_mode);
            state_load = 1'b0;
            reset_stats;
            wait_ctrl_ticks(20);
            ticks_used = ticks_used + 20;
            $display("  释放 20ms 后: fsm_state=%0d (2=BALANCE 说明 FSM 已自行捕获), catch_tick=%0d ms",
                     u_dut.fsm_state, catch_tick);
            if (u_dut.fsm_state !== 2'd2) begin
                n_fail = n_fail + 1;
                n_checks = n_checks + 1;
                $display("  [FAIL] PHASE A-alt: 即使投放到捕获窗口内, FSM 仍未能切入 BALANCE —— 捕获逻辑或 LQI 存在额外缺陷");
            end else begin
                $display("  [INFO] PHASE A-alt: FSM 已自行捕获并切入 BALANCE, 继续执行 B/C/D");
            end
        end


        if (catch_tick >= 0) begin
            test_b_run = 1;
            $display("\n[PHASE B / TEST B] 倒立平衡保持观测...");
            avail   = hil_ctrl_cycles - ticks_used - setup_ticks;
            n_ticks = (BAL_HOLD_PLAN < avail) ? BAL_HOLD_PLAN : avail;
            if (n_ticks < 0) n_ticks = 0;
            phase_start_tick = tick_idx;
            while (tick_idx - phase_start_tick < n_ticks && mon_state_now === 2'd2)
                sync_ctrl_tick;
            elapsed    = tick_idx - phase_start_tick;
            ticks_used = ticks_used + elapsed;
            bal_held   = elapsed;

            $display("  ---- TEST B 平衡保持结果 ----");
            $display("  本次保持时长     : %0d ms (计划 %0d ms)", bal_held, n_ticks);
            $display("  累计 BALANCE 时长: %0d ms", balance_ticks);
            $display("  BALANCE 期 |th| 峰值 : %8.3f deg (判据 < %.1f deg)", max_abs_th_bal_deg, BAL_ANG_LIMIT);
            $display("  BALANCE 期 |pwm| 峰值: %0d", max_abs_pwm_bal);
            $display("  跌落回退次数 (2->1)  : %0d", fall_count);
            $display("  PROTECT 停留 tick 数 : %0d", protect_ticks);
            $display("  soft_limit_err tick 数: %0d (转臂软限位 ±720 deg)", softlimit_ticks);

            n_checks = n_checks + 1;
            if (fall_count == 0 && protect_ticks == 0 && max_abs_th_bal_deg < BAL_ANG_LIMIT &&
                bal_held >= n_ticks && n_ticks > 0) begin
                test_b_pass = 1;
                $display("  [PASS] TEST B: 平衡保持 %0d ms 无跌落, |th| 峰值 %8.3f deg < %.1f deg",
                         bal_held, max_abs_th_bal_deg, BAL_ANG_LIMIT);
            end else begin
                n_fail = n_fail + 1;
                $display("  [FAIL] TEST B: 平衡未能稳定保持 (跌落 %0d 次, PROTECT %0d tick, |th|峰值 %8.3f deg, 保持 %0d/%0d ms)",
                         fall_count, protect_ticks, max_abs_th_bal_deg, bal_held, n_ticks);
            end
        end else begin
            $display("\n[PHASE B / TEST B] 未执行 —— 未进入 BALANCE, 无从考察平衡保持 (前置 TEST A 已失败)");
        end


        if (fall_count > 0) begin
            test_rec_run = 1;
            $display("\n[PHASE B'] 已发生跌落回退, 额外观察是否能自行重新起摆...");
            avail   = hil_ctrl_cycles - ticks_used - setup_ticks;
            n_ticks = (RECOVER_PLAN < avail) ? RECOVER_PLAN : avail;
            if (n_ticks < 0) n_ticks = 0;
            wait_ctrl_ticks(n_ticks);
            ticks_used = ticks_used + n_ticks;
            $display("  再观察 %0d ms 后: fsm_state=%0d, 累计进入 BALANCE 次数=%0d, 累计跌落次数=%0d",
                     n_ticks, u_dut.fsm_state, catch_count, fall_count);
        end


        if (u_dut.fsm_state === 2'd2) begin
            avail   = hil_ctrl_cycles - ticks_used - setup_ticks;
            n_ticks = (DIST_PLAN < avail) ? DIST_PLAN : avail;
            if (n_ticks >= DIST_PUSH_MS + 50) begin
                test_c_run = 1;
                $display("\n[PHASE C / TEST C] 施加外部推力扰动: disturb_tau_pendulum = +0.040 N*m, 持续 %0d ms",
                         DIST_PUSH_MS);
                trace_div_save = trace_div;
                trace_div      = 10;
                dist_max_deg    = 0.0;
                dist_recover_ms = -1;
                dist_win_open   = 1'b1;
                disturb_pend_uNm = DIST_TAU_uNm;
                wait_ctrl_ticks(DIST_PUSH_MS);
                disturb_pend_uNm = 32'sd0;
                dist_push_end_tick = tick_idx;
                wait_ctrl_ticks(n_ticks - DIST_PUSH_MS);
                dist_win_open = 1'b0;
                trace_div     = trace_div_save;
                ticks_used    = ticks_used + n_ticks;

                $display("  ---- TEST C 抗扰结果 ----");
                $display("  推力期间及恢复期 |th| 峰值 : %8.3f deg (判据 < %.1f deg)", dist_max_deg, DIST_DEF_LIMIT);
                $display("  恢复到 |th| < %.1f deg 耗时 : %0d ms (判据 < %0d ms, -1 = 未恢复)",
                         DIST_REC_TH, dist_recover_ms, DIST_REC_LIMIT);
                $display("  扰动后 fsm_state = %0d, 累计跌落次数 = %0d", u_dut.fsm_state, fall_count);

                n_checks = n_checks + 1;
                if (dist_max_deg < DIST_DEF_LIMIT && dist_recover_ms >= 0 &&
                    dist_recover_ms < DIST_REC_LIMIT && u_dut.fsm_state === 2'd2) begin
                    test_c_pass = 1;
                    $display("  [PASS] TEST C: 抵抗 0.04 N*m / %0d ms 推力, 最大偏角 %8.3f deg, %0d ms 内恢复直立 (基础要求 3)",
                             DIST_PUSH_MS, dist_max_deg, dist_recover_ms);
                end else begin
                    n_fail = n_fail + 1;
                    $display("  [FAIL] TEST C: 抗扰不达标 (峰值 %8.3f deg, 恢复 %0d ms, 末态 %0d)",
                             dist_max_deg, dist_recover_ms, u_dut.fsm_state);
                end
            end else begin
                $display("\n[PHASE C / TEST C] 跳过 —— 剩余预算 %0d ms 不足", avail);
            end
        end else begin
            $display("\n[PHASE C / TEST C] 跳过 —— 当前不在 BALANCE 态 (fsm_state=%0d)", u_dut.fsm_state);
        end


        if (u_dut.fsm_state === 2'd2) begin
            avail   = hil_ctrl_cycles - ticks_used - setup_ticks;
            n_ticks = (TRAJ_PLAN < avail) ? TRAJ_PLAN : avail;
            if (n_ticks >= 4700) begin
                test_d_run = 1;
                $display("\n[PHASE D / TEST D] 短按 KEY2 切换 traj_mode 0 -> 1 (alpha_ref = +45 deg), 观测位置伺服与直立保持...");
                traj_max_deg         = 0.0;
                traj_max_pos_err_deg = 0.0;
                traj_win_open        = 1'b1;
                traj_start_tick      = tick_idx;
                traj_ss_sum_pos      = 0.0;
                traj_ss_sum_th       = 0.0;
                traj_ss_cnt          = 0;

                key_pos_clear = 1'b0;
                #KEY_PRESS_NS;
                key_pos_clear = 1'b1;


                ramp_guard = 0;
                while (u_dut.alpha_ref_q16 != 32'sd51472 && ramp_guard < 3000) begin
                    wait_ctrl_ticks(1);
                    ramp_guard = ramp_guard + 1;
                end
                $display("  短按后 traj_mode = %0d (期望 1), alpha_ref_q16 = %0d (期望 51472 = +45deg), 斜坡耗时 %0d ms",
                         u_dut.traj_mode, u_dut.alpha_ref_q16, ramp_guard);


                wait_ctrl_ticks(TAIL_SETTLE_MS);
                traj_tail_start = tick_idx;
                $display("  沉降 %0d ms 后开启 %0d ms 稳态尾窗 (tick %0d)",
                         TAIL_SETTLE_MS, TAIL_AVG_MS, traj_tail_start);
                wait_ctrl_ticks(TAIL_AVG_MS);
                traj_win_open = 1'b0;
                ticks_used    = ticks_used + (tick_idx - traj_start_tick);

                traj_ss_pos_err_deg = (traj_ss_cnt > 0) ? (traj_ss_sum_pos / traj_ss_cnt) : -1.0;
                traj_ss_th_deg      = (traj_ss_cnt > 0) ? (traj_ss_sum_th  / traj_ss_cnt) : -1.0;

                $display("  ---- TEST D 定点伺服结果 ----");
                $display("  traj_mode          : %0d", u_dut.traj_mode);
                $display("  alpha_ref_q16      : %0d (%8.3f deg)", u_dut.alpha_ref_q16,
                         u_dut.alpha_ref_q16 * (RAD2DEG / 65536.0));
                $display("  PLANT alpha        : %0d (%8.3f deg)", plant_alpha_q16, al_deg_now);
                $display("  DUT alpha_rad_q16  : %0d (%8.3f deg)", u_dut.alpha_rad_q16,
                         u_dut.alpha_rad_q16 * (RAD2DEG / 65536.0));
                $display("  稳态位置误差(后%0dms均值): %8.4f deg (判据 < %.2f deg, 采样数 %0d)",
                         TAIL_AVG_MS, traj_ss_pos_err_deg, TRAJ_POS_LIMIT, traj_ss_cnt);
                $display("  末点位置误差(瞬时值)  : %8.4f deg", traj_final_pos_err_deg);
                $display("  过程最大位置误差      : %8.4f deg", traj_max_pos_err_deg);
                $display("  过程 |th| 峰值        : %8.3f deg (判据 < %.1f deg, 拓展要求 3: 位置控制中保持直立)",
                         traj_max_deg, TRAJ_TH_LIMIT);
                $display("  稳态 |th| 均值(后%0dms) : %8.4f deg (判据 < %.2f deg)",
                         TAIL_AVG_MS, traj_ss_th_deg, TRAJ_TH_SS_LIMIT);
                $display("  alpha_int_q16      : %0d (LQI 积分器, 消除静差)", u_dut.alpha_int_q16);

                n_checks = n_checks + 1;
                if (u_dut.traj_mode === 2'd1 && traj_ss_pos_err_deg >= 0.0 &&
                    traj_ss_pos_err_deg < TRAJ_POS_LIMIT && traj_ss_th_deg < TRAJ_TH_SS_LIMIT &&
                    traj_max_deg < TRAJ_TH_LIMIT && u_dut.fsm_state === 2'd2) begin
                    test_d_pass = 1;
                    $display("  [PASS] TEST D: 转臂跟随至 +45 deg (稳态静差 %8.4f deg), 全过程摆杆直立 (|th|峰值 %8.3f deg, 稳态 %8.4f deg) (拓展要求 1/3)",
                             traj_ss_pos_err_deg, traj_max_deg, traj_ss_th_deg);
                end else begin
                    n_fail = n_fail + 1;
                    $display("  [FAIL] TEST D: 定点伺服或直立保持不达标 (mode=%0d, 稳态静差 %8.4f deg, 稳态|th| %8.4f deg, |th|峰值 %8.3f deg, 末态 %0d)",
                             u_dut.traj_mode, traj_ss_pos_err_deg, traj_ss_th_deg, traj_max_deg, u_dut.fsm_state);
                end
            end else begin
                $display("\n[PHASE D / TEST D] 跳过 —— 剩余预算 %0d ms 不足", avail);
            end
        end else begin
            $display("\n[PHASE D / TEST D] 跳过 —— 当前不在 BALANCE 态 (fsm_state=%0d)", u_dut.fsm_state);
        end


        if (u_dut.fsm_state === 2'd2) begin
            phase_start_tick = tick_idx;
            $display("\n[PHASE D-ret] 按键切换 traj_mode 1 -> 2 -> 3 -> 0，平滑回正至 0° 定点...");

            repeat (3) begin
                key_pos_clear = 1'b0;
                #KEY_PRESS_NS;
                key_pos_clear = 1'b1;
                wait_ctrl_ticks(5);
            end
            if (u_dut.traj_mode !== 2'd0) begin
                $display("  [WARN] 连按后 traj_mode = %0d != 0, 步进切换至 0...", u_dut.traj_mode);
                while (u_dut.traj_mode !== 2'd0) begin
                    key_pos_clear = 1'b0;
                    #KEY_PRESS_NS;
                    key_pos_clear = 1'b1;
                    wait_ctrl_ticks(5);
                end
            end
            $display("  已切回 mode 0 (当前 traj_mode = %0d), 等待斜坡降为 0°...", u_dut.traj_mode);
            ramp_guard = 0;
            while (u_dut.alpha_ref_q16 != 32'sd0 && ramp_guard < 3000) begin
                wait_ctrl_ticks(1);
                ramp_guard = ramp_guard + 1;
            end
            settle_guard = 0;
            while ((abs_al_now >= 1.0) && settle_guard < 3000) begin
                wait_ctrl_ticks(1);
                settle_guard = settle_guard + 1;
            end
            $display("  回正至 0° 完成，斜坡耗时 %0d ms，转臂沉降耗时 %0d ms (al=%0.3f°)，额外沉降 500 ms...",
                     ramp_guard, settle_guard, al_deg_now);
            wait_ctrl_ticks(500);
            ticks_used = ticks_used + (tick_idx - phase_start_tick);
        end


        if (u_dut.fsm_state === 2'd2) begin
            avail = hil_ctrl_cycles - ticks_used - setup_ticks;
            if (avail >= SINE_PLAN) begin
                test_e_run = 1;
                phase_start_tick = tick_idx;
                $display("\n[PHASE E / TEST E] 切换 traj_mode 0 -> 1 -> 2 -> 3，启动 0.2Hz 正弦动态跟踪 (幅值 25°, 周期 5000ms)...");
                sine_max_deg         = 0.0;
                sine_max_pos_err_deg = 0.0;
                sine_ss_sum_sq       = 0.0;
                sine_ss_cnt          = 0;


                repeat (3) begin
                    key_pos_clear = 1'b0;
                    #KEY_PRESS_NS;
                    key_pos_clear = 1'b1;
                    wait_ctrl_ticks(5);
                end
                $display("  切入完成: traj_mode = %0d (期望 3)", u_dut.traj_mode);

                sine_win_open = 1'b1;
                wait_ctrl_ticks(SINE_PLAN);
                sine_win_open = 1'b0;
                ticks_used = ticks_used + (tick_idx - phase_start_tick);

                sine_rms_deg = (sine_ss_cnt > 0) ? $sqrt(sine_ss_sum_sq / sine_ss_cnt) : -1.0;

                $display("  ---- TEST E 正弦跟踪结果 ----");
                $display("  traj_mode             : %0d", u_dut.traj_mode);
                $display("  观测时长              : %0d ms (完整 1 周期)", SINE_PLAN);
                $display("  正弦跟踪 RMS 误差     : %8.4f deg (判据 < %.1f deg)", sine_rms_deg, SINE_RMS_LIMIT);
                $display("  过程最大位置跟踪误差  : %8.4f deg", sine_max_pos_err_deg);
                $display("  过程 |th| 峰值        : %8.3f deg (判据 < %.1f deg, 拓展要求 2: 动态过程中始终倒立自平衡)",
                         sine_max_deg, SINE_TH_LIMIT);
                $display("  跌落回退次数          : %0d", fall_count);
                $display("  PROTECT 停留 tick 数  : %0d", protect_ticks);
                $display("  FSM 末态              : %0d", u_dut.fsm_state);

                n_checks = n_checks + 1;
                if (u_dut.traj_mode === 2'd3 && sine_rms_deg >= 0.0 &&
                    sine_rms_deg < SINE_RMS_LIMIT && sine_max_deg < SINE_TH_LIMIT &&
                    fall_count == 0 && protect_ticks == 0 && u_dut.fsm_state === 2'd2) begin
                    test_e_pass = 1;
                    $display("  [PASS] TEST E: 0.2Hz 正弦动态轨迹跟踪达成 (RMS 误差 %8.4f deg), 摆杆始终稳定倒立 (|th| 峰值 %8.3f deg) (拓展要求 2)",
                             sine_rms_deg, sine_max_deg);
                end else begin
                    n_fail = n_fail + 1;
                    $display("  [FAIL] TEST E: 正弦跟踪或直立保持不达标 (mode=%0d, RMS=%8.4f deg, |th|峰值=%8.3f deg, fall=%0d, 末态=%0d)",
                             u_dut.traj_mode, sine_rms_deg, sine_max_deg, fall_count, u_dut.fsm_state);
                end
            end else begin
                $display("\n[PHASE E / TEST E] 跳过 —— 剩余预算 %0d ms 不足 (需至少 %0d ms)", avail, SINE_PLAN);
            end
        end else begin
            $display("\n[PHASE E / TEST E] 跳过 —— 当前不在 BALANCE 态 (fsm_state=%0d)", u_dut.fsm_state);
        end


        avail = hil_ctrl_cycles - ticks_used - setup_ticks;
        if (avail > 0) begin
            $display("\n[TAIL] 继续运行剩余 %0d ms 预算...", avail);
            wait_ctrl_ticks(avail);
            ticks_used = ticks_used + avail;
        end


        $display("\n=================================================================");
        $display("   HIL 闭环仿真总结");
        $display("=================================================================");
        $display("  总仿真物理时长       : %0d ms (setup %0d ms + 观测 %0d ms, 其中冻结摆位 %0d ms)",
                 setup_ticks + ticks_used, setup_ticks, ticks_used, freeze_ticks);
        expected_rk4 = (setup_ticks + ticks_used - freeze_ticks) * 5;
        $display("  RK4 累计步数         : %0d (自检: 应 = 5 x 非冻结物理时长 ms = %0d)",
                 plant_rk4_steps, expected_rk4);
        $display("  数值发散标志 diverged: %b (必须为 0, 否则闭环结论无效)", plant_diverged);
        $display("  FSM 末态             : %0d (0=HANGING 1=SWINGUP 2=BALANCE 3=PROTECT)", u_dut.fsm_state);
        $display("  进入 BALANCE 次数    : %0d, 跌落回退次数: %0d, PROTECT tick: %0d, soft_limit tick: %0d",
                 catch_count, fall_count, protect_ticks, softlimit_ticks);
        $display("  raw_dtheta 解卷绕假尖峰 : %0d 次 (|峰值| %0.1f rad/s) —— 自上一次 reset_stats 起",
                 dtheta_spike_n, dtheta_spike_peak);
        if (test_alt_used == 0) begin
            $display("  首次起摆耗时         : %0d ms", catch_tick);
        end else begin
            $display("  *** 警示: 下方 TEST B/C/D 的结果是在【PHASE A-alt 诊断性回退投放】下取得的 ***");
            $display("  ***       即摆杆并非由能量泵自行抬起, TEST A (基础要求 1) 已判 FAIL       ***");
            $display("  首次起摆耗时         : N/A —— 能量泵在 %0d ms 窗口内未能把摆杆抬进捕获区", SWING_MAX);
            $display("                         下方 %0d ms 是人工投放后 FSM 自行捕获的相对时刻", catch_tick);
            $display("                         (A-alt 后时间轴归零), **不代表起摆能力**");
        end
        $display("  累计 BALANCE 时长    : %0d ms", balance_ticks);
        $display("  |theta| 峰值(全程)   : %8.3f deg", max_abs_th_all_deg);
        $display("  |theta| 峰值(BALANCE): %8.3f deg", max_abs_th_bal_deg);
        $display("  |alpha| 峰值(全程)   : %8.3f deg", max_abs_alpha_deg);
        $display("  |pwm_duty| 峰值      : %0d / 1000", max_abs_pwm);
        $display("  末端 PLANT 状态      : theta=%8.3f deg, dtheta=%8.3f rad/s, alpha=%8.3f deg, dalpha=%8.3f rad/s",
                 th_deg_now, dth_now, al_deg_now, dal_now);
        $display("  末端 V_motor         : %8.1f mV", plant_vmotor_mv * 1.0);
        $display("-----------------------------------------------------------------");
        $display("  TEST 0 零点标定      : %s", (test_cal_pass == 1) ? "PASS" : "FAIL");
        $display("  TEST A 起摆(基础1)   : %s%s", (test_a_pass == 1) ? "PASS" : "FAIL",
                 (test_alt_used == 1) ? "   <== 赛题基础要求 1 在当前 RTL 下未达成" : "");
        $display("  TEST B 平衡保持(基础3): %s",
                 (test_b_run == 0) ? "SKIPPED (依赖 TEST A)" : ((test_b_pass == 1) ? "PASS" : "FAIL"));
        $display("  TEST C 抗扰(基础3)   : %s",
                 (test_c_run == 0) ? "SKIPPED" : ((test_c_pass == 1) ? "PASS" : "FAIL"));
        $display("  TEST D 定点直立(拓展1/3): %s",
                 (test_d_run == 0) ? "SKIPPED" : ((test_d_pass == 1) ? "PASS" : "FAIL"));
        $display("  TEST E 正弦跟踪(拓展2): %s",
                 (test_e_run == 0) ? "SKIPPED" : ((test_e_pass == 1) ? "PASS" : "FAIL"));
        $display("-----------------------------------------------------------------");
        n_checks = n_checks + 1;
        if (plant_diverged !== 1'b0 || plant_rk4_steps != expected_rk4) begin
            n_fail = n_fail + 1;
            $display("  [FAIL] 被控对象数值完整性检查未通过 (diverged=%b, rk4=%0d)",
                     plant_diverged, plant_rk4_steps);
        end else begin
            $display("  [PASS] 被控对象数值完整性检查通过 (无发散, RK4 步数与物理时长严格一致)");
        end


        if (sync_desync_n != 0 || sync_th_desync_n != 0) begin
            n_fail = n_fail + 1;
            $display("  [FAIL] 闭环全程存在传感器与 PLANT 失步: pulse 失步 %0d 次 (峰值偏差 %0d), theta 失步 %0d 次 (峰值 %0d Q16)",
                     sync_desync_n, sync_pulse_peak, sync_th_desync_n, sync_th_peak);
        end else begin
            $display("  [PASS] 闭环全程传感器与 PLANT 同步复查通过 (pulse 偏差峰值 %0d, theta 偏差峰值 %0d Q16)",
                     sync_pulse_peak, sync_th_peak);
        end


        if (n_checks < 10 || test_b_run == 0 || test_c_run == 0 || test_d_run == 0 || test_e_run == 0) begin
            n_fail = n_fail + 1;
            $display("  [FAIL] 存在未执行/被跳过的测试项或检查项数不足 (n_checks=%0d, 期望 10; b_run=%0d, c_run=%0d, d_run=%0d, e_run=%0d)",
                     n_checks, test_b_run, test_c_run, test_d_run, test_e_run);
        end
        $display("=================================================================");
        if (n_fail == 0)
            $display("  HIL RESULT: PASS  (%0d/%0d 项判据通过)", n_checks - n_fail, n_checks);
        else
            $display("  HIL RESULT: FAIL  (%0d/%0d 项判据通过, %0d 项失败)",
                     n_checks - n_fail, n_checks, n_fail);
        $display("=================================================================");
        $finish;
    end


    initial begin
        #1;
        wd_ns = (hil_ctrl_cycles + 3000) * 1000000.0;
        #wd_ns;
        $display("\n*** HIL WATCHDOG TIMEOUT: 仿真超出预算仍未正常结束, 强制终止 ***");
        $display("*** 已运行 tick_idx = %0d ms, fsm_state = %0d, catch_tick = %0d ***",
                 tick_idx, u_dut.fsm_state, catch_tick);
        $display("  HIL RESULT: FAIL  (watchdog timeout)");
        $finish;
    end

endmodule
