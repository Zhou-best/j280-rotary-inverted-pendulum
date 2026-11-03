# -*- coding: utf-8 -*-


from media.sensor import *
from media.display import *
from media.media import *
from libs.AIBase import AIBase
from libs.AI2D import Ai2d
import aidemo
import nncase_runtime as nn
import ulab.numpy as np
import image
import os
import time
import math
import gc
import ujson
import socket


MODEL_PATH = "/sdcard/kmodel/yolov8n-pose.kmodel"
WIDTH = 640
HEIGHT = 480
RGB_SIZE = [WIDTH, HEIGHT]


DISPLAY_MODE = "lcd"


ENABLE_TCP = True
WIFI_SSID = "ESP32-HeartRate"
WIFI_PASSWORD = "12345678"
ESP32_IP = "192.168.4.1"
ESP32_PORT = 9999


KP_MIN = 0.35
HOLD_MS = 500
RELEASE_MS = 300
COOLDOWN_MS = 800
SWAP_LEFT_RIGHT = False


SIDE_DY_MIN = -0.45
SIDE_DY_MAX = 0.40
SIDE_LAT_MIN = 0.70
UP_DY_MAX = -0.45
UP_LAT_MAX = 0.85
LOW_DY_MIN = 0.55

SIDE_ELBOW_MIN = 125
UP_ELBOW_MIN = 110


def align_up(value, alignment):
    return ((int(value) + alignment - 1) // alignment) * alignment


class PoseApp(AIBase):

    SKELETON = [(16, 14), (14, 12), (17, 15), (15, 13), (12, 13),
                (6, 12), (7, 13), (6, 7), (6, 8), (7, 9), (8, 10),
                (9, 11), (2, 3), (1, 2), (1, 3), (2, 4), (3, 5),
                (4, 6), (5, 7)]

    def __init__(self, kmodel_path, model_input_size, rgb888p_size,
                 display_size, confidence_threshold=0.25, nms_threshold=0.5,
                 debug_mode=0):
        super().__init__(kmodel_path, model_input_size, rgb888p_size, debug_mode)
        self.model_input_size = model_input_size
        self.rgb888p_size = [align_up(rgb888p_size[0], 16), rgb888p_size[1]]
        self.display_size = [align_up(display_size[0], 16), display_size[1]]
        self.confidence_threshold = confidence_threshold
        self.nms_threshold = nms_threshold
        self.debug_mode = debug_mode
        self.ai2d = Ai2d(debug_mode)
        self.ai2d.set_ai2d_dtype(nn.ai2d_format.NCHW_FMT,
                                 nn.ai2d_format.NCHW_FMT,
                                 np.uint8, np.uint8)

    def config_preprocess(self):
        top, bottom, left, right = self.get_padding_param()
        self.ai2d.pad([0, 0, 0, 0, top, bottom, left, right], 0, [0, 0, 0])
        self.ai2d.resize(nn.interp_method.tf_bilinear,
                         nn.interp_mode.half_pixel)
        self.ai2d.build([1, 3, self.rgb888p_size[1], self.rgb888p_size[0]],
                        [1, 3, self.model_input_size[1], self.model_input_size[0]])

    def postprocess(self, results):
        return aidemo.person_kp_postprocess(
            results[0], [self.rgb888p_size[1], self.rgb888p_size[0]],
            self.model_input_size, self.confidence_threshold,
            self.nms_threshold)

    def get_padding_param(self):
        dst_w, dst_h = self.model_input_size
        src_w, src_h = self.rgb888p_size
        ratio = min(dst_w / src_w, dst_h / src_h)
        new_w, new_h = int(ratio * src_w), int(ratio * src_h)
        dw, dh = (dst_w - new_w) / 2, (dst_h - new_h) / 2
        return int(round(dh - 0.1)), int(round(dh + 0.1)), \
               int(round(dw - 0.1)), int(round(dw - 0.1))


def dist(a, b):
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2)


