# 0xD0FA 关键代码（最新版）

- 版本：**0xD0FA**（2026-11-01，队友交付的最终版）
- 来源：`../02_FPGA_Source_0xD0FA/` 的完整工程
- 本目录只挑出**参与综合**的源码 + 约束 + 构建文件，方便快速查看；
  完整 testbench、旧文件、构建日志仍在 `../02_FPGA_Source_0xD0FA/`

---

## 1. 文件作用

| 文件 | 作用 |
|---|---|
| `src/j280_hw_top.v` | **顶层**：按键、6 档 `traj_mode`、`pos_traj_gen` 例化、UART 遥测 |
| `src/pos_traj_gen.v` | 位置/速度轨迹规划（6 档 + 0.1 Hz 正弦），**含 0xD0FA 的 sync 修复** |
| `src/furuta_pid_ctrl.v` | 平衡 PID（KP/KD/KC/KV，6 级流水） |
| `src/ctrl_fsm.v` | HANGING / SWINGUP / BALANCE / PROTECT |
| `src/swing_up_ctrl.v` | 起摆能量泵、底部 45 boost、顶部 30° 刹车 |
| `src/encoder_quad_reader.v` | 转臂编码器 + 测速 |
| `src/angle_sensor_reader.v` | 摆杆角度 + One Euro 角速度 |
| `src/adc_3pa1030_reader.v` | 3PA1030 并行 ADC 读取 |
| `src/motor_pwm_driver.v` | 19.5 kHz PWM 输出 |
| `src/uart_telemetry.v` | 22 字节遥测帧；**bit7 = traj_moving** |
| `src/furuta_lqr_ctrl.v` | 旧顶层/兼容文件，工程里仍 enable=1，但当前顶层是 `j280_hw_top` |
| `src/furuta_lqr_ctrl.cst` | 引脚约束（J280 底板） |
| `src/furuta_lqr_ctrl.sdc` | 50 MHz 时钟约束 |
| `build.tcl` | Gowin 综合 + PnR + 位流脚本 |
| `furuta_lqr_ctrl.gprj` | Gowin 工程文件（`traj_gen.v` 已 disable） |

---

## 2. 0xD0FA 相比旧版的关键变化

- 转臂参考统一由 `pos_traj_gen` 产生，取代旧的 hybrid 锁存/慢回 0；
- **6 档**：0° / +45° / −45° / +90° / −90° / 0.1 Hz 正弦，SW2 短按循环；
- SW4 长按 = 锚定转臂基准 + `pos_traj_gen.rezero`；
- `traj_moving=1` 时冻结慢积分；
- 遥测 `status bit7 = traj_moving`（不是恢复模式）；
- **sync 修复**：捕获当拍把 `settle_hold` 置 4，避免参考从捕获角一步吸到 0；
- DSP 19/20，Logic 4763，Fmax 50.634 MHz。

---

## 3. 构建

```powershell
cd <本目录>
$env:PATH="E:\Gowin\Gowin_V1.9.12.03_x64\IDE\bin;$env:PATH"
gw_sh.exe build.tcl
```

验收：

- Setup/Hold 违例 0/0；
- DSP ≤20（0xD0FA 为 19/20）；
- Fmax ≥50 MHz（已验证 50.634 MHz）；
- 生成 `impl/pnr/furuta_lqr_ctrl.fs`，CheckSum 可能与 0xD0FA 不同（PnR 正常波动）。

> **比赛/交付优先烧已验证位流**：
> `../01_Bitstream_and_Flash/furuta_lqr_ctrl_0xD0FA.fs`，
> 不要用随手重新构建的位流替代。

---

## 4. 已知限制

- +45°、−90°、0.1 Hz 正弦没有完整验证；−90° 先小角度试；
- mode0 首捕没有做 N=10 / ≥20 s 正式回归；
- **摆杆抗扰没有在这个版本解决**；
- SRAM 位流：断电/上电后必须重烧；
- 烧录/复位后：SW1 标定 → SW4 长按锚定 → SW3 使能。