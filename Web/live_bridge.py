from __future__ import annotations

import json
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse


ROOT = Path(__file__).resolve().parent
DEFAULT_PROJECT = ROOT.parent / "02_FPGA_Source_0xD0FA"
PROJECT = Path(os.environ.get("J280_PROJECT_DIR", str(DEFAULT_PROJECT))).resolve()
DATA = PROJECT / "data"
VERSIONS = PROJECT / "versions"
BACKUPS = PROJECT / "tune_backups"
TUNE_RUNS = PROJECT / "tune_runs"
def _config_value(name: str) -> str | None:


    path = ROOT.parent / name


    for encoding in ("utf-8-sig", "gbk"):
        try:
            text = path.read_text(encoding=encoding)
        except (OSError, UnicodeError):
            continue
        for line in text.splitlines():
            value = line.strip()
            if value and not value.startswith("#"):
                return value
        return None
    return None


GW_ROOT = Path(
    os.environ.get("J280_GOWIN_ROOT")
    or _config_value("GowinRoot.txt")
    or r"D:\Gowin\Gowin_V1.9.12.03_x64"
)
GW_SH = GW_ROOT / "IDE" / "bin" / "gw_sh.exe"
PROGRAMMER = GW_ROOT / "Programmer" / "bin" / "programmer_cli.exe"
BUILD_TCL = PROJECT / "build.tcl"
CABLE = os.environ.get("J280_CABLE") or _config_value("Cable.txt") or "Gowin USB Cable(GWU2X)"


def select_live_dir() -> Path:
    candidates = []
    configured = os.environ.get("J280_LIVE_DIR")
    if configured:
        candidates.append(Path(configured))
    candidates.extend([ROOT / "live", Path(tempfile.gettempdir()) / "J280Monitor" / "live"])
    for candidate in candidates:
        try:
            candidate.mkdir(parents=True, exist_ok=True)
            probe = candidate / ".write-test"
            probe.write_text("ok", encoding="utf-8")
            probe.unlink()
            return candidate
        except OSError:
            continue
    raise RuntimeError("no writable live directory")


LIVE = select_live_dir()
EVENTS = LIVE / "events.jsonl"
LATEST = LIVE / "latest.json"
LOCK = threading.Lock()
COMMAND_QUEUE: list[dict[str, Any]] = []
NEXT_COMMAND_ID = 1
SESSION: dict[str, Any] = {
    "schema_version": 2,
    "started_at": None,
    "last_event_at": None,
    "event_count": 0,
    "serial": {"connected": False, "rx_bytes": 0, "baud_rate": None},
    "last": None,
    "parameters": {},
}

JOB_LOCK = threading.Lock()
JOBS: dict[str, dict[str, Any]] = {}
CURRENT_JOB: str | None = None
JOB_SEQ = 0


SOURCE_LOCK = threading.Lock()


ALLOWED_ORIGINS = {
    "http://127.0.0.1:8787",
    "http://localhost:8787",
}
ALLOWED_HOSTS = {
    "127.0.0.1:8788",
    "localhost:8788",
}


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def local_stamp() -> str:
    return time.strftime("%Y%m%d_%H%M%S")


def write_latest() -> None:
    temporary = LATEST.with_suffix(".tmp")
    temporary.write_text(json.dumps(SESSION, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, LATEST)


def update_session(event: dict[str, Any]) -> None:
    event.setdefault("received_at", now())
    with LOCK:
        if SESSION["started_at"] is None:
            SESSION["started_at"] = event["received_at"]
        SESSION["last_event_at"] = event["received_at"]
        SESSION["event_count"] += 1
        SESSION["last"] = event
        if event.get("type") == "serial_status":
            SESSION["serial"].update(event.get("data", {}))
        if event.get("type") == "parameter_observation":
            name = event.get("data", {}).get("name")
            if name:
                SESSION["parameters"][name] = event["data"]
        LIVE.mkdir(parents=True, exist_ok=True)
        with EVENTS.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(event, ensure_ascii=False, separators=(",", ":")) + "\n")
        write_latest()


def queue_command(payload: dict[str, Any]) -> dict[str, Any]:
    global NEXT_COMMAND_ID
    code = payload.get("code")
    values = payload.get("payload", [])
    if not isinstance(code, int) or not 0 <= code <= 255:
        raise ValueError("code must be an integer from 0 to 255")
    if not isinstance(values, list) or len(values) != 4 or any(
        not isinstance(x, int) or not 0 <= x <= 255 for x in values
    ):
        raise ValueError("payload must contain four bytes")
    command = {
        "id": NEXT_COMMAND_ID,
        "code": code,
        "payload": values,
        "source": str(payload.get("source", "Claude")),
        "note": str(payload.get("note", "")),
        "queued_at": now(),
    }
    NEXT_COMMAND_ID += 1
    with LOCK:
        COMMAND_QUEUE.append(command)
    update_session({"type": "command_queued", "data": command})
    return command


