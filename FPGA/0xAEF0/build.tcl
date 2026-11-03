# =============================================================================
# Gowin EDA 命令行构建脚本（综合 + 布局布线 + bitstream + 时序分析）
#
# 用法（PowerShell）:
#   cd <本仓库根目录>                     # 即 .../GoWin/furuta_lqr_ctrl
#   $env:PATH="D:\Gowin_fpga\Gowin\Gowin_V1.9.12.03_x64\IDE\bin;$env:PATH"
#   gw_sh.exe build.tcl
#
# 本脚本用 [info script] 自定位工程文件，**不含任何指向本仓库的绝对路径**，
# 因此仓库整体移动位置后无需修改本文件。（唯一的外部绝对路径是 Gowin EDA
# 的安装位置，由调用方通过 PATH 提供。）
#
# 产出（均在 impl/ 下，该目录已被 .gitignore 忽略）:
#   impl/gwsynthesis/furuta_lqr_ctrl.log              综合日志（查 top module 与 WARN）
#   impl/gwsynthesis/furuta_lqr_ctrl.vg               网表
#   impl/pnr/furuta_lqr_ctrl.rpt.txt                  资源占用与引脚约束
#   impl/pnr/furuta_lqr_ctrl_tr_content.html          时序详情（Fmax / Slack / 关键路径）
#   impl/pnr/furuta_lqr_ctrl.fs                       bitstream
#
# 验收要点：
#   本目录是本包最终版 0xD0FA 的源码快照，验收值以 README.md 为准
#   （Fmax 50.634 MHz / Setup 0.251 ns / Setup·Hold 0/0 / DSP 19/20 / Logic 4763）。
#   注意：本文件旧头部曾写死一条历史基线（Fmax 62.352 MHz、DSP 17.5/20），
#   那是别的工程/别的版本的数字，重跑后对不上属正常，不要拿它判断本次构建失败。
#   唯一必须守住的是：0 违例、Fmax ≥ 50 MHz、User Code 与烧录版本一致。
# =============================================================================

set proj_dir [file dirname [info script]]
open_project "$proj_dir/furuta_lqr_ctrl.gprj"

# -----------------------------------------------------------------------------
# J280 实机必需的项目选项 (与官方 Interface_test 工程实测配置一致):
#   SSPI=1: 将 SSPI 配置专用脚释放为普通 I/O —— T20 (核心板 SW4 按键) 是
#           SSPI_DIN 专用脚, 不放开则 PnR 报 PR2017 "dedicated pin (SSPI)"。
#           用 JTAG/USB 下载不受影响。
# -----------------------------------------------------------------------------
set_option -use_sspi_as_gpio 1

run all
exit
