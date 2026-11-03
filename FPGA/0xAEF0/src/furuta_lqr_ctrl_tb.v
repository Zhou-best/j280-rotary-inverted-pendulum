`ifndef SYNTHESIS

`timescale 1ns / 1ps

module furuta_lqr_ctrl_tb;


    reg         clk;
    reg         rst_n;
    reg         calc_en;


    reg signed [31:0] theta_err;
    reg signed [31:0] dtheta;
    reg signed [31:0] alpha_err;
    reg signed [31:0] dalpha;
    reg signed [31:0] alpha_int;


    wire signed [15:0] pwm_duty;
    wire               calc_done;


    furuta_lqr_ctrl uut (
        .clk        (clk),
        .rst_n      (rst_n),
        .calc_en    (calc_en),
        .theta_err  (theta_err),
        .dtheta     (dtheta),
        .alpha_err  (alpha_err),
        .dalpha     (dalpha),
        .alpha_int  (alpha_int),
        .pwm_duty   (pwm_duty),
        .calc_done  (calc_done)
    );


    initial clk = 1'b0;
    always #10 clk = ~clk;


    integer pass_count = 0;
    integer fail_count = 0;


    task run_calc_check;
        input [8*32:1] test_name;
        input signed [15:0] exp_min;
        input signed [15:0] exp_max;
        integer cycle_delay;
        begin
            cycle_delay = 0;
            @(posedge clk);
            calc_en = 1'b1;
            @(posedge clk);
            calc_en = 1'b0;


            while (!calc_done && cycle_delay < 10) begin
                @(posedge clk);
                cycle_delay = cycle_delay + 1;
            end

            if (calc_done) begin
                $display("[TB 监控] %0s -> 延迟周期: %0d 拍 (理论: 4拍) | 输出 PWM: %0d",
                         test_name, cycle_delay, pwm_duty);


                if (cycle_delay != 4) begin
                    $display("  [ERROR] 流水线延迟异常! 期望 4 拍，实际得到 %0d 拍", cycle_delay);
                    fail_count = fail_count + 1;
                end

                else if (pwm_duty >= exp_min && pwm_duty <= exp_max) begin
                    $display("  [PASS] 结果校验通过! 处于预期范围 [%0d, %0d]", exp_min, exp_max);
                    pass_count = pass_count + 1;
                end else begin
                    $display("  [ERROR] 输出占空比错误! 实际: %0d, 期望区间: [%0d, %0d]",
                             pwm_duty, exp_min, exp_max);
                    fail_count = fail_count + 1;
                end
            end else begin
                $display("  [ERROR] 计算超时! 未检测到 calc_done 脉冲");
                fail_count = fail_count + 1;
            end
            @(posedge clk);
        end
    endtask


    initial begin
        $display("================================================================");
        $display("  高云 GW2A FPGA 倒立摆 LQI 硬件控制器 (furuta_lqr_ctrl) 功能仿真");
        $display("================================================================");


        rst_n     = 1'b0;
        calc_en   = 1'b0;
        theta_err = 32'sd0;
        dtheta    = 32'sd0;
        alpha_err = 32'sd0;
        dalpha    = 32'sd0;
        alpha_int = 32'sd0;
        #100;
        rst_n = 1'b1;
        #40;


        theta_err = 32'sd0;
        dtheta    = 32'sd0;
        alpha_err = 32'sd0;
        dalpha    = 32'sd0;
        alpha_int = 32'sd0;
        run_calc_check("Case 1: 绝对平衡零偏差工况", 16'sd0, 16'sd0);


        theta_err = 32'sd1311;
        dtheta    = 32'sd0;
        alpha_err = 32'sd0;
        dalpha    = 32'sd0;
        alpha_int = 32'sd0;
        run_calc_check("Case 2: 正向微小倾角偏差 (+0.02 rad)", 16'sd120, 16'sd175);


        theta_err = -32'sd1311;
        dtheta    = 32'sd0;
        alpha_err = 32'sd0;
        dalpha    = 32'sd0;
        alpha_int = 32'sd0;
        run_calc_check("Case 3: 负向微小倾角偏差 (-0.02 rad)", -16'sd175, -16'sd120);


        theta_err = 32'sd32768;
        dtheta    = 32'sd0;
        alpha_err = 32'sd0;
        dalpha    = 32'sd0;
        alpha_int = 32'sd0;
        run_calc_check("Case 4: 正向极大偏差硬件上限饱和", 16'sd1000, 16'sd1000);


        theta_err = -32'sd32768;
        dtheta    = 32'sd0;
        alpha_err = 32'sd0;
        dalpha    = 32'sd0;
        alpha_int = 32'sd0;
        run_calc_check("Case 5: 负向极大偏差硬件下限饱和", -16'sd1000, -16'sd1000);


        theta_err = 32'sd655;
        dtheta    = -32'sd1311;
        alpha_err = 32'sd1966;
        dalpha    = 32'sd0;
        alpha_int = 32'sd655;
        run_calc_check("Case 6: 多状态耦合与积分消除静差工况", 16'sd60, 16'sd110);


        #100;
        $display("================================================================");
        if (fail_count == 0) begin
            $display("🎉 恭喜! 全部 %0d 项 Testbench 测试用例 100%% 通过!", pass_count);
            $display("   -> 流水线延迟确定性 (80ns / 4 拍) 验证通过;");
            $display("   -> 有符号数符号扩展与 Q16 移位截断验证通过;");
            $display("   -> 极限大误差硬件饱和防溢出 (±1000) 验证通过。");
        end else begin
            $display("❌ 测试未通过! 成功: %0d, 失败: %0d", pass_count, fail_count);
        end
        $display("================================================================");
        $finish;
    end

endmodule

`endif
