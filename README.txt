J280 旋转倒立摆 —— 参赛提交代码

目录
  FPGA/0xD0FA    官方控制核心：源码、位流、烧录脚本
  FPGA/0xAEF0    手势命令版：源码、位流、烧录脚本
  ESP32          手势桥源码、已编译固件、烧录脚本
  K230           手势识别脚本、语音文件
  Web            网页上位机
  Tools          串口记录与数据分析脚本

烧录
  1. FPGA：Gowin Programmer 烧对应 .fs（SRAM，掉电丢失）
  2. ESP32：用 ESP-IDF 编译后烧 build/PERIPHERAL.bin 到 0x10000
  3. K230：j280_gesture.py 和 voice/ 拷到 SD 卡

环境
  Gowin V1.9.12.03
  ESP-IDF v5.4
  CanMV K230 V1.4.3

串口
  FPGA 遥测 115200-8-N-1，100 Hz，22 字节/帧，XOR 校验