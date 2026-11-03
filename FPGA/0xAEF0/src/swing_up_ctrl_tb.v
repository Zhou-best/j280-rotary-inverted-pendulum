`timescale 1ns / 1ps


module swing_up_ctrl_tb;

    reg                   clk;
    reg                   rst_n;
    reg                   calc_en;
    reg  signed [31:0]    theta_rad_q16;
    reg  signed [31:0]    dtheta_rad_s_q16;
    reg  signed [31:0]    alpha_rad_q16;
    reg  signed [31:0]    dalpha_rad_s_q16;

    wire signed [15:0]    pwm_swing_duty;
    wire                  energy_deficit;
    wire                  calc_done;


    swing_up_ctrl #(
        .MAX_ACC_PUMP (30),
        .VOLT_TO_PWM_K(18)
    ) u_dut (
        .clk             (clk),
        .rst_n           (rst_n),
        .calc_en         (calc_en),
        .swing_active    (1'b1),
        .theta_rad_q16   (theta_rad_q16),
        .dtheta_rad_s_q16(dtheta_rad_s_q16),
        .alpha_rad_q16   (alpha_rad_q16),
        .dalpha_rad_s_q16(dalpha_rad_s_q16),
        .pwm_swing_duty  (pwm_swing_duty),
        .energy_deficit  (energy_deficit),
        .calc_done       (calc_done)
    );


    initial begin
        clk = 1'b0;
        forever #10 clk = ~clk;
    end


    task tick_calc;
        begin
            calc_en = 1'b1;
            #20;
            calc_en = 1'b0;

            #120;
        end
    endtask

    integer total_tests = 0;
    integer pass_tests  = 0;


    task check_case(
        input [128*8:1] desc,
        input signed [31:0] th_q16,
        input signed [31:0] dth_q16,
        input expected_deficit
    );
        begin
            total_tests = total_tests + 1;
            theta_rad_q16    = th_q16;
            dtheta_rad_s_q16 = dth_q16;
            alpha_rad_q16    = 32'sd0;
            dalpha_rad_s_q16 = 32'sd0;

            tick_calc();

            if (energy_deficit === expected_deficit) begin
                $display("  [PASS] %0s | th=%0d, dth=%0d -> deficit=%b (期望: %b), PWM=%0d",
                         desc, th_q16, dth_q16, energy_deficit, expected_deficit, pwm_swing_duty);
                pass_tests = pass_tests + 1;
            end else begin
                $display("  [FAIL] %0s | th=%0d, dth=%0d -> deficit=%b (期望: %b), PWM=%0d",
                         desc, th_q16, dth_q16, energy_deficit, expected_deficit, pwm_swing_duty);
            end
        end
    endtask


    task check_case_full(
        input [128*8:1] desc,
        input signed [31:0] th_q16,
        input signed [31:0] dth_q16,
        input expected_deficit,
        input signed [31:0] exp_ekin,
        input signed [31:0] exp_epot
    );
        integer ekin_act, epot_act;
        integer bad;
        begin
            total_tests = total_tests + 1;
            theta_rad_q16    = th_q16;
            dtheta_rad_s_q16 = dth_q16;
            alpha_rad_q16    = 32'sd0;
            dalpha_rad_s_q16 = 32'sd0;

            tick_calc();

            ekin_act = u_dut.e_kin_q16;
            epot_act = u_dut.e_pot_q16;
            bad = 0;
            if (energy_deficit !== expected_deficit) bad = bad + 1;
            if (ekin_act !== exp_ekin)               bad = bad + 2;
            if (epot_act !== exp_epot)               bad = bad + 4;

            if (bad == 0) begin
                $display("  [PASS] %0s | dth=%0d -> e_kin=%0d e_pot=%0d deficit=%b (三项逐位吻合)",
                         desc, dth_q16, ekin_act, epot_act, energy_deficit);
                pass_tests = pass_tests + 1;
            end else begin
                $display("  [FAIL] %0s | dth=%0d -> e_kin=%0d(期望%0d) e_pot=%0d(期望%0d) deficit=%b(期望%b) [badmask=%0d: 1=deficit 2=e_kin 4=e_pot]",
                         desc, dth_q16, ekin_act, exp_ekin, epot_act, exp_epot,
                         energy_deficit, expected_deficit, bad);
            end
        end
    endtask


    localparam signed [31:0] TH_0DEG   = 32'sd0;
    localparam signed [31:0] TH_45DEG  = 32'sd51472;
    localparam signed [31:0] TH_90DEG  = 32'sd102944;
    localparam signed [31:0] TH_135DEG = 32'sd154415;
    localparam signed [31:0] TH_180DEG = 32'sd205887;

    localparam signed [31:0] DTH_0_0   = 32'sd0;
    localparam signed [31:0] DTH_0_067 = 32'sd4391;
    localparam signed [31:0] DTH_0_5   = 32'sd32768;
    localparam signed [31:0] DTH_1_0   = 32'sd65536;
    localparam signed [31:0] DTH_2_0   = 32'sd131072;
    localparam signed [31:0] DTH_4_0   = 32'sd262144;
    localparam signed [31:0] DTH_6_0   = 32'sd393216;
    localparam signed [31:0] DTH_8_0   = 32'sd524288;

    localparam signed [31:0] DTH_19_86 = 32'sd1301776;
    localparam signed [31:0] DTH_32_0  = 32'sd2097152;
    localparam signed [31:0] DTH_40_0  = 32'sd2621440;

    initial begin
        $display("=================================================================");
        $display("   swing_up_ctrl 单元级能量定标与控制律严密测试 (N1/N2/N3/N5)    ");
        $display("=================================================================");

        rst_n            = 1'b0;
        calc_en          = 1'b0;
        theta_rad_q16    = 32'sd0;
        dtheta_rad_s_q16 = 32'sd0;
        alpha_rad_q16    = 32'sd0;
        dalpha_rad_s_q16 = 32'sd0;

        #100;
        rst_n = 1'b1;
        #100;


        $display("\n[Batch 1] 验证 §2.5 核心工况断言表 (N1 关键拦截点)...");

        check_case("工况 1: 下垂静止", TH_180DEG, DTH_0_0, 1'b1);


        check_case("工况 2: 下垂微动(N1原死区)", TH_180DEG, DTH_0_067, 1'b1);


        check_case("工况 3: 下垂慢摆", TH_180DEG, DTH_0_5, 1'b1);


        check_case("工况 4: 中速起摆(水平过线)", TH_90DEG, DTH_4_0, 1'b1);


        check_case("工况 5: 倒立静止", TH_0DEG, DTH_0_0, 1'b0);


        check_case("工况 6: 高速过顶防过冲", TH_0DEG, DTH_4_0, 1'b0);


        $display("\n[Batch 2] 验证动能钳位阈值/钳位值与 E=0 物理临界点 (N2 重修 + M3 数值断言)...");

        check_case_full("工况 7: +8.0 阈值内",   TH_180DEG, DTH_8_0,      1'b1, 32'sd1408,  -32'sd8680);
        check_case_full("工况 8: +12.0 阈值内",  TH_180DEG, 32'sd786432,  1'b1, 32'sd3168,  -32'sd8680);


        check_case_full("工况 9: +19.86 E=0临界", TH_180DEG, DTH_19_86,   1'b0, 32'sd8680,  -32'sd8680);


        check_case_full("工况10: +32.0 钳位边界", TH_180DEG, DTH_32_0,    1'b0, 32'sd22527, -32'sd8680);
        check_case_full("工况11: +40.0 超限钳位", TH_180DEG, DTH_40_0,    1'b0, 32'sd22527, -32'sd8680);


        check_case_full("工况12: -8.0 阈值内",   TH_180DEG, -DTH_8_0,     1'b1, 32'sd1408,  -32'sd8680);
        check_case_full("工况13: -19.86 E=0临界", TH_180DEG, -DTH_19_86,  1'b0, 32'sd8680,  -32'sd8680);
        check_case_full("工况14: -32.0 钳位边界", TH_180DEG, -DTH_32_0,   1'b0, 32'sd22528, -32'sd8680);
        check_case_full("工况15: -40.0 超限钳位", TH_180DEG, -DTH_40_0,   1'b0, 32'sd22528, -32'sd8680);


        $display("\n[Batch 3] 全域 5x7 = 35 组向量全覆盖测试...");

        check_case("Matrix 01 th=180 dth=0.0", TH_180DEG, DTH_0_0, 1'b1);
        check_case("Matrix 02 th=180 dth=0.5", TH_180DEG, DTH_0_5, 1'b1);
        check_case("Matrix 03 th=180 dth=1.0", TH_180DEG, DTH_1_0, 1'b1);
        check_case("Matrix 04 th=180 dth=2.0", TH_180DEG, DTH_2_0, 1'b1);
        check_case("Matrix 05 th=180 dth=4.0", TH_180DEG, DTH_4_0, 1'b1);
        check_case("Matrix 06 th=180 dth=6.0", TH_180DEG, DTH_6_0, 1'b1);
        check_case("Matrix 07 th=180 dth=8.0", TH_180DEG, DTH_8_0, 1'b1);

        check_case("Matrix 08 th=135 dth=0.0", TH_135DEG, DTH_0_0, 1'b1);
        check_case("Matrix 09 th=135 dth=0.5", TH_135DEG, DTH_0_5, 1'b1);
        check_case("Matrix 10 th=135 dth=1.0", TH_135DEG, DTH_1_0, 1'b1);
        check_case("Matrix 11 th=135 dth=2.0", TH_135DEG, DTH_2_0, 1'b1);
        check_case("Matrix 12 th=135 dth=4.0", TH_135DEG, DTH_4_0, 1'b1);
        check_case("Matrix 13 th=135 dth=6.0", TH_135DEG, DTH_6_0, 1'b1);
        check_case("Matrix 14 th=135 dth=8.0", TH_135DEG, DTH_8_0, 1'b1);

        check_case("Matrix 15 th=90 dth=0.0",  TH_90DEG,  DTH_0_0, 1'b1);
        check_case("Matrix 16 th=90 dth=0.5",  TH_90DEG,  DTH_0_5, 1'b1);
        check_case("Matrix 17 th=90 dth=1.0",  TH_90DEG,  DTH_1_0, 1'b1);
        check_case("Matrix 18 th=90 dth=2.0",  TH_90DEG,  DTH_2_0, 1'b1);
        check_case("Matrix 19 th=90 dth=4.0",  TH_90DEG,  DTH_4_0, 1'b1);
        check_case("Matrix 20 th=90 dth=6.0",  TH_90DEG,  DTH_6_0, 1'b1);
        check_case("Matrix 21 th=90 dth=8.0",  TH_90DEG,  DTH_8_0, 1'b1);

        check_case("Matrix 22 th=45 dth=0.0",  TH_45DEG,  DTH_0_0, 1'b1);
        check_case("Matrix 23 th=45 dth=0.5",  TH_45DEG,  DTH_0_5, 1'b1);
        check_case("Matrix 24 th=45 dth=1.0",  TH_45DEG,  DTH_1_0, 1'b1);
        check_case("Matrix 25 th=45 dth=2.0",  TH_45DEG,  DTH_2_0, 1'b1);
        check_case("Matrix 26 th=45 dth=4.0",  TH_45DEG,  DTH_4_0, 1'b1);
        check_case("Matrix 27 th=45 dth=6.0",  TH_45DEG,  DTH_6_0, 1'b1);
        check_case("Matrix 28 th=45 dth=8.0",  TH_45DEG,  DTH_8_0, 1'b0);

        check_case("Matrix 29 th=0 dth=0.0",   TH_0DEG,   DTH_0_0, 1'b0);
        check_case("Matrix 30 th=0 dth=0.5",   TH_0DEG,   DTH_0_5, 1'b0);
        check_case("Matrix 31 th=0 dth=1.0",   TH_0DEG,   DTH_1_0, 1'b0);
        check_case("Matrix 32 th=0 dth=2.0",   TH_0DEG,   DTH_2_0, 1'b0);
        check_case("Matrix 33 th=0 dth=4.0",   TH_0DEG,   DTH_4_0, 1'b0);
        check_case("Matrix 34 th=0 dth=6.0",   TH_0DEG,   DTH_6_0, 1'b0);
        check_case("Matrix 35 th=0 dth=8.0",   TH_0DEG,   DTH_8_0, 1'b0);


        $display("\n[Batch 4] 验证死点扰动脉冲与转臂回中阻尼 (F4/N3)...");
        theta_rad_q16    = TH_180DEG;
        dtheta_rad_s_q16 = 32'sd0;
        alpha_rad_q16    = 32'sd0;
        dalpha_rad_s_q16 = 32'sd0;
        tick_calc();
        $display("  死点扰动输出 PWM: %0d (期望 > 0 推动转臂)", pwm_swing_duty);
        if (pwm_swing_duty > 0) pass_tests = pass_tests + 1;
        total_tests = total_tests + 1;


        alpha_rad_q16 = 32'sd32768;
        tick_calc();
        $display("  转臂偏置阻尼输出 PWM: %0d (期望产生负向回中减速, 阻尼后 < 90)", pwm_swing_duty);
        if (pwm_swing_duty < 90) pass_tests = pass_tests + 1;
        total_tests = total_tests + 1;


        #50;
        $display("\n=================================================================");
        $display("   测试汇总: 总用例数 = %0d, 通过数 = %0d, 失败数 = %0d",
                 total_tests, pass_tests, total_tests - pass_tests);
        if (pass_tests == total_tests) begin
            $display("🎉 [ALL PASS] swing_up_ctrl 单元级能量定标全部验证通过！");
        end else begin
            $display("❌ [FAIL] 存在未能通过的能量定标用例，请检查定点数运算！");
        end
        $display("=================================================================");
        $finish;
    end

endmodule