def joint_angle(a, b, c):

    v1x, v1y = a[0] - b[0], a[1] - b[1]
    v2x, v2y = c[0] - b[0], c[1] - b[1]
    n1 = math.sqrt(v1x * v1x + v1y * v1y)
    n2 = math.sqrt(v2x * v2x + v2y * v2y)
    if n1 < 1e-3 or n2 < 1e-3:
        return 0.0
    cosv = (v1x * v2x + v1y * v2y) / (n1 * n2)
    if cosv > 1.0:
        cosv = 1.0
    elif cosv < -1.0:
        cosv = -1.0
    return math.degrees(math.acos(cosv))


def arm_state(shoulder, elbow, wrist, sw, center_x):


    if min(shoulder[2], elbow[2], wrist[2]) < KP_MIN:
        return "none"
    dy = (wrist[1] - shoulder[1]) / sw
    lateral = abs(wrist[0] - shoulder[0]) / sw

    outward = (wrist[0] - center_x) * (shoulder[0] - center_x) > 0
    ang = joint_angle(shoulder, elbow, wrist)

    if dy <= UP_DY_MAX and lateral <= UP_LAT_MAX and ang >= UP_ELBOW_MIN:
        return "up"
    if (SIDE_DY_MIN <= dy <= SIDE_DY_MAX and lateral >= SIDE_LAT_MIN
            and outward and ang >= SIDE_ELBOW_MIN):
        return "side"
    if dy >= LOW_DY_MIN:
        return "low"
    return "mid"


def classify(kps):


    ls, le, lw = kps[5], kps[7], kps[9]
    rs, re, rw = kps[6], kps[8], kps[10]
    if min(ls[2], rs[2]) < KP_MIN:
        return "none", "no shoulders", "none", "none"
    sw = dist(ls, rs)
    if sw < 30:
        return "none", "too far", "none", "none"
    cx = (ls[0] + rs[0]) * 0.5

    left = arm_state(ls, le, lw, sw, cx)
    right = arm_state(rs, re, rw, sw, cx)
    dbg = "L:{} R:{} sw:{:.0f}".format(left, right, sw)


    if left == "none" or right == "none":
        return "none", dbg, left, right

    gesture = "unknown"

    if left == "side" and right == "side":
        gesture = "both_side"
    elif left == "side" and right != "side" and right != "up":
        gesture = "left_arm"
    elif right == "side" and left != "side" and left != "up":
        gesture = "right_arm"
    elif left == "low" and right == "low":
        gesture = "rest"

    if SWAP_LEFT_RIGHT:
        if gesture == "left_arm":
            gesture = "right_arm"
        elif gesture == "right_arm":
            gesture = "left_arm"
        left, right = right, left
    return gesture, dbg, left, right


COMMANDS = {
    "left_arm": "left",
    "right_arm": "right",
    "both_side": "stop",
}


ACTION_MODE = {"left": 4, "right": 3, "stop": 0}


ENABLE_VOICE = True

VOICE_DIRS = ("/sdcard/j280_gesture/voice", "/sdcard/audio")
VOICE_MODE_NAME = {0: "0", 1: "p45", 2: "m45", 3: "p90", 4: "m90"}


def voice_key(old_mode, new_mode):

    a = VOICE_MODE_NAME.get(old_mode, "0")
    b = VOICE_MODE_NAME.get(new_mode, "0")
    if a == b:
        return "at_" + b
    return "from_{}_to_{}".format(a, b)


class GestureGate(object):


    def __init__(self):
        self.cur = None
        self.cur_since = 0
        self.last_fire = -100000
        self.armed = True
        self.release_since = 0
        self.last_gesture = None

    def _released(self, gesture, left, right):

        if gesture in ("rest", "none"):
            return True
        if self.last_gesture == "left_arm":
            return left == "low"
        if self.last_gesture == "right_arm":
            return right == "low"
        if self.last_gesture == "both_side":
            return left == "low" and right == "low"
        return True

    def update(self, gesture, left, right, now):
        if gesture != self.cur:
            self.cur = gesture
            self.cur_since = now

        if not self.armed:
            if self._released(gesture, left, right):
                if self.release_since == 0:
                    self.release_since = now
                elif time.ticks_diff(now, self.release_since) >= RELEASE_MS:
                    self.armed = True
                    self.release_since = 0
            else:
                self.release_since = 0
            return None

        self.release_since = 0
        if time.ticks_diff(now, self.cur_since) < HOLD_MS:
            return None
        if time.ticks_diff(now, self.last_fire) < COOLDOWN_MS:
            return None
        if gesture not in COMMANDS:
            return None

        self.armed = False
        self.last_fire = now
        self.last_gesture = gesture
        return COMMANDS[gesture]

    def hint(self):
        if self.armed:
            return "READY"
        if self.last_gesture == "left_arm":
            return "LOWER LEFT ARM TO REARM"
        if self.last_gesture == "right_arm":
            return "LOWER RIGHT ARM TO REARM"
        if self.last_gesture == "both_side":
            return "LOWER BOTH ARMS TO REARM"
        return "LOWER ARM TO REARM"


