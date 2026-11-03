`ifndef SYNTHESIS

`timescale 1ns / 1ps

module furuta_plant_model #(
    parameter integer CLK_FREQ_HZ     = 50_000_000,
    parameter integer PHYS_RATE_HZ    = 5_000,
    parameter integer CTRL_RATE_HZ    = 1_000,
    parameter integer OUT_REFRESH_DIV = 100,
    parameter integer ADC_ZERO_CODE   = 2048,
    parameter integer ADC_BITS        = 12,
    parameter integer ENC_CPR         = 4000,
    parameter real    V_SUPPLY        = 12.0
)(
    input  wire               clk,
    input  wire               rst_n,


    input  wire               motor_in1,
    input  wire               motor_in2,
    input  wire               motor_stby,


    input  wire signed [31:0] disturb_tau_pend_uNm,
    input  wire signed [31:0] disturb_tau_arm_uNm,


    input  wire signed [31:0] sensor_noise_urad,


    input  wire               state_load,
    input  wire signed [31:0] init_alpha_q16,
    input  wire signed [31:0] init_dalpha_q16,
    input  wire signed [31:0] init_theta_q16,
    input  wire signed [31:0] init_dtheta_q16,


    output reg  [ADC_BITS-1:0] raw_adc,
    output reg                 enc_a,
    output reg                 enc_b,


    output reg  signed [31:0]  theta_q16,
    output reg  signed [31:0]  dtheta_q16,
    output reg  signed [31:0]  alpha_q16,
    output reg  signed [31:0]  dalpha_q16,
    output reg  signed [31:0]  v_motor_mV,
    output reg  signed [31:0]  energy_rel_mJ,
    output reg                 phys_step,
    output reg                 ctrl_tick,
    output reg                 diverged,
    output reg  signed [31:0]  rk4_step_count
);


    localparam integer PHYS_CYCLES    = CLK_FREQ_HZ / PHYS_RATE_HZ;
    localparam integer CTRL_CYCLES    = CLK_FREQ_HZ / CTRL_RATE_HZ;
    localparam integer STEPS_PER_CTRL = PHYS_RATE_HZ / CTRL_RATE_HZ;
    localparam integer ADC_COUNTS     = 1 << ADC_BITS;
    localparam real    DT_PHYS        = 1.0 / (PHYS_RATE_HZ * 1.0);


    localparam real L1           = 0.152;
    localparam real J1           = 0.000693;
    localparam real B1           = 0.0010;
    localparam real COULOMB_TAU  = 0.0020;

    localparam real L2_FULL      = 0.150;
    localparam real LC2          = 0.075;
    localparam real M2           = 0.090;
    localparam real JP           = 0.000675;
    localparam real B2           = 0.00010;
    localparam real G_ACC        = 9.81;

    localparam real V_MAX        = 12.0;
    localparam real R_M          = 4.0;
    localparam real K_T          = 0.050;
    localparam real K_B          = 0.050;
    localparam real GEAR_RATIO   = 1.0;
    localparam real MOTOR_DEAD   = 0.25;

    localparam real J0           = J1 + M2 * L1 * L1;
    localparam real KM           = (GEAR_RATIO * K_T) / R_M;
    localparam real B_EMF        = (GEAR_RATIO * K_T * K_B) / R_M;

    localparam real PI       = 3.14159265358979323846;
    localparam real TWO_PI   = 6.28318530717958647692;
    localparam real RAD2DEG  = 57.29577951308232087680;


    real alpha;
    real dalpha;
    real theta;
    real dtheta;
    real V_motor;
    real dist_pend;
    real dist_arm;

    integer rk4_steps_i;


    function real fabs_r;
        input real x;
        begin
            fabs_r = (x < 0.0) ? -x : x;
        end
    endfunction


    function real ftanh;
        input real x;
        real xx, ep, en;
        begin
            xx = x;
            if (xx >  20.0) xx =  20.0;
            if (xx < -20.0) xx = -20.0;
            ep    = $exp(xx);
            en    = $exp(-xx);
            ftanh = (ep - en) / (ep + en);
        end
    endfunction


    function real norm_theta;
        input real x;
        real    r;
        integer guard;
        begin
            r     = x;
            guard = 0;
            while ((r > PI) && (guard < 64)) begin
                r     = r - TWO_PI;
                guard = guard + 1;
            end
            while ((r < -PI) && (guard < 128)) begin
                r     = r + TWO_PI;
                guard = guard + 1;
            end
            norm_theta = r;
        end
    endfunction


    function integer round_real;
        input real x;
        real xc;
        begin
            round_real = 0;
            xc = x;
            if (xc >  1.0e9) xc =  1.0e9;
            if (xc < -1.0e9) xc = -1.0e9;
            if (xc >= 0.0) round_real = $rtoi(xc + 0.5);
            else           round_real = $rtoi(xc - 0.5);
        end
    endfunction


    integer noise_seed;
    function real urand01;
        input real dummy;
        integer r;
        begin
            r       = $random(noise_seed);
            urand01 = (r + 2147483648.0) / 4294967296.0;
            if (urand01 < 1.0e-12)      urand01 = 1.0e-12;
            if (urand01 > 0.9999999999) urand01 = 0.9999999999;
        end
    endfunction


    function real gauss_scaled;
        input real sigma;
        real u1, u2;
        begin
            u1           = urand01(0.0);
            u2           = urand01(0.0);
            gauss_scaled = sigma * $sqrt(-2.0 * $ln(u1)) * $cos(TWO_PI * u2);
        end
    endfunction


    task plant_deriv;
        input  real s_a, s_da, s_th, s_dth, s_vm;
        output real d_a, d_da, d_th, d_dth;
        real th_n, sin_th, cos_th, sin_2th;
        real v_eff, tau_motor, tau_coulomb;
        real m11, m12, m22, f1, f2, det_m;
        begin

            th_n    = norm_theta(s_th);
            sin_th  = $sin(th_n);
            cos_th  = $cos(th_n);
            sin_2th = $sin(2.0 * th_n);


            v_eff = s_vm;
            if (v_eff >  V_MAX) v_eff =  V_MAX;
            if (v_eff < -V_MAX) v_eff = -V_MAX;
            if (fabs_r(v_eff) < MOTOR_DEAD) v_eff = 0.0;
            tau_motor = KM * v_eff - B_EMF * s_da + dist_arm;


            if (COULOMB_TAU > 0.0) tau_coulomb = COULOMB_TAU * ftanh(50.0 * s_da);
            else                   tau_coulomb = 0.0;


            m11 = J0 + JP * (sin_th * sin_th);
            m12 = M2 * L1 * LC2 * cos_th;
            m22 = JP;


            f1 = tau_motor
               - B1 * s_da
               - tau_coulomb
               - JP * sin_2th * s_da * s_dth
               + M2 * L1 * LC2 * sin_th * (s_dth * s_dth);

            f2 = M2 * G_ACC * LC2 * sin_th
               + 0.5 * JP * sin_2th * (s_da * s_da)
               - B2 * s_dth
               + dist_pend;


            det_m = m11 * m22 - m12 * m12;
            d_a   = s_da;
            d_da  = (m22 * f1 - m12 * f2) / det_m;
            d_th  = s_dth;
            d_dth = (-m12 * f1 + m11 * f2) / det_m;
        end
    endtask


    task rk4_step;
        input real vm;
        input real dt;
        real k1a, k1b, k1c, k1d;
        real k2a, k2b, k2c, k2d;
        real k3a, k3b, k3c, k3d;
        real k4a, k4b, k4c, k4d;
        begin
            plant_deriv(alpha, dalpha, theta, dtheta, vm,
                        k1a, k1b, k1c, k1d);
            plant_deriv(alpha + 0.5*dt*k1a, dalpha + 0.5*dt*k1b,
                        theta + 0.5*dt*k1c, dtheta + 0.5*dt*k1d, vm,
                        k2a, k2b, k2c, k2d);
            plant_deriv(alpha + 0.5*dt*k2a, dalpha + 0.5*dt*k2b,
                        theta + 0.5*dt*k2c, dtheta + 0.5*dt*k2d, vm,
                        k3a, k3b, k3c, k3d);
            plant_deriv(alpha + dt*k3a, dalpha + dt*k3b,
                        theta + dt*k3c, dtheta + dt*k3d, vm,
                        k4a, k4b, k4c, k4d);

            alpha  = alpha  + (dt/6.0) * (k1a + 2.0*k2a + 2.0*k3a + k4a);
            dalpha = dalpha + (dt/6.0) * (k1b + 2.0*k2b + 2.0*k3b + k4b);
            theta  = norm_theta(theta + (dt/6.0) * (k1c + 2.0*k2c + 2.0*k3c + k4c));
            dtheta = dtheta + (dt/6.0) * (k1d + 2.0*k2d + 2.0*k3d + k4d);

            rk4_steps_i = rk4_steps_i + 1;
        end
    endtask


    reg [15:0] phys_cnt;
    reg [3:0]  sub_cnt;

    wire phys_now = (rst_n === 1'b1) && (phys_cnt == PHYS_CYCLES - 1);
    wire ctrl_now = phys_now && (sub_cnt == STEPS_PER_CTRL - 1);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            phys_cnt  <= 16'd0;
            sub_cnt   <= 4'd0;
            phys_step <= 1'b0;
            ctrl_tick <= 1'b0;
        end else begin
            phys_step <= phys_now;
            ctrl_tick <= ctrl_now;
            if (phys_now) begin
                phys_cnt <= 16'd0;
                if (ctrl_now) sub_cnt <= 4'd0;
                else          sub_cnt <= sub_cnt + 4'd1;
            end else begin
                phys_cnt <= phys_cnt + 16'd1;
            end
        end
    end


    reg signed [31:0] in1_hi_cnt;
    reg signed [31:0] in2_hi_cnt;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            in1_hi_cnt <= 32'sd0;
            in2_hi_cnt <= 32'sd0;
        end else if (ctrl_now) begin
            in1_hi_cnt <= 32'sd0;
            in2_hi_cnt <= 32'sd0;
        end else begin
            if (motor_in1 && motor_stby) in1_hi_cnt <= in1_hi_cnt + 32'sd1;
            if (motor_in2 && motor_stby) in2_hi_cnt <= in2_hi_cnt + 32'sd1;
        end
    end


    integer pulse_emitted;


    reg [15:0] out_cnt;
    always @(posedge clk) begin
        if (rst_n !== 1'b1) begin
            alpha   = 0.0;
            dalpha  = 0.0;
            theta   = PI;
            dtheta  = 0.0;
            V_motor = 0.0;
            rk4_steps_i   = 0;
            pulse_emitted = 0;
            out_cnt       <= 16'd0;
            raw_adc       <= ADC_ZERO_CODE;
            enc_a         <= 1'b0;
            enc_b         <= 1'b0;
            theta_q16     <= 32'sd0;
            dtheta_q16    <= 32'sd0;
            alpha_q16     <= 32'sd0;
            dalpha_q16    <= 32'sd0;
            v_motor_mV    <= 32'sd0;
            energy_rel_mJ <= 32'sd0;
            diverged      <= 1'b0;
            rk4_step_count<= 32'sd0;
        end else if (state_load) begin

            alpha   = init_alpha_q16  / 65536.0;
            dalpha  = init_dalpha_q16 / 65536.0;
            theta   = init_theta_q16  / 65536.0;
            dtheta  = init_dtheta_q16 / 65536.0;
            V_motor = 0.0;


            pulse_emitted = round_real(alpha * ENC_CPR / TWO_PI);
            refresh_outputs;
        end else begin
            dist_pend = disturb_tau_pend_uNm * 1.0e-6;
            dist_arm  = disturb_tau_arm_uNm  * 1.0e-6;

            if (ctrl_now) begin
                V_motor = V_SUPPLY * (in1_hi_cnt - in2_hi_cnt) / (CTRL_CYCLES * 1.0);
                if (!motor_stby) V_motor = 0.0;
            end

            if (phys_now) rk4_step(V_motor, DT_PHYS);


            if (out_cnt == OUT_REFRESH_DIV - 1) begin
                out_cnt <= 16'd0;
                refresh_outputs;
            end else begin
                out_cnt <= out_cnt + 16'd1;
            end
        end


        if (!( (theta <= 4.0) && (theta >= -4.0) &&
               (fabs_r(dalpha) <= 1.0e4) && (fabs_r(dtheta) <= 1.0e4) &&
               (fabs_r(alpha)  <= 3.0e4) ))
            diverged <= 1'b1;
    end


    task refresh_outputs;
        real    th_n, adc_r, pulse_r, th_out, al_out;
        integer adc_i, pulse_i, s_i;
        real    e_rel;
        begin
            th_n = norm_theta(theta);


            th_out = th_n;
            if (th_out >  4.0) th_out =  4.0;
            if (th_out < -4.0) th_out = -4.0;
            adc_r  = ADC_ZERO_CODE + th_out * ADC_COUNTS / TWO_PI;
            if (sensor_noise_urad != 32'sd0)
                adc_r = adc_r + gauss_scaled((sensor_noise_urad * 1.0e-6) * ADC_COUNTS / TWO_PI);
            adc_i = round_real(adc_r) % ADC_COUNTS;
            if (adc_i < 0) adc_i = adc_i + ADC_COUNTS;
            raw_adc <= adc_i;


            al_out  = alpha;
            if (al_out >  30000.0) al_out =  30000.0;
            if (al_out < -30000.0) al_out = -30000.0;
            pulse_r = al_out * ENC_CPR / TWO_PI;
            pulse_i = round_real(pulse_r);
            if      (pulse_emitted < pulse_i) pulse_emitted = pulse_emitted + 1;
            else if (pulse_emitted > pulse_i) pulse_emitted = pulse_emitted - 1;
            s_i     = pulse_emitted % 4;
            if (s_i < 0) s_i = s_i + 4;
            enc_a <= (s_i == 1) || (s_i == 2);
            enc_b <= (s_i == 2) || (s_i == 3);


            theta_q16  <= round_real(th_out * 65536.0);
            dtheta_q16 <= round_real(dtheta * 65536.0);
            alpha_q16  <= round_real(al_out * 65536.0);
            dalpha_q16 <= round_real(dalpha * 65536.0);
            v_motor_mV <= round_real(V_motor * 1000.0);


            e_rel         = 0.5 * JP * (dtheta * dtheta)
                          + M2 * G_ACC * LC2 * ($cos(th_n) - 1.0);
            energy_rel_mJ <= round_real(e_rel * 1000.0);

            rk4_step_count <= rk4_steps_i;
        end
    endtask


    integer selftest_en;
    real    st_adc_r;
    integer st_adc_i, st_pulse_i, st_s, st_i;
    reg     st_a, st_b;

    task map_adc;
        input real th;
        integer ai;
        begin
            st_adc_r = ADC_ZERO_CODE + th * ADC_COUNTS / TWO_PI;
            ai       = round_real(st_adc_r) % ADC_COUNTS;
            if (ai < 0) ai = ai + ADC_COUNTS;
            st_adc_i = ai;
        end
    endtask

    task map_ab;
        input real al;
        begin
            st_pulse_i = round_real(al * ENC_CPR / TWO_PI);
            st_s       = st_pulse_i % 4;
            if (st_s < 0) st_s = st_s + 4;
            st_a = (st_s == 1) || (st_s == 2);
            st_b = (st_s == 2) || (st_s == 3);
        end
    endtask

    initial begin
        selftest_en = 1;
        if ($value$plusargs("PLANT_SELFTEST=%d", st_i)) selftest_en = st_i;
        noise_seed  = 32'h1234_5678;
        dist_pend   = 0.0;
        dist_arm    = 0.0;

        if (selftest_en) begin
            $display("  [PLANT-SELFTEST] dt_phys=%.6f s, steps/ctrl=%0d, phys_cycles=%0d, ctrl_cycles=%0d",
                     DT_PHYS, STEPS_PER_CTRL, PHYS_CYCLES, CTRL_CYCLES);

            map_adc(0.0);
            $display("  [PLANT-SELFTEST] theta=0 (inverted)  -> raw_adc=%0d (expect %0d)",
                     st_adc_i, ADC_ZERO_CODE);
            if (st_adc_i !== ADC_ZERO_CODE)
                $display("  [PLANT-SELFTEST] *** FAIL: inverted point mapping ***");

            map_adc(PI);
            $display("  [PLANT-SELFTEST] theta=+pi (hanging) -> raw_adc=%0d (expect 0)", st_adc_i);
            if (st_adc_i !== 0)
                $display("  [PLANT-SELFTEST] *** FAIL: +pi mapping ***");

            map_adc(-PI);
            $display("  [PLANT-SELFTEST] theta=-pi (hanging) -> raw_adc=%0d (expect 0)", st_adc_i);
            if (st_adc_i !== 0)
                $display("  [PLANT-SELFTEST] *** FAIL: -pi mapping ***");


            $write("  [PLANT-SELFTEST] quadrature sequence for increasing alpha: ");
            for (st_i = 0; st_i < 5; st_i = st_i + 1) begin
                map_ab(st_i * TWO_PI / (ENC_CPR * 1.0));
                $write("%b%b ", st_a, st_b);
            end
            $display("");
            $display("  [PLANT-SELFTEST]   expect 00 10 11 01 00 (matches encoder_quad_reader +1 table)");


            $write("  [PLANT-SELFTEST] quadrature sequence for decreasing alpha: ");
            for (st_i = 0; st_i < 5; st_i = st_i + 1) begin
                map_ab(-st_i * TWO_PI / (ENC_CPR * 1.0));
                $write("%b%b ", st_a, st_b);
            end
            $display("");
            $display("  [PLANT-SELFTEST]   expect 00 01 11 10 00 (matches encoder_quad_reader -1 table)");
        end
    end

endmodule

`endif
