# 网页监控（完整代码 + 本地桥接）

这是最终交付包里的完整网页监控代码，来自 J280 微调台。  
它在浏览器里通过 **Web Serial** 直接读 CH340 串口，不经过后端；  
本地 `live_bridge.py` 只负责历史 CSV、参数面板和“构建/烧录”服务，**不占串口**。

原始说明保留在 `README_原始.md`（里面的路径/模式是旧机器的，已不作为主说明）。

---

## 1. 启动

### 最简：双击根目录的 `Start_Web.bat`

它等价于：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "03_Web_Monitor\start_monitor.ps1"
```

脚本会启动两个进程：

| 服务 | 地址 | 作用 |
|---|---|---|
| 静态网页 | `http://127.0.0.1:8787/` | 页面、3D、波形、FFT、历史、参数面板 |
| 本地桥接 | `http://127.0.0.1:8788/health` | 历史 CSV、参数构建/烧录、事件记录 |

启动后浏览器打开：

```text
http://127.0.0.1:8787/
```

**不要用 `file://` 直接打开 `index.html`**；Web Serial 需要 localhost/HTTPS。

### 手动启动（调试用）

```powershell
cd <本目录>
python -m http.server 8787 --bind 127.0.0.1
```

另开一个终端：

```powershell
cd <本目录>
python live_bridge.py
```

---

## 2. 环境要求

| 项 | 要求 |
|---|---|
| Python | **3.9+**，安装时勾选 “Add python.exe to PATH” |
| 浏览器 | **Chrome / Edge**（支持 Web Serial） |
| 串口驱动 | CH340（Windows 10/11 通常自动装；没有则装 WCH CH340 驱动） |
| 端口 | 8787、8788 不要被其他程序占用 |

网页只读遥测；**不会向板子写任何字节**。

---

## 3. 串口与遥测格式

| 项 | 值 |
|---|---|
| 波特率 | 115200 |
| 格式 | 8-N-1，无流控 |
| 帧长 | 22 字节 |
| 帧头 | `AA 55` |
| 频率 | 100 Hz |
| 字节 2..17 | theta_err / dtheta / alpha / dalpha：4 × s32 LE Q12.16（÷65536） |
| 字节 18..19 | duty：s16（±1000） |
| 字节 20 | status：`[1:0]` FSM，`[2]` lqr_en，`[3]` swing_en，`[4]` motor_active，`[5]` energy_deficit，`[6]` calib_done，**`[7]` traj_moving** |
| 字节 21 | XOR(byte 2..20) |

### 0xD0FA 的 bit7

**bit7 = `traj_moving`**，表示轨迹正在运动，不是“推杆恢复”。  
网页/CSV 里看到 bit7=1 是正常轨迹运动。

CSV 列：

```text
time,theta_rad,dtheta_rad_s,alpha_rad,dalpha_rad_s,duty,status
```

网页“保存本次”写回 `02_FPGA_Source_0xD0FA/data/web_*.csv`；历史对比读取同一目录。

---

## 4. 页面能做什么

- **三维姿态**：摆臂 + 摆杆实时投影，末端轨迹、数值读数。
- **时域**：θ / α / duty 三通道，5/10/30/60 s 或全窗口；22°/45° 参考线。
- **频域**：Hann 窗 FFT，0–25 Hz 谱线 + 滚动瀑布图；不足 2.56 s 不报主频。
- **稳定度/噪音**：θ σ、α σ、duty σ、2–20 Hz 音调幅、主频、饱和占比、首捕保持等。
- **历史对比**：读取 `data/*.csv`，或用“保存本次”写回。
- **参数面板**：21 个源码常量，显示当前值/单位/范围。
- **构建/烧录**：调 `gw_sh` 构建、检查时序/DSP、归档位流、可选 SRAM 烧录。
- **板载按键说明**。

> 参数面板/构建烧录来自主工程工作台，0xD0FA 是拓展工程。
> **2026-11-01 已把页面按键表改成 6 档语义，桥接的 DSP 硬门也改为按器件总量校验**，
> 所以 19/20 的 0xD0FA 现在可以走网页构建；参数默认值仍偏主工程，改前对照源码。
> 改参数/烧录也可以直接用 `01_Bitstream_and_Flash/Flash_0xD0FA.bat` 或 Gowin IDE。

---

## 5. 本地桥接路径（换电脑的关键）

`start_monitor.ps1` 会自动设置：

```text
J280_PROJECT_DIR = <交接包>/02_FPGA_Source_0xD0FA
```

所以桥接的历史 CSV、参数源码、构建脚本都指向本交接包，不依赖旧电脑路径。

如果 Gowin 不在 `D:\Gowin\Gowin_V1.9.12.03_x64`：

1. 在交接包根目录新建 `GowinRoot.txt`；
2. 只写一行安装根目录，例如：
   ```text
   C:\Gowin\Gowin_V1.9.12.03_x64
   ```

如果下载线不是 GWU2X：

1. 在交接包根目录新建 `Cable.txt`；
2. 只写一行线名，例如：
   ```text
   Gowin USB Cable(FT2CH)
   ```

不建这两个文件也可以：默认 `D:\Gowin\...` + GWU2X；找不到工具时网页仍可只读串口。

---

## 6. 0xD0FA 按键表（网页下方也会显示）

| 键 | 操作 | 作用 |
|---|---|---|
| SW1 | 短按 | 摆杆竖直零点标定 |
| SW2 | 短按 | 6 档：0° → +45° → −45° → +90° → −90° → 0.1 Hz 正弦 |
| SW2 | 长按 ≥0.7 s | 只清 PID 慢积分 |
| SW3 | 短按 | 电机使能开/关（上电默认关） |
| SW4 | 短按 | 停机刹车/自由滑行切换 |
| SW4 | 长按 >0.7 s | 转臂基准清零（α=0） |
| SW5 | 复位 | 清状态机/标定/使能/轨迹；运行中不要按 |

---

## 7. 常见问题

### 网页打不开

```powershell
Get-NetTCPConnection -LocalPort 8787,8788
```

如果被旧实例占用，结束旧实例；不要同时开多份。

### 提示 “当前浏览器不支持 Web Serial”

用 Chrome / Edge，并通过 `http://127.0.0.1:8787/` 打开；不要用 `file://`。

### 串口列表为空

- 检查 CH340 驱动和设备管理器；
- 换 USB 口/线，避免 Hub；
- 关掉其他占用 COM 口的软件（串口是独占的）。

### 桥接显示离线

- 确认 `python live_bridge.py` 在 8788 运行；
- 只读串口不依赖桥接；历史/参数面板才需要。

### 参数构建报找不到 gw_sh / programmer

- 检查 `GowinRoot.txt`；
- 或直接手动烧 `01_Bitstream_and_Flash/Flash_0xD0FA.bat`。

---

## 8. 文件说明

| 文件 | 作用 |
|---|---|
| `index.html` | 页面结构 |
| `app.js` | 串口解析、3D、波形、FFT、历史、参数面板 |
| `styles.css` | 样式 |
| `live_bridge.py` | 本地桥接（历史/参数/构建烧录） |
| `start_monitor.ps1` | 启动 8787 + 8788 |
| `README_原始.md` | 旧机器版说明（路径/4 档语义已过时） |
| `CLAUDE_LIVE_PROTOCOL.md` | 旧 live 协议说明，保留参考 |