class CommandSender(object):


    def __init__(self):
        self.sock = None
        self.wifi = None
        self.seq = 0
        self.queue = []
        self.lock = None
        self.running = False
        self.thread_started = False
        self._thread = None
        try:
            import _thread
            self._thread = _thread
            self.lock = _thread.allocate_lock()
        except Exception as e:
            print("TCP thread unavailable:", e)

    def start(self):
        if self._thread and not self.thread_started:
            self.running = True
            self.thread_started = True
            self._thread.start_new_thread(self._loop, ())

    def send(self, action):
        self.seq += 1
        payload = {"type": "cmd", "cmd": "gesture", "arg": action,
                   "seq": self.seq, "t_ms": time.ticks_ms()}
        line = (ujson.dumps(payload) + "\n").encode()
        if not ENABLE_TCP:
            print("CMD_DRYRUN," + line.decode().strip())
            return True
        if self.lock:
            self.lock.acquire()
        try:
            if len(self.queue) < 16:
                self.queue.append(line)
        finally:
            if self.lock:
                self.lock.release()
        return True

    def _connect(self):
        try:
            if self.wifi is None:
                import network
                self.wifi = network.WLAN(network.STA_IF)
                self.wifi.active(True)
            if not self.wifi.isconnected():
                self.wifi.connect(WIFI_SSID, WIFI_PASSWORD)
                t0 = time.ticks_ms()
                while (not self.wifi.isconnected() and
                       time.ticks_diff(time.ticks_ms(), t0) < 3000):
                    time.sleep(0.1)
                if not self.wifi.isconnected():
                    raise OSError("wifi timeout")
            s = socket.socket()
            s.settimeout(1.0)
            s.connect((ESP32_IP, ESP32_PORT))
            s.settimeout(0.05)
            print("TCP connected {}:{}".format(ESP32_IP, ESP32_PORT))
            return s
        except Exception as e:
            print("TCP connect failed:", e)
            return None

    def _loop(self):
        last_hb = -10000
        while self.running:
            if self.sock is None:
                self.sock = self._connect()
                if self.sock is None:
                    time.sleep_ms(1000)
                    continue
                last_hb = time.ticks_ms()

            try:
                d = self.sock.recv(512)
                if d == b"":
                    raise OSError(104, "peer closed")
            except OSError as e:
                code = e.args[0] if e.args else 0
                if code in (11, 110):
                    pass
                else:
                    try:
                        self.sock.close()
                    except Exception:
                        pass
                    self.sock = None
                    continue
            except Exception:
                try:
                    self.sock.close()
                except Exception:
                    pass
                self.sock = None
                continue
            line = None
            if self.lock:
                self.lock.acquire()
            try:
                if self.queue:
                    line = self.queue.pop(0)
            finally:
                if self.lock:
                    self.lock.release()
            if line is not None:
                try:
                    self.sock.send(line)
                    print("CMD_SENT," + line.decode().strip())
                except Exception as e:
                    print("TCP send failed:", e)
                    try:
                        self.sock.close()
                    except Exception:
                        pass
                    self.sock = None
                    if self.lock:
                        self.lock.acquire()
                    try:
                        self.queue.insert(0, line)
                    finally:
                        if self.lock:
                            self.lock.release()
            else:
                now = time.ticks_ms()
                if time.ticks_diff(now, last_hb) >= 5000:
                    try:
                        self.sock.send(b'{"type":"ping"}\n')
                        last_hb = now
                    except Exception:
                        try:
                            self.sock.close()
                        except Exception:
                            pass
                        self.sock = None
            time.sleep_ms(20)
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def close(self):
        self.running = False
        time.sleep_ms(100)


