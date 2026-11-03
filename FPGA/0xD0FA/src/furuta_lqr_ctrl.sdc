// =============================================================================
// J280 姿态控制系统开发套件 - 时序约束文件 (SDC)
// 目标器件: 高云半导体 GW2A-LV55PG484C8/I7
// 顶层模块: j280_hw_top (J280 实机引脚适配版)
// =============================================================================

// 1. 系统主时钟约束: 板载 50MHz 有源晶振 (周期 20.000ns, 占空比 50%)
create_clock -name clk_50m -period 20.000 -waveform {0.000 10.000} [get_ports {clk_50m}]

// 2. 异步复位与慢速用户按键假路径 (False Path)
set_false_path -from [get_ports {rst_n}]
set_false_path -from [get_ports {key_zero_calib key_pos_clear key_motor_en key_brake_mode}]

// 3. 异步传感器输入端口假路径 (内部已做双级同步器 + 8 拍数字滤波消抖)
set_false_path -from [get_ports {enc_a enc_b}]

// 4. 3PA1030 并行 ADC 数据假路径:
//    数据由本设计生成的 adc_clk (12.5MHz, div_cnt[1]) 驱动翻转,
//    RTL 采样点固定选在 adc_clk 上升沿之后 40ns/60ns 的稳定相位
//    (adc_3pa1030_reader 内 div_cnt==0/1), 距两端边沿均有 >=20ns 裕量,
//    建立/保持关系由设计保证, 对静态时序分析设为假路径。
set_false_path -from [get_ports {adc_data_in[*]}]

// 5. 板载状态指示 LED 假路径 (人眼观察慢速显示)
set_false_path -to [get_ports {led_balance led_calib_ok led_motor_run led_enable}]
