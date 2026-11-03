# J280 Rotary Inverted Pendulum

基于高云 GW2A-55 FPGA 的旋转倒立摆控制系统。FPGA 以 1 kHz 完成角度采样、滤波、能量起摆、串级 PID 自平衡与位置/轨迹控制；K230 视觉模块识别手势，经 ESP32-S3 转为单线脉宽命令送入 FPGA，实现非接触控制。系统输出 100 Hz 遥测，配套网页上位机。

## 硬件

- FPGA：GW2A-LV55PG484C8/I7（GW2A-55C），50 MHz
- 摆杆角度：WDD35D4 导电塑料电位器 + 3PA1030 10 bit ADC
- 转臂角度：电机尾部增量式编码器，A/B 正交四倍频
- 电机驱动：TB6612FNG，PWM 19.5 kHz
- 视觉：K230 CanMV，YOLOv8n-pose
- 通信：ESP32-S3，WiFi AP + TCP

## 功能

- 能量起摆、自动捕获、自平衡
- 6 档位置控制：0° / +45° / −45° / +90° / −90° / 0.1 Hz 正弦
- 0.1 Hz 正弦轨迹跟踪（幅值 0.6 rad）
- K230 手势控制：右臂 +90°、左臂 −90°、双臂平举 0°
- ESP32 BOOT 键一键进入正弦档
- 100 Hz 遥测、网页三维姿态 / 时域 / FFT / 稳定度统计

## 目录

```
FPGA/0xD0FA    控制核心源码、位流、烧录脚本
FPGA/0xAEF0    手势命令版源码、位流、烧录脚本
ESP32/         手势桥源码、已编译固件、烧录脚本
K230/          手势识别脚本与语音文件
Web/           网页上位机
Tools/         串口记录与数据分析脚本
```

## 烧录

1. FPGA：Gowin Programmer 烧 `FPGA/*/**.fs`（SRAM，掉电丢失）
2. ESP32：esptool 烧 `ESP32/build/PERIPHERAL.bin` 到 `0x10000`
3. K230：`j280_gesture.py` 与 `voice/` 拷到 SD 卡

## 环境

- Gowin V1.9.12.03
- ESP-IDF v5.4
- CanMV K230 V1.4.3

## 遥测

115200-8-N-1，100 Hz，22 字节/帧，XOR 校验。字段：θ、θ̇、α、α̇、duty、status。

## License

MIT

> ESP32 的 AI 聊天/TTS 需要自己的火山引擎 Key，仓库中已替换为占位符；
> 预编译 app（build/PERIPHERAL.bin）未上传，请用 ESP-IDF 自行编译。