class VoicePlayer(object):


    def __init__(self):
        self.PyAudio = None
        self.wave = None
        self.spk = None
        self.send_frame = None
        self.uctypes = None
        self.pa = None
        self.stream = None
        self.audio_buffer = None
        self.wav = None
        self.pending = None
        self.chunk = 1000
        self.playing = False
        self.failed = False
        self.queue = []
        try:
            from media.pyaudio import PyAudio
            import media.wave as wave
            from ybUtils.YbSpeaker import YbSpeaker
            from mpp import kd_mpi_ao_send_frame
            import uctypes
            self.PyAudio = PyAudio
            self.wave = wave
            self.spk = YbSpeaker()
            self.send_frame = kd_mpi_ao_send_frame
            self.uctypes = uctypes
            self.pa = PyAudio()
            self.pa.initialize(self.chunk)
        except Exception as e:
            print("VOICE unavailable:", e)
            self.failed = True

    def start(self):
        pass

    def warmup(self):
        pass

    def _find(self, key):
        for d in VOICE_DIRS:
            p = d + "/" + key + ".wav"
            try:
                os.stat(p)
                return p
            except OSError:
                continue
        return None

    def play(self, key):
        if self.failed or not ENABLE_VOICE:
            return
        path = self._find(key)
        if path is None:
            print("VOICE missing:", key, VOICE_DIRS)
            return
        if len(self.queue) < 8:
            self.queue.append(path)
            print("VOICE queue:", key)

    def _open_output(self, wav):
        self.spk.enable()
        self.chunk = max(800, int(wav.get_framerate() / 16))
        self.stream = self.pa.open(
            format=self.pa.get_format_from_width(wav.get_sampwidth()),
            channels=wav.get_channels(), rate=wav.get_framerate(),
            output=True, frames_per_buffer=self.chunk)
        self.stream.volume(vol=75)
        frame = self.stream._audio_frame
        self.audio_buffer = self.uctypes.bytearray_at(frame.virt_addr, frame.len)
        print("VOICE out: rate={} ch={} width={} chunk={} buf={}".format(
            wav.get_framerate(), wav.get_channels(), wav.get_sampwidth(),
            self.chunk, frame.len))

    def _start_next(self):
        if self.playing or not self.queue:
            return
        path = self.queue.pop(0)
        try:
            wav = self.wave.open(path, "rb")
            if self.stream is None:
                self._open_output(wav)
            self.wav = wav
            self.pending = None
            self.playing = True
            print("VOICE begin:", path)
        except Exception as e:
            print("VOICE open:", e)
            self.failed = True

    def _finish(self):
        if self.wav is not None:
            try:
                self.wav.close()
            except Exception:
                pass
            self.wav = None
        self.pending = None
        self.playing = False

    def pump(self):
        if self.failed:
            return
        self._start_next()
        if not self.playing:
            return
        try:
            if self.pending is None:
                data = self.wav.read_frames(self.chunk)
                if not data:
                    self._finish()
                    return
                n = len(self.audio_buffer)
                self.pending = data[:n] if len(data) >= n else data + bytes(n - len(data))
            self.audio_buffer[:] = self.pending
            ret = self.send_frame(self.stream._ao_dev, self.stream._ao_chn,
                                  self.stream._audio_frame, 0)
            if ret == 0:
                self.pending = None
        except Exception as e:
            print("VOICE play:", e)
            self._finish()

    def close(self):
        self._finish()
        self.queue = []
        if self.stream is not None:
            try:
                self.stream.stop_stream()
                self.stream.close()
            except Exception:
                pass
            self.stream = None
        if self.pa is not None:
            try:
                self.pa.terminate()
            except Exception:
                pass
            self.pa = None
        if self.spk is not None:
            try:
                self.spk.disable()
            except Exception:
                pass