PARAMS: list[dict[str, Any]] = [

    {
        "id": "kp", "group": "loop", "label": "KP · θ 比例", "unit": "×1024", "kind": "q10_s18",
        "min": -100, "max": 0, "step": 0.1,
        "description": "摆杆角度比例增益（Q10）。−36 是实测下限；继续减半会削弱捕获。负值方向是已验证极性。",
        "targets": [("src/furuta_pid_ctrl.v", r"(localparam\s+signed\s+\[17:0\]\s+KP_18\s*=\s*)(-?18'sd-?\d+)")],
    },
    {
        "id": "kd", "group": "loop", "label": "KD · θ̇ 微分", "unit": "×1024", "kind": "q10_s18",
        "min": -30, "max": 0, "step": 0.01,
        "description": "摆杆角速度微分增益（Q10）。−4.82；与 θ 低通一起决定相位裕量。",
        "targets": [("src/furuta_pid_ctrl.v", r"(localparam\s+signed\s+\[17:0\]\s+KD_18\s*=\s*)(-?18'sd-?\d+)")],
    },
    {
        "id": "kc", "group": "loop", "label": "KC · 转臂位置", "unit": "×1024", "kind": "q10_s18",
        "min": -30, "max": 0, "step": 0.01,
        "description": "转臂位置误差增益（Q10）。−4.0；避免 −4096 等 2 的幂，综合器会映射成 LUT 移位链导致时序崩。",
        "targets": [("src/furuta_pid_ctrl.v", r"(localparam\s+signed\s+\[17:0\]\s+KC_18\s*=\s*)(-?18'sd-?\d+)")],
    },
    {
        "id": "kv", "group": "loop", "label": "KV · 转臂速度阻尼", "unit": "×1024", "kind": "q10_s18",
        "min": -30, "max": 0, "step": 0.01,
        "description": "转臂速度反馈（Q10）。−7.99；是 8 Hz 音调的主要阻尼，勿再降低。避免 −8192 等位模式。",
        "targets": [("src/furuta_pid_ctrl.v", r"(localparam\s+signed\s+\[17:0\]\s+KV_18\s*=\s*)(-?18'sd-?\d+)")],
    },
    {
        "id": "volt_to_pwm", "group": "loop", "label": "电压→PWM 标度", "unit": "×1024", "kind": "q10_s18",
        "min": 40, "max": 120, "step": 0.1,
        "description": "控制电压到 duty 的换算（Q10）。83.33 = 1000/12；一般不动。",
        "targets": [("src/furuta_pid_ctrl.v", r"(localparam\s+signed\s+\[17:0\]\s+VOLT_TO_PWM_18\s*=\s*)(18'sd\d+)")],
    },
    {
        "id": "pwm_max", "group": "loop", "label": "PWM 饱和", "unit": "count", "kind": "s16",
        "min": 100, "max": 1000, "step": 10,
        "description": "输出饱和值（count），同时写 PWM_MAX 与 PWM_MIN。1000 = 12 V；不要超过 1000。",
        "targets": [
            ("src/furuta_pid_ctrl.v", r"(localparam\s+signed\s+\[15:0\]\s+PWM_MAX\s*=\s*)(16'sd\d+)"),
            ("src/furuta_pid_ctrl.v", r"(localparam\s+signed\s+\[15:0\]\s+PWM_MIN\s*=\s*)(-16'sd\d+)"),
        ],
        "symmetric_negative": True,
    },

    {
        "id": "bal_ang", "group": "fsm", "label": "捕获角门", "unit": "°", "kind": "q16_deg",
        "min": 5, "max": 45, "step": 0.5,
        "description": "允许进入 BALANCE 的 |θ| 上限。22° 时 Kp·θ 已接近饱和；10° 是未上板的方向。",
        "targets": [("src/ctrl_fsm.v", r"(localparam\s+signed\s+\[31:0\]\s+BAL_ANG_THRESH\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "bal_omg", "group": "fsm", "label": "捕获速度门", "unit": "rad/s", "kind": "q16_s32",
        "min": 0.5, "max": 12, "step": 0.1,
        "description": "进入 BALANCE 的 |θ̇| 上限。4.0；低于它才允许交接。",
        "targets": [("src/ctrl_fsm.v", r"(localparam\s+signed\s+\[31:0\]\s+BAL_OMG_THRESH\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "fall", "group": "fsm", "label": "跌倒角", "unit": "°", "kind": "q16_deg",
        "min": 30, "max": 80, "step": 0.5,
        "description": "BALANCE 中 |θ| 超过它判跌落。45°；必须大于捕获角门。",
        "targets": [("src/ctrl_fsm.v", r"(localparam\s+signed\s+\[31:0\]\s+FALL_THRESH\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "kick_duty", "group": "fsm", "label": "初始踢腿 duty", "unit": "count", "kind": "int16",
        "min": 0, "max": 600, "step": 10,
        "description": "HANGING→SWINGUP 首个 3 V 脉冲。250 ≈ 3 V；改它会影响首帧方向和能量。",
        "targets": [("src/ctrl_fsm.v", r"(if\s*\(motor_en_sw\s*&&\s*calib_done\)\s*begin[\s\S]{0,250}?final_pwm_duty\s*<=\s*)(16'sd\d+)")],
    },

    {
        "id": "max_acc_pump", "group": "swing", "label": "泵角加速度上限", "unit": "rad/s²", "kind": "int_param",
        "min": 5, "max": 40, "step": 1,
        "description": "能量泵加速度限幅。24；泵 PWM 饱和 = 24×18 = 432。",
        "targets": [("src/swing_up_ctrl.v", r"(parameter\s+integer\s+MAX_ACC_PUMP\s*=\s*)(\d+)")],
    },
    {
        "id": "volt_to_pwm_k", "group": "swing", "label": "泵加速度→duty", "unit": "count/(rad/s²)", "kind": "int_param",
        "min": 8, "max": 30, "step": 1,
        "description": "泵加速度到 duty 的系数。18；与上限相乘应不超过 PWM 饱和。",
        "targets": [("src/swing_up_ctrl.v", r"(parameter\s+integer\s+VOLT_TO_PWM_K\s*=\s*)(\d+)")],
    },
    {
        "id": "deadband", "group": "swing", "label": "泵能 dθ 死区", "unit": "rad/s", "kind": "q16_s32",
        "min": 0, "max": 2, "step": 0.01,
        "description": "低于该 |θ̇| 时泵能项归零，用来杀静止噪声。0.8；过大会削弱小摆幅起摆。",
        "targets": [("src/swing_up_ctrl.v", r"(localparam\s+signed\s+\[31:0\]\s+DTH_DEADBAND_Q16\s*=\s*)(32'sd\d+)")],
    },

    {
        "id": "int_limit", "group": "limits", "label": "LQR 臂积分限幅", "unit": "rad", "kind": "q16_s32",
        "min": 0, "max": 0.5, "step": 0.005,
        "description": "LQR/LQI 转臂积分状态限幅。0.25；PID 模式主要用下一项的门控。",
        "targets": [("src/j280_hw_top.v", r"(localparam\s+signed\s+\[31:0\]\s+INT_LIMIT_Q16\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "int_err", "group": "limits", "label": "LQR 积分分离窗", "unit": "rad", "kind": "q16_s32",
        "min": 0, "max": 0.2, "step": 0.001,
        "description": "|α 误差| 超过它冻结积分。0.0192 ≈ 1.1°；放宽会追着摆推更远。",
        "targets": [("src/j280_hw_top.v", r"(localparam\s+signed\s+\[31:0\]\s+INT_ERR_THRESH_Q16\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "pid_gate", "group": "limits", "label": "PID 慢积分门控", "unit": "rad", "kind": "q16_s32",
        "min": 0, "max": 1.2, "step": 0.01,
        "description": "PID 位置环慢积分的误差门控。0.55；误差越大越冻结，防饱和。",
        "targets": [("src/j280_hw_top.v", r"(localparam\s+signed\s+\[31:0\]\s+PID_INT_GATE_Q16\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "arm_soft", "group": "limits", "label": "转臂软限位", "unit": "°", "kind": "q16_deg",
        "min": 180, "max": 1080, "step": 10,
        "description": "转臂绝对角软限位。720° = ±2 圈；触发会停机保护。",
        "targets": [("src/j280_hw_top.v", r"(parameter\s+signed\s+\[31:0\]\s+ARM_SOFT_LIMIT_Q16\s*=\s*)(32'sd\d+)")],
    },

    {
        "id": "alpha_min", "group": "filter", "label": "One Euro 基频", "unit": "系数", "kind": "coeff_q16",
        "min": 0, "max": 0.3, "step": 0.001,
        "description": "静止时的滤波系数，越大越跟手、噪声越大。0.01885 ≈ 3 Hz 基底。",
        "targets": [("src/angle_sensor_reader.v", r"(localparam\s+signed\s+\[31:0\]\s+ALPHA_MIN_Q16\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "alpha_slope", "group": "filter", "label": "One Euro 斜率", "unit": "系数", "kind": "coeff_q16",
        "min": 0, "max": 0.1, "step": 0.0005,
        "description": "截止频率随 |θ̇| 上升的斜率。0.01257；越大，大摆时滤波越轻。",
        "targets": [("src/angle_sensor_reader.v", r"(localparam\s+signed\s+\[31:0\]\s+ALPHA_SLOPE_Q16\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "alpha_max", "group": "filter", "label": "One Euro 上限", "unit": "系数", "kind": "coeff_q16",
        "min": 0, "max": 0.3, "step": 0.001,
        "description": "滤波系数上限。0.08；0.15 曾在 10 Hz 音调上表现更差，已回退。",
        "targets": [("src/angle_sensor_reader.v", r"(localparam\s+signed\s+\[31:0\]\s+ALPHA_MAX_Q16\s*=\s*)(32'sd\d+)")],
    },
    {
        "id": "alpha_d", "group": "filter", "label": "dθ 平滑系数", "unit": "系数", "kind": "coeff_q16",
        "min": 0, "max": 0.3, "step": 0.001,
        "description": "dθ One Euro 的导数平滑。0.1116 ≈ 20 Hz；越大 dθ 越跟手、噪声越大。",
        "targets": [("src/angle_sensor_reader.v", r"(localparam\s+signed\s+\[31:0\]\s+ALPHA_D_Q16\s*=\s*)(32'sd\d+)")],
    },
]

GROUPS = [
    {"id": "loop", "label": "闭环增益"},
    {"id": "fsm", "label": "状态机 / 捕获"},
    {"id": "swing", "label": "起摆泵"},
    {"id": "limits", "label": "限幅 / 积分"},
    {"id": "filter", "label": "摆杆滤波"},
]

KIND_SCALE = {
    "q10_s18": 1024.0,
    "q16_s32": 65536.0,
    "q16_deg": 65536.0,
    "coeff_q16": 65536.0,
    "int16": 1.0,
    "int_param": 1.0,
    "s16": 1.0,
}

TOKEN_RE = re.compile(r"(-?)(\d+)'sd(-?\d+)")


def token_to_raw(token: str) -> int:
    token = token.strip()
    if re.fullmatch(r"-?\d+", token):
        return int(token)
    m = TOKEN_RE.fullmatch(token)
    if not m:
        raise ValueError(f"无法解析常量: {token}")
    sign = -1 if (m.group(1) == "-" or m.group(3).startswith("-")) else 1
    return sign * abs(int(m.group(3)))


def raw_to_value(kind: str, raw: int) -> float:
    if kind == "q16_deg":
        return raw / 65536.0 * 180.0 / 3.141592653589793
    scale = KIND_SCALE.get(kind, 1.0)
    return raw / scale


def value_to_raw(kind: str, value: float) -> int:
    if kind == "q16_deg":
        return int(round(value * 3.141592653589793 / 180.0 * 65536.0))
    scale = KIND_SCALE.get(kind, 1.0)
    return int(round(value * scale))


def literal_for(kind: str, raw: int, negative_hint: bool = False) -> str:
    if kind == "int_param":
        return str(raw)
    if kind in ("q10_s18",):
        return f"-18'sd{abs(raw)}" if raw < 0 else f"18'sd{raw}"
    if kind in ("q16_s32", "q16_deg", "coeff_q16"):
        return f"-32'sd{abs(raw)}" if raw < 0 else f"32'sd{raw}"
    if kind in ("int16", "s16"):
        if negative_hint or raw < 0:
            return f"-16'sd{abs(raw)}"
        return f"16'sd{raw}"
    return str(raw)


def read_param_current(param: dict[str, Any]) -> tuple[float, int, str]:
    file_rel, pattern = param["targets"][0]
    path = PROJECT / file_rel
    text = read_source(path)
    m = re.search(pattern, text)
    if not m:
        raise ValueError(f"{param['id']}: 在 {file_rel} 找不到声明")
    token = m.group(2)
    raw = token_to_raw(token)
    return raw_to_value(param["kind"], raw), raw, token


def read_source(path: Path) -> str:
    with path.open("r", encoding="utf-8", newline="") as stream:
        return stream.read()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def schema_payload() -> dict[str, Any]:
    groups = []
    problems = []
    for g in GROUPS:
        items = []
        for p in PARAMS:
            if p["group"] != g["id"]:
                continue
            try:
                value, raw, token = read_param_current(p)
                items.append({
                    "id": p["id"], "label": p["label"], "unit": p["unit"],
                    "min": p["min"], "max": p["max"], "step": p["step"],
                    "description": p["description"], "file": p["targets"][0][0],
                    "value": round(value, 6), "raw": raw, "rawText": token,
                })
            except Exception as exc:
                problems.append(str(exc))
        groups.append({"id": g["id"], "label": g["label"], "params": items})
    return {
        "ok": True,
        "project": str(PROJECT),
        "groups": groups,
        "problems": problems,
        "tools": {
            "gw_sh": GW_SH.exists(),
            "programmer": PROGRAMMER.exists(),
            "build_tcl": BUILD_TCL.exists(),
            "data": DATA.exists(),
        },
        "version": read_version(),
        "note": "参数写入 RTL 常量后必须构建并烧录；当前位流没有 UART 接收通路。",
    }


def read_version() -> dict[str, Any]:
    fs = PROJECT / "impl" / "pnr" / "furuta_lqr_ctrl.fs"
    if not fs.exists():
        return {"file": None, "checksum": None, "mtime": None}
    head = fs.read_bytes()[:512]
    m = re.search(rb"CheckSum:\s*0x([0-9A-Fa-f]+)", head)
    return {
        "file": "impl/pnr/furuta_lqr_ctrl.fs",
        "checksum": ("0x" + m.group(1).decode()) if m else None,
        "mtime": datetime.fromtimestamp(fs.stat().st_mtime).strftime("%Y-%m-%d %H:%M:%S"),
        "sha256": sha256_file(fs),
    }


def job_log(job: dict[str, Any], text: str) -> None:
    job["log"] = (job.get("log", "") + text + "\n")[-80000:]


def validate_params(params: dict[str, Any]) -> tuple[dict[str, float], list[str]]:
    values: dict[str, float] = {}
    warnings: list[str] = []
    by_id = {p["id"]: p for p in PARAMS}
    for pid, p in by_id.items():
        if pid not in params:
            raise ValueError(f"缺少参数 {pid}")
        try:
            v = float(params[pid])
        except (TypeError, ValueError):
            raise ValueError(f"{pid} 不是数字") from None
        if not (p["min"] <= v <= p["max"]):
            raise ValueError(f"{p['label']} 超出范围 {p['min']}…{p['max']}（收到 {v}）")
        values[pid] = v
    if values["fall"] <= values["bal_ang"]:
        raise ValueError("跌倒角必须大于捕获角门")
    if values["bal_omg"] <= values["deadband"]:
        warnings.append("捕获速度门 ≤ 泵死区：小摆幅可能无法捕获")
    if values["max_acc_pump"] * values["volt_to_pwm_k"] > values["pwm_max"]:
        warnings.append("泵饱和 > PWM 饱和：起摆会被最终限幅削顶")
    for pid in ("kp", "kd", "kc", "kv"):
        if values[pid] > 0:
            warnings.append(f"{by_id[pid]['label']} 为正：与已验证极性相反，除非你在做极性实验")
    if values["pwm_max"] > 1000:
        warnings.append("PWM 饱和 > 1000 可能超出驱动/供电设计")
    for pid in ("kc", "kv"):
        raw = value_to_raw(by_id[pid]["kind"], values[pid])
        if abs(raw) in (4096, 6144, 8192, 12288) or (abs(raw) & (abs(raw) - 1)) == 0:
            warnings.append(f"{by_id[pid]['label']} 落在 2 的幂/病态位模式附近（raw={raw}），综合后可能时序崩")
    return values, warnings


def patch_sources(values: dict[str, float], job: dict[str, Any]) -> tuple[Path, dict[str, str]]:
    stamp = local_stamp()
    backup_dir = BACKUPS / stamp
    backup_dir.mkdir(parents=True, exist_ok=True)
    originals: dict[str, str] = {}
    patched: dict[str, str] = {}
    by_id = {p["id"]: p for p in PARAMS}

    for pid, p in by_id.items():
        raw = value_to_raw(p["kind"], values[pid])
        for idx, (file_rel, pattern) in enumerate(p["targets"]):
            path = PROJECT / file_rel
            text = originals.get(file_rel)
            if text is None:
                text = read_source(path)
                originals[file_rel] = text
                patched[file_rel] = text
            negative = bool(p.get("symmetric_negative") and pid == "pwm_max" and idx == 1)
            literal = literal_for(p["kind"], raw, negative_hint=negative)
            new_text, count = re.subn(pattern, lambda m, lit=literal: m.group(1) + lit, patched[file_rel])
            if count != 1:
                raise ValueError(f"{p['label']}：{file_rel} 中匹配到 {count} 处（应为 1），源码可能已改动")
            patched[file_rel] = new_text
            job_log(job, f"  {pid:14s} {web_num(values[pid]):>12}  ->  {literal}  [{file_rel}]")


    for file_rel in originals:
        dst = backup_dir / file_rel.replace("/", "__").replace("\\", "__")
        shutil.copy2(PROJECT / file_rel, dst)
    (backup_dir / "params.json").write_text(
        json.dumps({"values": values, "created_at": now()}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    written: list[str] = []
    try:
        for file_rel, text in patched.items():
            if text == originals[file_rel]:
                continue
            path = PROJECT / file_rel
            tmp = path.with_suffix(path.suffix + ".tune_tmp")
            with tmp.open("w", encoding="utf-8", newline="") as stream:
                stream.write(text)
            os.replace(tmp, path)
            written.append(file_rel)
    except Exception:
        for file_rel in written:
            with (PROJECT / file_rel).open("w", encoding="utf-8", newline="") as stream:
                stream.write(originals[file_rel])
        raise
    job_log(job, f"  已写回 {len(written)} 个源文件；备份：{backup_dir}")
    return backup_dir, originals


def web_num(v: float) -> str:
    return f"{v:.6g}"


def decode_bytes(raw: bytes) -> str:
    for enc in ("utf-8", "gbk", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def run_command(job: dict[str, Any], cmd: list[str], cwd: Path, timeout: int, phase: str) -> int:
    job["phase"] = phase
    job_log(job, f"$ {' '.join(cmd)}")
    env = os.environ.copy()
    env["PATH"] = str(GW_SH.parent) + os.pathsep + env.get("PATH", "")
    proc = subprocess.run(cmd, cwd=str(cwd), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    out = decode_bytes(proc.stdout or b"")
    if out:
        job_log(job, out[-30000:])
    return proc.returncode


def parse_build_reports(job: dict[str, Any]) -> dict[str, Any]:
    rpt = PROJECT / "impl" / "pnr" / "furuta_lqr_ctrl.rpt.txt"
    tr = PROJECT / "impl" / "pnr" / "furuta_lqr_ctrl_tr_content.html"
    info: dict[str, Any] = {}
    if rpt.exists():
        text = rpt.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"DSP\s*\|\s*(\d+)/(\d+)", text)
        if m:
            info["dsp"] = f"{m.group(1)}/{m.group(2)}"
        m = re.search(r"Logic\s*\|\s*(\d+)/(\d+)", text)
        if m:
            info["logic"] = f"{m.group(1)}/{m.group(2)}"
    if tr.exists():
        html = tr.read_text(encoding="utf-8", errors="replace")
        m = re.search(
            r"Actual Fmax[\s\S]{0,400}?<td>clk_50m</td>\s*<td>[\d.]+\(MHz\)</td>\s*<td>([\d.]+)\(MHz\)</td>",
            html,
        )
        if m:
            info["fmax_mhz"] = float(m.group(1))
        m = re.search(r"Setup Paths Table[\s\S]{0,900}?<td>\d+</td>\s*<td>(-?[\d.]+)</td>", html)
        if m:
            info["setup_slack_ns"] = float(m.group(1))
        m = re.search(r"Hold Paths Table[\s\S]{0,900}?<td>\d+</td>\s*<td>(-?[\d.]+)</td>", html)
        if m:
            info["hold_slack_ns"] = float(m.group(1))
    return info


def parse_fs_checksum(fs: Path) -> str | None:
    head = fs.read_bytes()[:1024]
    m = re.search(rb"CheckSum:\s*0x([0-9A-Fa-f]+)", head)
    return ("0x" + m.group(1).decode()) if m else None


def run_job(job_id: str, params: dict[str, Any], flash: bool) -> None:
    global CURRENT_JOB
    job = JOBS[job_id]
    job["state"] = "running"
    job["started_at"] = now()
    try:
        if not BUILD_TCL.exists():
            raise RuntimeError(f"找不到 {BUILD_TCL}")
        if not GW_SH.exists():
            raise RuntimeError(f"找不到 {GW_SH}")
        values, warnings = validate_params(params)
        for w in warnings:
            job_log(job, "[WARN] " + w)
        job["warnings"] = warnings

        job["phase"] = "patch"
        job_log(job, "== 1/5 备份并写回 RTL 常量 ==")
        backup_dir, originals = patch_sources(values, job)

        job["phase"] = "build"
        job_log(job, "== 2/5 综合 / PnR / bitstream ==")
        t0 = time.time()
        rc = run_command(job, [str(GW_SH), "build.tcl"], PROJECT, timeout=900, phase="build")
        job_log(job, f"gw_sh 退出码 {rc}，用时 {time.time() - t0:.1f} s")
        if rc != 0:
            raise RuntimeError(f"gw_sh 失败（退出码 {rc}）")

        job["phase"] = "verify"
        job_log(job, "== 3/5 校验位流与时序 ==")
        fs = PROJECT / "impl" / "pnr" / "furuta_lqr_ctrl.fs"
        if not fs.exists():
            raise RuntimeError("构建结束但 impl/pnr/furuta_lqr_ctrl.fs 不存在")
        if fs.stat().st_mtime < t0 - 5:
            raise RuntimeError("位流文件没有更新，构建可能没有真正执行")
        checksum = parse_fs_checksum(fs)
        digest = sha256_file(fs)
        if not checksum:
            raise RuntimeError("位流里找不到 CheckSum 头，拒绝判定为成功")
        if fs.stat().st_size < 100_000:
            raise RuntimeError(f"位流大小异常：{fs.stat().st_size} 字节")
        info = parse_build_reports(job)
        job_log(job, f"  CheckSum: {checksum}  SHA-256: {digest}")
        job_log(job, f"  资源/时序: {json.dumps(info, ensure_ascii=False)}")


        reports = [
            PROJECT / "impl" / "pnr" / "furuta_lqr_ctrl.rpt.txt",
            PROJECT / "impl" / "pnr" / "furuta_lqr_ctrl_tr_content.html",
        ]
        missing_fields = [k for k in ("dsp", "fmax_mhz", "setup_slack_ns", "hold_slack_ns") if info.get(k) is None]
        if missing_fields:
            raise RuntimeError("时序报告解析不完整，缺少：" + ", ".join(missing_fields) + "；拒绝判定为成功")
        stale = [str(p.name) for p in reports if (not p.exists()) or p.stat().st_mtime < t0 - 5]
        if stale:
            raise RuntimeError("时序报告不存在或不是本次构建生成：" + ", ".join(stale))


        dsp_match = re.fullmatch(r"(\d+)\s*/\s*(\d+)", str(info["dsp"]))
        if not dsp_match:
            raise RuntimeError(f"无法解析 DSP 占用 '{info['dsp']}'；拒绝判定为成功")
        dsp_used, dsp_total = int(dsp_match.group(1)), int(dsp_match.group(2))
        if dsp_total != 20:
            raise RuntimeError(f"器件 DSP 总数为 {dsp_total}，GW2A-55 应为 20；构建目标可能不对")


        if dsp_used < 1:
            raise RuntimeError(f"DSP 占用为 {info['dsp']}，报告解析异常；拒绝判定为成功")
        if dsp_used > dsp_total:
            raise RuntimeError(f"DSP 超用：{info['dsp']}")
        if info["setup_slack_ns"] < 0:
            raise RuntimeError(f"Setup slack 为负：{info['setup_slack_ns']} ns")
        if info["hold_slack_ns"] < 0:
            raise RuntimeError(f"Hold slack 为负：{info['hold_slack_ns']} ns")
        if info["fmax_mhz"] < 50:
            raise RuntimeError(f"Fmax {info['fmax_mhz']} MHz < 50 MHz")

        VERSIONS.mkdir(parents=True, exist_ok=True)
        dest = VERSIONS / f"tune_{local_stamp()}_{checksum or 'nocs'}_{digest[:8]}.fs"
        shutil.copy2(fs, dest)
        if sha256_file(dest) != digest:
            raise RuntimeError("位流归档校验失败（SHA-256 不一致）")
        job_log(job, f"  已归档：{dest}")

        flashed = False
        if flash:
            if not PROGRAMMER.exists():
                raise RuntimeError(f"找不到 {PROGRAMMER}")
            job["phase"] = "flash"
            job_log(job, "== 4/5 SRAM 烧录 ==")
            rc = run_command(job, [
                str(PROGRAMMER), "-d", "GW2A-55C", "-r", "2",
                "--cable", CABLE, "--cable-index", "0",
                "--fsFile", str(dest),
            ], PROJECT, timeout=240, phase="flash")
            if rc != 0:
                raise RuntimeError(f"programmer_cli 失败（退出码 {rc}）")
            flashed = True
        else:
            job_log(job, "== 4/5 跳过烧录（仅构建） ==")

        job["phase"] = "record"
        result = {
            "checksum": checksum,
            "sha256": digest,
            "fs": str(dest),
            "backup": str(backup_dir),
            "flashed": flashed,
            "warnings": warnings,
            "info": info,
            "built_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "values": values,
        }
        job["result"] = result
        try:
            TUNE_RUNS.mkdir(parents=True, exist_ok=True)
            (TUNE_RUNS / f"{job_id}.json").write_text(
                json.dumps({**result, "log": job.get("log", "")[-30000:]}, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        except Exception as exc:

            job_log(job, f"[WARN] 构建记录写入失败：{exc}")
        job["state"] = "done"
        job_log(job, "== 5/5 完成 ==" + ("  已烧录，请 SW1 重标定。" if flashed else "  未烧录。"))
    except Exception as exc:
        job["state"] = "failed"
        job["error"] = str(exc)
        job_log(job, "[ERROR] " + str(exc))
    finally:
        job["finished_at"] = now()
        with JOB_LOCK:
            CURRENT_JOB = None
        try:
            SOURCE_LOCK.release()
        except RuntimeError:
            pass


def list_runs() -> list[dict[str, Any]]:
    if not DATA.exists():
        return []
    items = []
    for p in DATA.glob("*.csv"):
        try:
            st = p.stat()
        except OSError:
            continue
        items.append({
            "name": p.name,
            "size": st.st_size,
            "mtime": datetime.fromtimestamp(st.st_mtime).strftime("%Y-%m-%d %H:%M:%S"),
            "mtime_ts": st.st_mtime,
        })
    items.sort(key=lambda x: x["mtime_ts"], reverse=True)
    return items[:300]


def safe_data_file(name: str) -> Path:
    name = unquote(name).strip()
    if "/" in name or "\\" in name or name.startswith("."):
        raise ValueError("非法文件名")
    path = (DATA / name).resolve()
    if DATA.resolve() not in path.parents:
        raise ValueError("文件不在 data/ 内")
    if path.suffix.lower() != ".csv":
        raise ValueError("只允许 .csv")
    return path


class Handler(BaseHTTPRequestHandler):
    server_version = "J280LiveBridge/2.1"

    def _allowed_origin(self) -> str | None:
        origin = (self.headers.get("Origin") or "").strip()
        return origin if origin in ALLOWED_ORIGINS else None

    def _host_ok(self) -> bool:
        host = (self.headers.get("Host") or "").strip().lower()
        return host in ALLOWED_HOSTS

    def _guard(self) -> bool:
        if not self._host_ok():
            self.send_json(403, {"ok": False, "error": "Host 不在白名单（只允许 127.0.0.1:8788）"})
            return False
        origin = (self.headers.get("Origin") or "").strip()
        if origin and origin not in ALLOWED_ORIGINS:
            self.send_json(403, {"ok": False, "error": "Origin 不在白名单（只允许 http://127.0.0.1:8787）"})
            return False
        return True

    def _is_json(self) -> bool:
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        return ctype == "application/json"

    def _cors_header(self) -> None:
        origin = self._allowed_origin()
        if origin:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")

    def send_json(self, code: int, payload: Any) -> None:
        body = json.dumps(payload, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self._cors_header()
        self.end_headers()
        self.wfile.write(body)

    def send_text(self, code: int, text: str, ctype: str = "text/plain; charset=utf-8") -> None:
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self._cors_header()
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self) -> None:
        if not self._host_ok():
            self.send_json(403, {"ok": False, "error": "Host 不在白名单"})
            return
        origin = self._allowed_origin()
        if not origin:
            self.send_json(403, {"ok": False, "error": "Origin 不在白名单"})
            return
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", origin)
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Max-Age", "600")
        self.send_header("Vary", "Origin")
        self.end_headers()

    def do_GET(self) -> None:
        if not self._guard():
            return
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        if parsed.path == "/":
            self.send_json(200, {
                "service": "j280-live-bridge", "version": 2,
                "monitor": "http://127.0.0.1:8787/",
                "health": "http://127.0.0.1:8788/health",
                "tune_schema": "/tune/schema", "runs": "/runs",
            })
            return
        if parsed.path == "/health":
            self.send_json(200, {
                "ok": True, "service": "j280-live-bridge", "version": 2,
                "live_dir": str(LIVE), "latest": str(LATEST), "events": str(EVENTS),
                "project": str(PROJECT), "tools": {
                    "gw_sh": GW_SH.exists(), "programmer": PROGRAMMER.exists(),
                    "build_tcl": BUILD_TCL.exists(), "data": DATA.exists(),
                },
            })
            return
        if parsed.path == "/latest.json":
            with LOCK:
                payload = dict(SESSION)
            self.send_json(200, payload)
            return
        if parsed.path == "/events.jsonl":
            if not EVENTS.exists():
                self.send_response(404)
                self.end_headers()
                return
            body = EVENTS.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self._cors_header()
            self.end_headers()
            self.wfile.write(body)
            return
        if parsed.path == "/runs":
            self.send_json(200, {"ok": True, "runs": list_runs()})
            return
        if parsed.path == "/run":
            try:
                name = (qs.get("file") or [""])[0]
                path = safe_data_file(name)
                if not path.exists():
                    raise FileNotFoundError(name)
                self.send_text(200, path.read_text(encoding="utf-8", errors="replace"), "text/csv; charset=utf-8")
            except Exception as exc:
                self.send_json(404, {"ok": False, "error": str(exc)})
            return
        if parsed.path == "/tune/schema":
            try:
                self.send_json(200, schema_payload())
            except Exception as exc:
                self.send_json(500, {"ok": False, "error": str(exc)})
            return
        if parsed.path == "/tune/status":
            jid = (qs.get("id") or [""])[0] or CURRENT_JOB
            with JOB_LOCK:
                job = JOBS.get(jid) if jid else None
            if not job:
                self.send_json(404, {"ok": False, "error": "没有该任务"})
                return
            self.send_json(200, {"ok": True, "job": public_job(job)})
            return
        if parsed.path == "/tune/backups":
            items = []
            if BACKUPS.exists():
                for d in sorted(BACKUPS.iterdir(), reverse=True):
                    if d.is_dir():
                        params = {}
                        pf = d / "params.json"
                        if pf.exists():
                            try:
                                params = json.loads(pf.read_text(encoding="utf-8")).get("values", {})
                            except Exception:
                                pass
                        items.append({"name": d.name, "params": params})
            self.send_json(200, {"ok": True, "backups": items[:50]})
            return
        if parsed.path == "/version":
            self.send_json(200, {"ok": True, **read_version()})
            return
        if parsed.path == "/command":
            self.send_json(405, {"ok": False, "error": "POST /command only"})
            return
        if parsed.path == "/commands":
            with LOCK:
                commands = list(COMMAND_QUEUE)
                COMMAND_QUEUE.clear()
            self.send_json(200, {"ok": True, "commands": commands})
            return
        self.send_json(404, {"error": "not found"})

    def do_POST(self) -> None:
        global CURRENT_JOB, JOB_SEQ
        if not self._guard():
            return
        if not self._is_json():
            self.send_json(415, {"ok": False, "error": "POST 必须使用 Content-Type: application/json"})
            return
        parsed = urlparse(self.path)
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if parsed.path == "/tune/build":
                if size <= 0 or size > 64_000:
                    raise ValueError("invalid content length")
                payload = json.loads(self.rfile.read(size).decode("utf-8"))
                if not isinstance(payload, dict):
                    raise ValueError("payload must be an object")
                params = payload.get("params")
                if not isinstance(params, dict):
                    raise ValueError("params must be an object")
                flash_value = payload.get("flash", False)
                if not isinstance(flash_value, bool):
                    raise ValueError("flash 必须是 JSON 布尔值 true/false")
                flash = flash_value
                if flash:
                    if str(payload.get("confirm", "")).strip().upper() != "BURN":
                        raise ValueError("烧录确认词必须是 BURN")
                    if payload.get("safe_ack") is not True:
                        raise ValueError("烧录前必须由页面提交 safe_ack=true（已扶稳摆杆、SW3 关）")

                values, warnings = validate_params(params)
                if not SOURCE_LOCK.acquire(blocking=False):
                    self.send_json(409, {"ok": False, "error": "已有构建/恢复任务正在改动源码"})
                    return
                with JOB_LOCK:
                    if CURRENT_JOB is not None:
                        SOURCE_LOCK.release()
                        self.send_json(409, {"ok": False, "error": "已有构建/烧录任务在运行"})
                        return
                    JOB_SEQ += 1
                    jid = f"tune_{time.strftime('%Y%m%d_%H%M%S')}_{JOB_SEQ}"
                    JOBS[jid] = {
                        "id": jid, "state": "queued", "phase": "queued", "log": "",
                        "created_at": now(), "flash": flash, "params": values,
                        "warnings": warnings,
                    }
                    CURRENT_JOB = jid
                thread = threading.Thread(target=run_job, args=(jid, values, flash), daemon=True)
                try:
                    thread.start()
                except Exception:
                    with JOB_LOCK:
                        CURRENT_JOB = None
                    SOURCE_LOCK.release()
                    raise
                self.send_json(200, {
                    "ok": True, "job_id": jid, "warnings": warnings,
                    "job": public_job(JOBS[jid]),
                })
                return

            if parsed.path == "/tune/restore":
                if size <= 0 or size > 16_000:
                    raise ValueError("invalid content length")
                payload = json.loads(self.rfile.read(size).decode("utf-8"))
                name = str(payload.get("backup", "")).strip()
                if not re.fullmatch(r"\d{8}_\d{6}", name):
                    raise ValueError("非法备份名")
                bdir = BACKUPS / name
                if not bdir.is_dir():
                    raise ValueError("备份不存在")
                if not SOURCE_LOCK.acquire(blocking=False):
                    self.send_json(409, {"ok": False, "error": "构建/烧录进行中，拒绝恢复源码"})
                    return
                try:
                    restored = []
                    for f in bdir.iterdir():
                        if f.name == "params.json" or not f.is_file():
                            continue
                        rel = f.name.replace("__", "/")
                        dst = (PROJECT / rel).resolve()
                        if PROJECT.resolve() not in dst.parents:
                            continue
                        shutil.copy2(f, dst)
                        restored.append(rel)
                    self.send_json(200, {"ok": True, "restored": restored, "backup": name})
                finally:
                    SOURCE_LOCK.release()
                return

            if parsed.path == "/runs/save":
                if size <= 0 or size > 8_000_000:
                    raise ValueError("invalid content length")
                payload = json.loads(self.rfile.read(size).decode("utf-8"))
                name = str(payload.get("name", "")).strip()
                csv = payload.get("csv")
                if not re.fullmatch(r"[A-Za-z0-9_.-]{1,60}", name):
                    raise ValueError("非法文件名")
                if not isinstance(csv, str) or not csv.startswith("time,theta_rad"):
                    raise ValueError("CSV 头不符合当前遥测格式")
                DATA.mkdir(parents=True, exist_ok=True)
                dst = DATA / f"web_{time.strftime('%Y%m%d_%H%M%S')}_{name}.csv"
                dst.write_text(csv, encoding="utf-8", newline="")
                self.send_json(200, {"ok": True, "file": dst.name, "size": len(csv)})
                return

            if parsed.path == "/command":
                if size <= 0 or size > 16_000:
                    raise ValueError("invalid content length")
                payload = json.loads(self.rfile.read(size).decode("utf-8"))
                if not isinstance(payload, dict):
                    raise ValueError("command must be an object")
                self.send_json(200, {"ok": True, "command": queue_command(payload)})
                return

            if parsed.path != "/event":
                self.send_json(404, {"error": "not found"})
                return
            if size <= 0 or size > 256_000:
                raise ValueError("invalid content length")
            event = json.loads(self.rfile.read(size).decode("utf-8"))
            if not isinstance(event, dict) or not isinstance(event.get("type"), str):
                raise ValueError("event must be an object with a string type")
            update_session(event)
            self.send_json(200, {"ok": True, "event_count": SESSION["event_count"]})
        except (ValueError, json.JSONDecodeError) as error:
            self.send_json(400, {"ok": False, "error": str(error)})
        except Exception as error:
            self.send_json(500, {"ok": False, "error": f"{type(error).__name__}: {error}"})

    def log_message(self, format: str, *args: Any) -> None:
        print("[bridge] " + format % args, flush=True)


def public_job(job: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": job.get("id"), "state": job.get("state"), "phase": job.get("phase"),
        "log": job.get("log", "")[-20000:], "created_at": job.get("created_at"),
        "started_at": job.get("started_at"), "finished_at": job.get("finished_at"),
        "error": job.get("error"), "result": job.get("result"),
        "warnings": job.get("warnings"), "flash": job.get("flash"),
    }


def main() -> None:
    LIVE.mkdir(parents=True, exist_ok=True)
    if not PROJECT.exists():
        print(f"[bridge] WARNING project not found: {PROJECT}", flush=True)
    write_latest()
    server = ThreadingHTTPServer(("127.0.0.1", 8788), Handler)
    print("J280 live bridge v2 listening on http://127.0.0.1:8788", flush=True)
    print(f"Live files: {LIVE}", flush=True)
    print(f"Project:    {PROJECT}", flush=True)
    print(f"Tools:      gw_sh={GW_SH.exists()} programmer={PROGRAMMER.exists()}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