def pick_person(boxes, kpses, width, height):

    best = None
    best_d = 1e9
    n = min(len(boxes), len(kpses))
    for i in range(n):
        kps = kpses[i]
        if len(kps) != 17:
            continue
        ls, rs = kps[5], kps[6]
        if ls[2] < KP_MIN or rs[2] < KP_MIN:
            continue
        ok = True
        for p in (ls, rs):
            if not (0 <= p[0] < width and 0 <= p[1] < height):
                ok = False
                break
        if not ok:
            continue
        sw = dist(ls, rs)
        if sw < 30:
            continue
        cx = (ls[0] + rs[0]) * 0.5
        d = abs(cx - width * 0.5)
        if d < best_d:
            best_d = d
            best = (kps, sw)
    return best


def draw_skeleton(img, kps, sx, sy):
    for a, b in PoseApp.SKELETON:
        p, q = kps[a - 1], kps[b - 1]
        if p[2] >= KP_MIN and q[2] >= KP_MIN:
            img.draw_line(int(p[0] * sx), int(p[1] * sy),
                          int(q[0] * sx), int(q[1] * sy),
                          (255, 255, 255), 2)
    for i in range(17):
        p = kps[i]
        if p[2] >= KP_MIN:
            img.draw_circle(int(p[0] * sx), int(p[1] * sy), 3,
                            (255, 0, 255), 2)


def draw_ui(img, gesture, dbg, hold_s, last_cmd, last_cmd_ms, now, fps, w, h, hint, same_mode):
    img.draw_rectangle(0, 0, w, 64, color=(0, 0, 0), fill=True)
    label = gesture if gesture else "NO PERSON"
    color = (255, 255, 255)
    if gesture in COMMANDS:
        color = (255, 255, 0)
    img.draw_string_advanced(8, 6, 26, "GESTURE: " + label, color)
    img.draw_string_advanced(8, 38, 18, dbg, (255, 255, 255))

    if gesture in COMMANDS:
        ratio = hold_s * 1000.0 / HOLD_MS
        if ratio > 1.0:
            ratio = 1.0
        bw = int((w - 16) * ratio)
        img.draw_rectangle(8, 66, w - 16, 10, color=(60, 60, 60), fill=True)
        img.draw_rectangle(8, 66, max(1, bw), 10, color=(0, 220, 0), fill=True)

    hint_color = (0, 255, 0) if hint == "READY" else (255, 180, 0)
    img.draw_string_advanced(8, 82, 18, hint, hint_color)

    if last_cmd and time.ticks_diff(now, last_cmd_ms) < 1500:
        if same_mode:
            txt = "ALREADY: " + last_cmd
            col = (255, 180, 0)
        else:
            txt = "SENT: " + last_cmd
            col = (0, 255, 0)
        img.draw_string_advanced(8, 106, 22, txt, col)

    img.draw_string_advanced(8, h - 26, 16,
                             "FPS:{:.1f}  TCP:{}".format(fps, "ON" if ENABLE_TCP else "OFF"),
                             (255, 255, 255))


def main():

    try:
        os.stat(MODEL_PATH)
    except OSError:
        print("ERROR: model not found:", MODEL_PATH)
        print("请确认 /sdcard/kmodel/yolov8n-pose.kmodel 仍在（本脚本只读不改）")
        return


    sensor = Sensor()
    sensor.reset()
    sensor.set_framesize(width=WIDTH, height=HEIGHT, chn=CAM_CHN_ID_0)
    sensor.set_pixformat(Sensor.RGB565, chn=CAM_CHN_ID_0)
    sensor.set_framesize(width=WIDTH, height=HEIGHT, chn=CAM_CHN_ID_1)
    sensor.set_pixformat(PIXEL_FORMAT_RGB_888_PLANAR, chn=CAM_CHN_ID_1)

    if DISPLAY_MODE == "virt":

        Display.init(Display.VIRT, width=WIDTH, height=HEIGHT, fps=30)
    else:

        Display.init(Display.ST7701, width=WIDTH, height=HEIGHT,
                     to_ide=True, fps=10, quality=50)


    voice = VoicePlayer()
    voice.start()

    MediaManager.init()

    app = PoseApp(MODEL_PATH, [320, 320], RGB_SIZE, [WIDTH, HEIGHT])
    app.config_preprocess()

    gate = GestureGate()
    sender = CommandSender()
    sender.start()
    print("J280 GESTURE BUILD 2026-11-02-r14")
    print("voice:", "ON" if (ENABLE_VOICE and not voice.failed) else "OFF")
    print("display:", DISPLAY_MODE)
    print("gate: HOLD={}ms RELEASE={}ms COOLDOWN={}ms".format(
        HOLD_MS, RELEASE_MS, COOLDOWN_MS))
    print("model:", MODEL_PATH)
    print("TCP:", "ON" if ENABLE_TCP else "OFF (dry-run, only prints)")
    print("gesture: left_arm/right_arm/both_side; hold {}ms".format(HOLD_MS))

    sensor.run()
    voice.warmup()

    frame_no = 0
    last_cmd = None
    last_cmd_ms = 0
    last_print = 0
    fps_t0 = time.ticks_ms()
    fps_frames = 0
    fps = 0.0
    vote_hist = []
    target_mode = 0
    last_same = False

    try:
        while True:

            ai_img = sensor.snapshot(chn=CAM_CHN_ID_1)
            np_img = ai_img.to_numpy_ref()
            res = app.run(np_img)

            del np_img
            del ai_img
            now = time.ticks_ms()

            person = pick_person(res[0], res[1], RGB_SIZE[0], RGB_SIZE[1])
            if person is None:
                gesture, dbg = "none", "no person"
                left_state, right_state = "none", "none"
            else:
                kps, sw = person
                gesture, dbg, left_state, right_state = classify(kps)


            vote_hist.append(gesture)
            if len(vote_hist) > 5:
                vote_hist.pop(0)
            counts = {}
            for g in vote_hist:
                counts[g] = counts.get(g, 0) + 1
            voted = gesture
            for g in ("left_arm", "right_arm", "both_side"):
                if counts.get(g, 0) >= 3:
                    voted = g
                    break
            if voted == gesture:
                if counts.get("rest", 0) >= 3:
                    voted = "rest"
                elif counts.get("none", 0) >= 3:
                    voted = "none"

            cmd = gate.update(voted, left_state, right_state, now)
            hold_s = time.ticks_diff(now, gate.cur_since) / 1000.0

            if cmd is not None:
                old_mode = target_mode
                new_mode = ACTION_MODE.get(cmd, target_mode)
                last_same = (new_mode == target_mode)
                target_mode = new_mode


                sender.send(cmd)
                if ENABLE_VOICE:
                    voice.play(voice_key(old_mode, new_mode))
                last_cmd = cmd
                last_cmd_ms = now
                print("GESTURE_FIRE,{},{} same={}".format(voted, cmd, int(last_same)))


            if time.ticks_diff(now, last_print) >= 200:
                last_print = now
                print("GESTURE,{},{},voted={},hold={:.2f},armed={}".format(
                    gesture, dbg, voted, hold_s, int(gate.armed)))


            disp = sensor.snapshot()
            if person is not None:
                kps = person[0]
                sx = disp.width() / float(RGB_SIZE[0])
                sy = disp.height() / float(RGB_SIZE[1])
                draw_skeleton(disp, kps, sx, sy)
            draw_ui(disp, gesture, dbg, hold_s, last_cmd, last_cmd_ms,
                    now, fps, disp.width(), disp.height(), gate.hint(), last_same)
            Display.show_image(disp, x=0, y=0)
            voice.pump()

            frame_no += 1
            fps_frames += 1
            if time.ticks_diff(now, fps_t0) >= 1000:
                fps = fps_frames * 1000.0 / time.ticks_diff(now, fps_t0)
                fps_t0 = now
                fps_frames = 0
            if frame_no % 30 == 0:
                gc.collect()

    except KeyboardInterrupt:
        print("stopped by user")
    finally:
        sender.close()
        voice.close()
        try:
            sensor.stop()
        except Exception:
            pass
        try:
            Display.deinit()
        except Exception:
            pass
        time.sleep_ms(100)
        try:
            MediaManager.deinit()
        except Exception:
            pass


if __name__ == "__main__":
    main()
