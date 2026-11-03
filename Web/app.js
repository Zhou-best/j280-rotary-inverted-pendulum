/* J280 最终微调台 — 22 B 0xAA55 / 115200 / 100 Hz
 * 纯原生 JS，无外部依赖。串口由浏览器独占，桥接只做构建与历史文件。
 */
(() => {
  'use strict';

  const BRIDGE = 'http://127.0.0.1:8788';
  const FS = 100;                 // 遥测标称 100 Hz
  const MAX_SAMPLES = 60000;      // 约 600 s；超出才丢最旧样本，历史 CSV 不再截断
  const ARM_LEN = 1.02;
  const ROD_LEN = 1.30;
  const GROUND_Y = -0.78;
  const STATE_NAMES = ['HANGING', 'SWINGUP', 'BALANCE', 'PROTECT'];
  const STATE_CLASS = ['hanging', 'swing', 'balance', 'protect'];
  const COLORS = ['#53d9ff', '#ff9e5e', '#4ade9a', '#b388ff', '#ffd166', '#ff6b7a', '#6aa8ff', '#f78fb3'];
  const TAU = Math.PI * 2;

  const $ = id => document.getElementById(id);
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const deg = r => r * 180 / Math.PI;
  const rad = d => d * Math.PI / 180;
  const fmt = (v, n = 2) => (v === null || v === undefined || !isFinite(v)) ? '--' : Number(v).toFixed(n);
  const nowMs = () => performance.now();
  // 中文输入法可能把 BURN 输成全角（ＢＵＲＮ），部分浏览器还会夹零宽字符。
  // NFKC 把全角/兼容字符折成 ASCII，再统一大写；桥接端仍只接受 BURN。
  const burnWord = v => String(v || '').normalize('NFKC')
    .replace(/[​-‍﻿]/g, '').trim().toUpperCase();

  const S = {
    port: null, reader: null, readTask: null, connected: false, connectT: 0, connectWall: null,
    rx: new Uint8Array(0), bytes: 0, frames: 0, bad: 0, fps: 0, fpsT: 0, fpsN: 0,
    samples: [], last: null, lastRx: 0,
    paused: false, windowSec: 30, fftMax: 1024,
    markers: [], smooth: true,
    renderTheta: 0, renderAlpha: 0, trail: [], lastTrail: 0,
    specCols: [], lastSpec: 0, lastSpecDraw: 0, lastSpecT: 0,
    history: [], compare: [],
    schema: null, draft: {}, activeGroup: null,
    job: null, poll: null, busy: false,
    rawVisible: false, rawTail: [],
    lastMetrics: null, lastWarn: '',
    lastChartT: 0, lastReadoutT: 0,
  };

  /* ======================================================================
   * 基础工具
   * ==================================================================== */
  function loadHistory() {
    try { return JSON.parse(localStorage.getItem('j280_runs_v3') || '[]'); } catch { return []; }
  }
  function saveHistory() {
    try { localStorage.setItem('j280_runs_v3', JSON.stringify(S.history.slice(0, 40))); } catch {}
  }
  S.history = loadHistory();

  function setBadge(el, text, online) {
    if (!el) return;
    el.textContent = text;
    el.classList.toggle('online', !!online);
    el.classList.toggle('offline', !online);
  }

  async function fetchJSON(url, opts = {}, timeout = 4000) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), timeout);
    try {
      const r = await fetch(url, { ...opts, signal: ctl.signal });
      const text = await r.text();
      let data;
      try { data = JSON.parse(text); } catch { throw new Error('非 JSON 响应：' + text.slice(0, 120)); }
      if (!r.ok) throw new Error(data.error || ('HTTP ' + r.status));
      return data;
    } finally { clearTimeout(timer); }
  }

  function fmtWall(t) {
    if (!S.connectWall) return '--:--:--.---';
    const d = new Date(S.connectWall + (t - S.connectT));
    const p = (n, w = 2) => String(n).padStart(w, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
  }

  /* ======================================================================
   * 串口
   * ==================================================================== */
  function setSerialStatus(kind, text) {
    const dot = $('serialDot');
    dot.className = 'dot ' + kind;
    $('serialText').textContent = text;
  }

  async function connect() {
    if (!('serial' in navigator)) {
      alert('当前浏览器不支持 Web Serial。请用 Chrome / Edge，并通过 http://127.0.0.1:8787/ 打开。');
      return;
    }
    if (S.connected) return;
    try {
      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' });
      S.port = port;
      S.connected = true;
      S.connectT = nowMs();
      S.connectWall = Date.now();
      S.rx = new Uint8Array(0);
      S.bytes = 0; S.frames = 0; S.bad = 0; S.fpsN = 0; S.fpsT = nowMs(); S.fps = 0;
      // 新会话必须清空上一会话的样本与时间轴；否则旧样本会被
      // windowSamples/指标混进来，重连后的首段数据不可用。
      S.samples = []; S.last = null; S.lastRx = 0;
      S.markers = []; S.trail = []; S.specCols = []; S.lastSpecT = 0;
      S.renderTheta = 0; S.renderAlpha = 0; S.lastMetrics = null;
      renderMarkers();
      $('connectBtn').disabled = true;
      $('disconnectBtn').disabled = false;
      setSerialStatus('warn', '已连接，等待 0xAA55 帧…');
      logEvent('serial_status', { connected: true, baud_rate: 115200 });
      S.readTask = readLoop(port);
    } catch (e) {
      setSerialStatus('offline', '未连接');
      if (e && e.name !== 'NotFoundError') alert('打开串口失败：' + e.message);
    }
  }

  async function readLoop(port) {
    try {
      S.reader = port.readable.getReader();
      while (true) {
        const { value, done } = await S.reader.read();
        if (done) break;
        if (value && value.length) ingest(value);
      }
    } catch (e) {
      if (S.connected) setSerialStatus('warn', '读取中断：' + e.message);
    } finally {
      try { S.reader.releaseLock(); } catch {}
      S.reader = null;
    }
  }

  async function disconnect() {
    if (!S.port) return;
    S.connected = false;
    try { if (S.reader) await S.reader.cancel(); } catch {}
    try { if (S.readTask) await S.readTask; } catch {}
    try { await S.port.close(); } catch {}
    S.port = null; S.readTask = null;
    $('connectBtn').disabled = false;
    $('disconnectBtn').disabled = true;
    setSerialStatus('offline', '未连接');
    logEvent('serial_status', { connected: false });
  }

  function ingest(chunk) {
    S.bytes += chunk.length;
    if (S.bytes === chunk.length) setSerialStatus('online', '已连接 · 115200');
    const buf = new Uint8Array(S.rx.length + chunk.length);
    buf.set(S.rx); buf.set(chunk, S.rx.length);
    let off = 0;
    const offsets = [];
    while (off + 22 <= buf.length) {
      if (buf[off] !== 0xAA || buf[off + 1] !== 0x55) { off++; continue; }
      const f = buf.subarray(off, off + 22);
      let x = 0;
      for (let i = 2; i <= 20; i++) x ^= f[i];
      if (x !== f[21]) { S.bad++; off++; continue; }
      offsets.push(off);
      off += 22;
    }
    S.rx = buf.slice(off);
    if (S.rx.length > 4096) S.rx = S.rx.slice(-1024);
    // 一个串口 chunk 里可能有 0/1/多帧。第一遍先找出所有合法帧，
    // 再按 100 Hz 标称间隔给它们各自的时间戳，避免同 chunk 多帧撞在同一时刻。
    if (offsets.length) {
      const n = offsets.length;
      const now = nowMs();
      let firstT = now - (n - 1) * (1000 / FS);
      if (S.last && firstT <= S.last.t) firstT = S.last.t + (1000 / FS);
      for (let i = 0; i < n; i++) {
        const o = offsets[i];
        parseFrame(buf.subarray(o, o + 22), firstT + i * (1000 / FS));
      }
    }
    if (S.rawVisible) {
      const tail = chunk.length > 48 ? chunk.slice(-48) : chunk;
      S.rawTail.push(...tail);
      if (S.rawTail.length > 160) S.rawTail = S.rawTail.slice(-160);
      renderRaw();
    }
  }

  function parseFrame(f, t = nowMs()) {
    const dv = new DataView(f.buffer, f.byteOffset, 22);
    const theta = dv.getInt32(2, true) / 65536;
    const dtheta = dv.getInt32(6, true) / 65536;
    // 固件的 alpha 是编码器 pulse_count 的累积值（非 ±π 环绕），
    // 所以这里不能再做 2π unwrap，否则会把真实的连续转角拧回去。
    const alpha = dv.getInt32(10, true) / 65536;
    const dalpha = dv.getInt32(14, true) / 65536;
    const duty = dv.getInt16(18, true);
    const status = f[20];
    const fsm = status & 3;
    const prev = S.last;
    const s = {
      t, theta, dtheta, alpha, alphaU: alpha, dalpha, duty, status, fsm,
      lqr: !!(status & 4), swing: !!(status & 8), motor: !!(status & 16),
      deficit: !!(status & 32), calib: !!(status & 64),
    };
    if (prev && prev.fsm !== fsm) {
      addMarker(`FSM ${STATE_NAMES[prev.fsm]}→${STATE_NAMES[fsm]}`, t, false);
    }
    S.samples.push(s);
    if (S.samples.length > MAX_SAMPLES) S.samples.splice(0, S.samples.length - MAX_SAMPLES);
    S.last = s;
    S.lastRx = t;
    S.frames++;
    S.fpsN++;
    if (S.frames % 100 === 0) {
      // 1 Hz 抽样给桥接实时状态，避免每帧都写 events.jsonl。
      logEvent('sample', { t: Math.round(t), theta, dtheta, alpha, dalpha, duty, status });
    }
    if (S.fpsN >= 10) {
      const dt = t - S.fpsT;
      if (dt >= 500) { S.fps = S.fpsN * 1000 / dt; S.fpsN = 0; S.fpsT = t; }
    }
  }

  /* ======================================================================
   * 数据窗口 / 重采样 / FFT
   * ==================================================================== */
  function windowSamples() {
    if (!S.samples.length) return [];
    if (S.windowSec === 0) return S.samples;
    const t1 = S.samples[S.samples.length - 1].t;
    const t0 = t1 - S.windowSec * 1000;
    let lo = 0, hi = S.samples.length - 1;
    while (lo < hi) { const m = (lo + hi) >> 1; if (S.samples[m].t < t0) lo = m + 1; else hi = m; }
    return S.samples.slice(Math.max(0, lo - 1));
  }

  function getVal(s, key) {
    switch (key) {
      case 'theta': return deg(s.theta);
      case 'dtheta': return s.dtheta;
      case 'alpha': return deg(s.alphaU);
      case 'dalpha': return s.dalpha;
      case 'duty': return s.duty;
      default: return 0;
    }
  }

  function resample(samples, key, n, fs = FS) {
    if (!samples || samples.length < 4) return null;
    const t1 = samples[samples.length - 1].t;
    const t0 = t1 - (n - 1) * (1000 / fs);
    const out = new Float64Array(n);
    let j = 0;
    while (j < samples.length - 1 && samples[j].t < t0) j++;
    for (let k = 0; k < n; k++) {
      const t = t0 + k * 1000 / fs;
      while (j < samples.length - 1 && samples[j + 1].t < t) j++;
      const a = samples[j], b = samples[Math.min(j + 1, samples.length - 1)];
      const span = b.t - a.t;
      const f = span > 0.01 ? clamp((t - a.t) / span, 0, 1) : 0;
      out[k] = getVal(a, key) * (1 - f) + getVal(b, key) * f;
    }
    return out;
  }

  function resampleRange(samples, key, fromT, toT, n) {
    if (!samples || samples.length < 2 || toT <= fromT) return null;
    const out = new Float64Array(n);
    let j = 0;
    while (j < samples.length - 1 && samples[j].t < fromT) j++;
    for (let k = 0; k < n; k++) {
      const t = fromT + (toT - fromT) * k / (n - 1);
      while (j < samples.length - 1 && samples[j + 1].t < t) j++;
      const a = samples[j], b = samples[Math.min(j + 1, samples.length - 1)];
      const span = b.t - a.t;
      const f = span > 0.01 ? clamp((t - a.t) / span, 0, 1) : 0;
      out[k] = getVal(a, key) * (1 - f) + getVal(b, key) * f;
    }
    return out;
  }

  function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const ang = -TAU / len;
      const wr = Math.cos(ang), wi = Math.sin(ang);
      for (let i = 0; i < n; i += len) {
        let cr = 1, ci = 0;
        for (let k = 0; k < len / 2; k++) {
          const ur = re[i + k], ui = im[i + k];
          const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
          const vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
          re[i + k] = ur + vr; im[i + k] = ui + vi;
          re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
          const ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
        }
      }
    }
  }

  function spectrum(arr, fs = FS, win = true) {
    const n = arr.length;
    if (!n || (n & (n - 1)) !== 0) return null;
    const re = new Float64Array(n), im = new Float64Array(n);
    let mean = 0;
    for (let i = 0; i < n; i++) mean += arr[i];
    mean /= n;
    let sumWin = 0;
    for (let i = 0; i < n; i++) {
      const w = win ? 0.5 - 0.5 * Math.cos(TAU * i / (n - 1)) : 1;
      re[i] = (arr[i] - mean) * w;
      sumWin += w;
    }
    fft(re, im);
    const half = n / 2 + 1;
    const mag = new Float64Array(half);
    for (let k = 0; k < half; k++) mag[k] = 2 * Math.hypot(re[k], im[k]) / Math.max(1, sumWin);
    mag[0] /= 2;
    return mag;
  }

  function peakFreq(mag, fs = FS, fmin = 0.5, fmax = 25) {
    if (!mag) return null;
    const n = (mag.length - 1) * 2;
    const k0 = Math.max(1, Math.floor(fmin * n / fs));
    const k1 = Math.min(mag.length - 1, Math.ceil(fmax * n / fs));
    let best = k0, bv = -1;
    for (let k = k0; k <= k1; k++) if (mag[k] > bv) { bv = mag[k]; best = k; }
    return { freq: best * fs / n, mag: bv };
  }

  // 选择不超过“用户窗长”和“实际样本数”的最大 2 的幂。
  // 不再把短段用常值硬填到 1024 点——那会把 3 s 段的谱峰从 13.8 Hz 漂到 99 Hz。
  function fftLen(maxN, count) {
    if (!count || count < 32) return 0;
    let n = 32;
    while (n * 2 <= maxN && n * 2 <= count) n *= 2;
    return n;
  }

  // Hann 窗对数幅度抛物线插值，把频率从 bin 中心细化到约 0.01–0.05 Hz。
  function refinePeak(mag, fs = FS, fmin = 0.5, fmax = 25) {
    const p = peakFreq(mag, fs, fmin, fmax);
    if (!p) return null;
    const n = (mag.length - 1) * 2;
    const k = Math.round(p.freq * n / fs);
    if (k < 1 || k >= mag.length - 1) return p;
    const y0 = 20 * Math.log10(mag[k - 1] + 1e-12);
    const y1 = 20 * Math.log10(mag[k] + 1e-12);
    const y2 = 20 * Math.log10(mag[k + 1] + 1e-12);
    const den = y0 - 2 * y1 + y2;
    let d = den !== 0 ? 0.5 * (y0 - y2) / den : 0;
    d = clamp(d, -0.5, 0.5);
    return { freq: (k + d) * fs / n, mag: mag[k], binFreq: p.freq };
  }

  // 时域锁相幅值：对去均值序列在给定频率上做 sin/cos 投影。
  function toneAmplitude(arr, freq) {
    const n = arr.length;
    if (!n || !freq) return null;
    let mean = 0;
    for (let i = 0; i < n; i++) mean += arr[i];
    mean /= n;
    let c = 0, s = 0;
    for (let i = 0; i < n; i++) {
      const x = arr[i] - mean, w = TAU * freq * i / FS;
      c += x * Math.cos(w); s += x * Math.sin(w);
    }
    return 2 * Math.hypot(c, s) / n;
  }

  /* ======================================================================
   * 指标
   * ==================================================================== */
  function balanceSegments(samples) {
    const segs = [];
    let cur = null;
    for (const s of samples) {
      if ((s.status & 3) === 2) {
        if (!cur) cur = { start: s.t, end: s.t, samples: [] };
        cur.end = s.t; cur.samples.push(s);
      } else if (cur) { segs.push(cur); cur = null; }
    }
    if (cur) segs.push(cur);
    return segs;
  }

  function stats(samples) {
    if (!samples.length) return null;
    const n = samples.length;
    let st = 0, sa = 0, sd = 0, sd2 = 0, sat = 0, stMean = 0, saMean = 0;
    for (const x of samples) {
      const th = deg(x.theta), al = deg(x.alpha);
      stMean += th; saMean += al;
      st += th * th; sa += al * al;
      sd += x.duty; sd2 += x.duty * x.duty;
      if (Math.abs(x.duty) >= 1000) sat++;
    }
    const thetaMean = stMean / n, alphaMean = saMean / n;
    const dutyMean = sd / n;
    // 与项目 tools/analyze_noise.py 对齐：θ/α 都报“去均值标准差”，
    // 不报未去均值的 RMS（α 的均值可达数 rad，会严重偏置）。
    const thetaVar = Math.max(0, st / n - thetaMean * thetaMean);
    const alphaVar = Math.max(0, sa / n - alphaMean * alphaMean);
    return {
      thetaRms: Math.sqrt(thetaVar),
      alphaRms: Math.sqrt(alphaVar),
      thetaMean, alphaMean,
      dutyMean,
      dutyStd: Math.sqrt(Math.max(0, sd2 / n - dutyMean * dutyMean)),
      satPct: 100 * sat / n,
      n,
    };
  }

  function movingAvgHighpass(samples, key, winSamples) {
    const out = [];
    let acc = 0;
    const q = [];
    for (const s of samples) {
      const v = getVal(s, key);
      q.push(v); acc += v;
      if (q.length > winSamples) acc -= q.shift();
      out.push(v - acc / q.length);
    }
    return out;
  }

  function computeMetrics(samples) {
    if (!samples || samples.length < 5) return null;
    const segs = balanceSegments(samples);
    const first = segs.length ? segs[0] : null;
    const longest = segs.length ? segs.slice().sort((a, b) => (b.end - b.start) - (a.end - a.start))[0] : null;
    const now = samples[samples.length - 1].t;
    const firstHold = first ? (first.end - first.start) / 1000 : null;
    const bestHold = longest ? (longest.end - longest.start) / 1000 : null;
    let captureSec = null;
    const firstSwing = samples.find(x => (x.status & 3) === 1);
    if (firstSwing && first && first.start >= firstSwing.t) captureSec = (first.start - firstSwing.t) / 1000;

    // 稳定段 = 最长 BALANCE 段；无 BALANCE 时统计退回全段，但绝不据此报频域。
    // 短 BALANCE 也保持用该段本身，不能偷换成全段，否则界面会误标“稳定段 0.0 s”。
    let stable = longest ? longest.samples : samples;
    if (longest && stable.length > 5) {
      const t0 = longest.start + 2000;
      const cut = stable.filter(x => x.t >= t0);
      if (cut.length >= 5) stable = cut;
    }
    const base = stats(stable);
    if (!base) return null;

    // 频谱只对真实 BALANCE 段计算。fftLen 返回 2 的幂，最小可用档是 256 点（2.56 s）；
    // 128 点只有 1.28 s，不满足“至少 2 s”，因此 N=0，不报主频/音调幅。
    // 6–8 Hz 需要足够的周期数，否则不同段长会给出完全不同的“主频”。
    const maxN = S.fftMax || 1024;
    const N0 = longest ? fftLen(maxN, stable.length) : 0;
    const N = N0 >= 256 ? N0 : 0;
    const th = N ? resample(stable, 'theta', N) : null;
    const dt = N ? resample(stable, 'duty', N) : null;
    const thSpec = th ? spectrum(th) : null;
    const dtSpec = dt ? spectrum(dt) : null;
    const thPeak = thSpec ? refinePeak(thSpec, FS, 0.5, 25) : null;
    let tone = null;
    if (dtSpec && dt) {
      // 先由 FFT 找峰，再用该频率做时域锁相（lock-in）读幅值；
      // 这样幅值不会因为 FFT 频点落在两根谱线之间而少 10–15%。
      const p = refinePeak(dtSpec, FS, 2, 20);
      if (p) tone = toneAmplitude(dt, p.freq);
    }
    const hp = movingAvgHighpass(stable, 'theta', 20);
    const hf = Math.sqrt(hp.reduce((a, v) => a + v * v, 0) / Math.max(1, hp.length));
    // 少于 32 点（0.32 s）不报统计量：历史对比、导出、诊断都消费这些字段，
    // 统一在这里置空，避免各消费者各自漏判而把 0.00 当成“很稳”。
    const lowN = stable.length < 32;

    return {
      thetaRms: lowN ? null : base.thetaRms,
      alphaRms: lowN ? null : base.alphaRms,
      dutyStd: lowN ? null : base.dutyStd, dutyMean: base.dutyMean,
      satPct: lowN ? null : base.satPct, tone: lowN ? null : tone,
      freq: thPeak ? thPeak.freq : null,
      hfNoise: lowN ? null : hf,
      firstHold, bestHold, captureSec,
      stableStart: longest ? longest.start : null,
      stableEnd: longest ? longest.end : null,
      stableN: stable.length,
      specN: N,
      n: samples.length, live: now === (samples.length ? samples[samples.length - 1].t : 0),
    };
  }

  /* ======================================================================
   * 标记
   * ==================================================================== */
  function addMarker(text, t = nowMs(), manual = true) {
    const m = { t, text, manual };
    S.markers.push(m);
    if (S.markers.length > 80) S.markers.shift();
    renderMarkers();
    logEvent('marker', { text, t: Math.round(t), manual });
  }
  function renderMarkers() {
    const el = $('markerList');
    if (!S.markers.length) { el.textContent = '暂无标记'; return; }
    el.innerHTML = S.markers.slice(-12).reverse().map(m => {
      const wall = fmtWall(m.t);
      return `<div class="marker-item"><span>${wall}</span><span>${escapeHtml(m.text)}</span></div>`;
    }).join('');
  }
  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  /* ======================================================================
   * Canvas 基础
   * ==================================================================== */
  function canvasCtx(c) {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, c.clientWidth), h = Math.max(1, c.clientHeight);
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr); c.height = Math.round(h * dpr);
    }
    const ctx = c.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    return { ctx, w, h };
  }
  function niceSpan(min, max, minSpan) {
    let lo = min, hi = max;
    if (!isFinite(lo) || !isFinite(hi)) { lo = -1; hi = 1; }
    if (hi - lo < minSpan) { const c = (hi + lo) / 2; lo = c - minSpan / 2; hi = c + minSpan / 2; }
    const pad = (hi - lo) * 0.12;
    return [lo - pad, hi + pad];
  }

  /* ======================================================================
   * 时域图
   * ==================================================================== */
  function drawTimeChart(canvas, series, opts = {}) {
    const { ctx, w, h } = canvasCtx(canvas);
    const m = { l: 46, r: 46, t: 18, b: 18 };
    const pw = w - m.l - m.r, ph = h - m.t - m.b;
    if (pw <= 10 || ph <= 10) return;
    const data = windowSamples();
    const t1 = data.length ? data[data.length - 1].t : nowMs();
    const t0 = S.windowSec === 0 ? (data.length ? data[0].t : t1 - 1000) : t1 - S.windowSec * 1000;
    const span = Math.max(1, t1 - t0);

    // 轴范围
    const axes = { left: [], right: [] };
    for (const se of series) {
      if (se.off) continue;
      const vals = data.map(x => getVal(x, se.key));
      if (!vals.length) continue;
      axes[se.axis || 'left'].push(...vals);
    }
    let lr = niceSpan(Math.min(...(axes.left.length ? axes.left : [-1])), Math.max(...(axes.left.length ? axes.left : [1])), opts.minSpan || 1);
    let rr = niceSpan(Math.min(...(axes.right.length ? axes.right : [-1])), Math.max(...(axes.right.length ? axes.right : [1])), opts.minSpan || 1);
    if (opts.fixedLeft) lr = opts.fixedLeft;

    const xOf = t => m.l + (t - t0) / span * pw;
    const yOf = (v, axis) => {
      const [lo, hi] = axis === 'right' ? rr : lr;
      return m.t + (hi - v) / (hi - lo) * ph;
    };

    // 背景 / 网格
    ctx.fillStyle = '#060c15';
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = 'rgba(29,45,70,.75)';
    ctx.lineWidth = 1;
    ctx.font = '10px "Cascadia Mono", Consolas, monospace';
    ctx.textBaseline = 'middle';
    const gridN = 4;
    for (let i = 0; i <= gridN; i++) {
      const y = m.t + ph * i / gridN;
      ctx.beginPath(); ctx.moveTo(m.l, y); ctx.lineTo(m.l + pw, y); ctx.stroke();
      const lv = lr[1] - (lr[1] - lr[0]) * i / gridN;
      const rv = rr[1] - (rr[1] - rr[0]) * i / gridN;
      ctx.fillStyle = series.find(s => (s.axis || 'left') === 'left' && !s.off)?.color || '#7f93ad';
      ctx.textAlign = 'right'; ctx.fillText(fmt(lv, Math.abs(lv) < 10 ? 2 : 1), m.l - 5, y);
      if (series.some(s => (s.axis || 'left') === 'right' && !s.off)) {
        ctx.fillStyle = series.find(s => s.axis === 'right' && !s.off)?.color || '#7f93ad';
        ctx.textAlign = 'left'; ctx.fillText(fmt(rv, Math.abs(rv) < 10 ? 2 : 1), m.l + pw + 5, y);
      }
    }
    // 竖网格 / 时间
    const vN = 6;
    ctx.textAlign = 'center';
    for (let i = 0; i <= vN; i++) {
      const x = m.l + pw * i / vN;
      ctx.beginPath(); ctx.moveTo(x, m.t); ctx.lineTo(x, m.t + ph); ctx.stroke();
      const ts = t0 + span * i / vN;
      ctx.fillStyle = '#5b6d85';
      ctx.fillText(`${((ts - t1) / 1000).toFixed(1)}s`, x, h - 8);
    }

    // 参考线
    if (opts.refLines) {
      for (const r of opts.refLines) {
        const y = yOf(r.value, r.axis || 'left');
        if (y < m.t || y > m.t + ph) continue;
        ctx.save();
        ctx.setLineDash([4, 4]);
        ctx.strokeStyle = r.color || 'rgba(255,107,122,.5)';
        ctx.beginPath(); ctx.moveTo(m.l, y); ctx.lineTo(m.l + pw, y); ctx.stroke();
        ctx.setLineDash([]);
        ctx.fillStyle = r.color || '#ff6b7a';
        ctx.textAlign = 'left';
        ctx.fillText(r.label, m.l + 4, y - 7);
        ctx.restore();
      }
    }

    // 标记
    for (const mk of S.markers) {
      if (mk.t < t0 || mk.t > t1) continue;
      const x = xOf(mk.t);
      ctx.strokeStyle = mk.manual ? 'rgba(255,209,102,.55)' : 'rgba(179,136,255,.28)';
      ctx.setLineDash([2, 4]);
      ctx.beginPath(); ctx.moveTo(x, m.t); ctx.lineTo(x, m.t + ph); ctx.stroke();
      ctx.setLineDash([]);
    }

    // 数据
    ctx.save();
    ctx.beginPath(); ctx.rect(m.l, m.t, pw, ph); ctx.clip();
    for (const se of series) {
      if (se.off || !data.length) continue;
      const axis = se.axis || 'left';
      ctx.strokeStyle = se.color;
      ctx.lineWidth = se.width || 1.25;
      ctx.beginPath();
      let started = false;
      const step = Math.max(1, Math.ceil(data.length / Math.max(50, pw * 1.5)));
      for (let i = 0; i < data.length; i += step) {
        const x = data[i], xx = xOf(x.t), yy = yOf(getVal(x, se.key), axis);
        if (!started) { ctx.moveTo(xx, yy); started = true; } else ctx.lineTo(xx, yy);
      }
      if (started) ctx.stroke();
    }
    ctx.restore();

    // 图例
    let lx = m.l + 4;
    ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    for (const se of series) {
      if (se.off) continue;
      const lastV = data.length ? getVal(data[data.length - 1], se.key) : null;
      const text = `${se.label} ${lastV === null ? '--' : fmt(lastV, 2)}${se.unit || ''}`;
      ctx.fillStyle = se.color;
      ctx.fillText(text, lx, 9);
      lx += ctx.measureText(text).width + 14;
    }
    if (!data.length) {
      ctx.fillStyle = '#5b6d85'; ctx.textAlign = 'center';
      ctx.fillText('等待遥测数据（115200, 0xAA55）', w / 2, h / 2);
    }
  }

  function drawTimeCharts() {
    const th = $('showTheta').checked, al = $('showAlpha').checked, du = $('showDuty').checked;
    drawTimeChart($('chartTheta'), [
      { key: 'theta', label: 'θ', unit: '°', color: COLORS[0], width: 1.5 },
      { key: 'dtheta', label: 'θ̇', unit: 'rad/s', color: COLORS[1], axis: 'right' },
    ], { minSpan: 2, refLines: [
      { value: 22, label: '捕获门 22°', color: 'rgba(74,222,154,.45)' },
      { value: -22, label: '−22°', color: 'rgba(74,222,154,.45)' },
      { value: 45, label: '跌倒 45°', color: 'rgba(255,107,122,.45)' },
      { value: -45, label: '−45°', color: 'rgba(255,107,122,.45)' },
    ]});
    drawTimeChart($('chartAlpha'), [
      { key: 'alpha', label: 'α', unit: '°', color: COLORS[2], width: 1.4, off: !al },
      { key: 'dalpha', label: 'α̇', unit: 'rad/s', color: COLORS[3], axis: 'right', off: !al },
    ], { minSpan: 10 });
    drawTimeChart($('chartDuty'), [
      { key: 'duty', label: 'duty', unit: '', color: COLORS[4], width: 1.35, off: !du },
    ], { fixedLeft: [-1050, 1050], refLines: [
      { value: 1000, label: '+1000', color: 'rgba(255,107,122,.5)' },
      { value: -1000, label: '−1000', color: 'rgba(255,107,122,.5)' },
      { value: 432, label: '泵饱和 432', color: 'rgba(255,209,102,.35)' },
      { value: -432, label: '−432', color: 'rgba(255,209,102,.35)' },
    ]});
  }

  /* ======================================================================
   * 频域
   * ==================================================================== */
  function drawSpectrum(freqMax = 25) {
    const { ctx, w, h } = canvasCtx($('chartSpectrum'));
    const m = { l: 42, r: 14, t: 20, b: 20 };
    const pw = w - m.l - m.r, ph = h - m.t - m.b;
    ctx.fillStyle = '#060c15'; ctx.fillRect(0, 0, w, h);
    const data = windowSamples();
    const requested = Number($('fftSelect').value) || 1024;
    const N = fftLen(requested, data.length);
    if (!N) {
      ctx.fillStyle = '#5b6d85'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.fillText('等待足够数据（≥ 32 个样本）', w / 2, h / 2);
      return;
    }
    const specs = [];
    for (const se of [
      { key: 'theta', label: 'θ', color: COLORS[0] },
      { key: 'dalpha', label: 'α̇', color: COLORS[3] },
      { key: 'duty', label: 'duty', color: COLORS[4] },
    ]) {
      const arr = resample(data, se.key, N);
      const mag = arr ? spectrum(arr) : null;
      specs.push({ ...se, mag, peak: mag ? refinePeak(mag, FS, 0.5, freqMax) : null });
    }
    if (!specs.some(x => x.mag)) {
      ctx.fillStyle = '#5b6d85'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.fillText('等待足够数据', w / 2, h / 2);
      return;
    }
    const fmaxBin = Math.floor(freqMax * N / FS);
    const dbOf = (mag, k) => 20 * Math.log10(mag[k] + 1e-9);
    // 每条曲线按自身峰值归一化；纵轴 -70..+3 dB
    const norm = specs.map(sp => {
      if (!sp.mag) return null;
      let peak = -200;
      for (let k = 1; k <= fmaxBin; k++) peak = Math.max(peak, dbOf(sp.mag, k));
      return peak;
    });
    const yOf = db => m.t + (3 - db) / 73 * ph;
    const xOf = f => m.l + f / freqMax * pw;
    ctx.strokeStyle = 'rgba(29,45,70,.8)'; ctx.fillStyle = '#5b6d85';
    ctx.font = '10px "Cascadia Mono", Consolas, monospace'; ctx.textBaseline = 'middle';
    for (let db = 0; db >= -60; db -= 20) {
      const y = yOf(db);
      ctx.beginPath(); ctx.moveTo(m.l, y); ctx.lineTo(m.l + pw, y); ctx.stroke();
      ctx.textAlign = 'right'; ctx.fillText(`${db}dB`, m.l - 5, y);
    }
    for (let f = 0; f <= freqMax; f += 5) {
      const x = xOf(f);
      ctx.beginPath(); ctx.moveTo(x, m.t); ctx.lineTo(x, m.t + ph); ctx.stroke();
      ctx.textAlign = 'center'; ctx.fillStyle = '#5b6d85'; ctx.fillText(`${f}Hz`, x, h - 8);
    }
    ctx.save(); ctx.beginPath(); ctx.rect(m.l, m.t, pw, ph); ctx.clip();
    specs.forEach((sp, idx) => {
      if (!sp.mag || norm[idx] === null) return;
      ctx.strokeStyle = sp.color; ctx.lineWidth = 1.4;
      ctx.beginPath();
      for (let k = 1; k <= fmaxBin; k++) {
        const f = k * FS / N;
        const db = clamp(dbOf(sp.mag, k) - norm[idx], -72, 3);
        const x = xOf(f), y = yOf(db);
        if (k === 1) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    });
    ctx.restore();
    let lx = m.l + 4;
    ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    specs.forEach((sp, idx) => {
      const text = `${sp.label} 峰 ${sp.peak ? sp.peak.freq.toFixed(2) : '--'}Hz`;
      ctx.fillStyle = sp.color; ctx.fillText(text, lx, 9);
      lx += ctx.measureText(text).width + 14;
    });
    ctx.textAlign = 'right'; ctx.fillStyle = '#5b6d85';
    ctx.fillText(`N=${N} · ${(N / FS).toFixed(2)}s`, w - m.r, 9);
  }

  function drawSpectrogram() {
    const { ctx, w, h } = canvasCtx($('chartSpectrogram'));
    const m = { l: 42, r: 10, t: 16, b: 16 };
    const pw = w - m.l - m.r, ph = h - m.t - m.b;
    ctx.fillStyle = '#060c15'; ctx.fillRect(0, 0, w, h);
    if (S.specCols.length < 2) {
      ctx.fillStyle = '#5b6d85'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.fillText('频谱瀑布：等待数据…', w / 2, h / 2);
      return;
    }
    const cols = S.specCols.length, bins = S.specCols[0].mags.length;
    const off = document.createElement('canvas');
    off.width = cols; off.height = bins;
    const octx = off.getContext('2d');
    const img = octx.createImageData(cols, bins);
    for (let x = 0; x < cols; x++) {
      const col = S.specCols[x];
      for (let y = 0; y < bins; y++) {
        const v = clamp(col.mags[y] / (col.peak || 1), 0, 1);
        const db = 20 * Math.log10(v + 1e-6);
        const q = clamp((db + 60) / 60, 0, 1);
        const r = Math.round(255 * Math.pow(q, 2.4));
        const g = Math.round(255 * Math.pow(q, 0.85) * 0.88);
        const b = Math.round(255 * (0.16 + 0.84 * Math.pow(q, 0.35)));
        const i = ((bins - 1 - y) * cols + x) * 4;
        img.data[i] = r; img.data[i + 1] = g; img.data[i + 2] = b; img.data[i + 3] = 255;
      }
    }
    octx.putImageData(img, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(off, m.l, m.t, pw, ph);
    ctx.strokeStyle = 'rgba(29,45,70,.85)'; ctx.strokeRect(m.l, m.t, pw, ph);
    ctx.fillStyle = '#5b6d85'; ctx.font = '10px "Cascadia Mono", Consolas, monospace';
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (let f = 0; f <= 25; f += 5) {
      const y = m.t + ph - f / 25 * ph;
      ctx.fillText(`${f}`, m.l - 5, y);
    }
    ctx.textAlign = 'left'; ctx.fillText('Hz', 6, m.t + 5);
    ctx.textAlign = 'right'; ctx.fillText('θ 频谱瀑布 · 最近 20 s', m.l + pw, h - 5);
  }

  function pushSpectrogram() {
    const data = S.samples;
    if (data.length < 256) return;
    const lastT = data[data.length - 1].t;
    // 没有新样本时（断开/暂停）不要继续压入同一时间戳的列，
    // 否则 260 列上限会在约 57 s 内被重复列占满，真实历史被挤掉。
    if (lastT === S.lastSpecT) return;
    const N = 256;
    const arr = resample(data, 'theta', N);
    if (!arr) return;
    const mag = spectrum(arr);
    if (!mag) return;
    S.lastSpecT = lastT;
    const bins = Math.floor(25 * N / FS) + 1;
    const slice = Array.from(mag.slice(0, bins));
    const peak = Math.max(...slice.slice(1)) || 1;
    const t = data[data.length - 1].t;
    S.specCols.push({ t, mags: slice, peak });
    while (S.specCols.length && t - S.specCols[0].t > 20000) S.specCols.shift();
    if (S.specCols.length > 260) S.specCols.shift();
  }

  /* ======================================================================
   * 三维姿态
   * ==================================================================== */
  function update3D(ts) {
    const c = $('viz3d');
    if (!c) return;
    const { ctx, w, h } = canvasCtx(c);
    const bg = ctx.createRadialGradient(w * .5, h * .45, 10, w * .5, h * .5, Math.max(w, h) * .7);
    bg.addColorStop(0, '#0e1c30'); bg.addColorStop(1, '#060b14');
    ctx.fillStyle = bg; ctx.fillRect(0, 0, w, h);

    const s = S.last;
    const targetTheta = s ? s.theta : 0;
    const targetAlpha = s ? s.alpha : 0;
    if (S.smooth) {
      let dt = targetTheta - S.renderTheta;
      while (dt > Math.PI) dt -= TAU;
      while (dt < -Math.PI) dt += TAU;
      S.renderTheta += dt * 0.38;
      let da = targetAlpha - S.renderAlpha;
      while (da > Math.PI) da -= TAU;
      while (da < -Math.PI) da += TAU;
      S.renderAlpha += da * 0.38;
    } else { S.renderTheta = targetTheta; S.renderAlpha = targetAlpha; }
    const th = S.renderTheta, al = S.renderAlpha;

    const scale = Math.min(w, h) * 0.27;
    const yaw = -0.58, pitch = 0.40;
    const cyw = Math.cos(yaw), syw = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
    const proj = (x, y, z) => {
      const X = x * cyw - z * syw;
      const Z = x * syw + z * cyw;
      const Y = y * cp - Z * sp;
      return [w * 0.5 + X * scale, h * 0.57 - Y * scale, Z];
    };
    const armX = Math.cos(al) * ARM_LEN, armZ = Math.sin(al) * ARM_LEN;
    const tx = -Math.sin(al), tz = Math.cos(al);
    const rodX = armX + Math.sin(th) * ROD_LEN * tx;
    const rodY = Math.cos(th) * ROD_LEN;
    const rodZ = armZ + Math.sin(th) * ROD_LEN * tz;

    // 地面网格
    const G = 1.6;
    ctx.strokeStyle = 'rgba(46,74,108,.35)'; ctx.lineWidth = 1;
    for (let i = -4; i <= 4; i++) {
      const q = i / 4 * G;
      const a = proj(-G, GROUND_Y, q), b = proj(G, GROUND_Y, q);
      const c1 = proj(q, GROUND_Y, -G), d = proj(q, GROUND_Y, G);
      ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(c1[0], c1[1]); ctx.lineTo(d[0], d[1]); ctx.stroke();
    }
    // 地面圆
    ctx.strokeStyle = 'rgba(83,217,255,.14)'; ctx.beginPath();
    for (let i = 0; i <= 64; i++) {
      const a = i / 64 * TAU;
      const p = proj(Math.cos(a) * ARM_LEN, GROUND_Y, Math.sin(a) * ARM_LEN);
      if (i === 0) ctx.moveTo(p[0], p[1]); else ctx.lineTo(p[0], p[1]);
    }
    ctx.stroke();

    // 影子
    ctx.strokeStyle = 'rgba(0,0,0,.55)'; ctx.lineWidth = 4;
    const pa = proj(0, GROUND_Y, 0), pb = proj(armX, GROUND_Y, armZ);
    ctx.beginPath(); ctx.moveTo(pa[0], pa[1]); ctx.lineTo(pb[0] + 4, pb[1] + 5); ctx.stroke();
    const pr = proj(rodX, GROUND_Y, rodZ);
    ctx.fillStyle = 'rgba(0,0,0,.35)';
    ctx.beginPath(); ctx.ellipse(pr[0] + 5, pr[1] + 6, 11, 5, 0, 0, TAU); ctx.fill();

    // 竖直参考线
    const p0 = proj(armX, 0, armZ), p1 = proj(armX, ROD_LEN, armZ);
    ctx.save(); ctx.setLineDash([4, 5]); ctx.strokeStyle = 'rgba(126,160,196,.4)'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(p0[0], p0[1]); ctx.lineTo(p1[0], p1[1]); ctx.stroke(); ctx.restore();

    // 摆臂
    const armA = proj(0, 0, 0), armB = proj(armX, 0, armZ);
    ctx.save();
    ctx.strokeStyle = '#ff9e5e'; ctx.lineWidth = 7; ctx.lineCap = 'round';
    ctx.shadowColor = 'rgba(255,158,94,.65)'; ctx.shadowBlur = 14;
    ctx.beginPath(); ctx.moveTo(armA[0], armA[1]); ctx.lineTo(armB[0], armB[1]); ctx.stroke();
    ctx.restore();
    // 摆杆
    const rodA = proj(armX, 0, armZ), rodB = proj(rodX, rodY, rodZ);
    ctx.save();
    ctx.strokeStyle = '#53d9ff'; ctx.lineWidth = 4.5; ctx.lineCap = 'round';
    ctx.shadowColor = 'rgba(83,217,255,.65)'; ctx.shadowBlur = 16;
    ctx.beginPath(); ctx.moveTo(rodA[0], rodA[1]); ctx.lineTo(rodB[0], rodB[1]); ctx.stroke();
    ctx.restore();
    // 摆杆末端
    const glow = ctx.createRadialGradient(rodB[0], rodB[1], 1, rodB[0], rodB[1], 16);
    glow.addColorStop(0, 'rgba(210,250,255,1)'); glow.addColorStop(0.35, 'rgba(83,217,255,.75)'); glow.addColorStop(1, 'rgba(83,217,255,0)');
    ctx.fillStyle = glow; ctx.beginPath(); ctx.arc(rodB[0], rodB[1], 16, 0, TAU); ctx.fill();
    ctx.fillStyle = '#e9fcff'; ctx.beginPath(); ctx.arc(rodB[0], rodB[1], 3.4, 0, TAU); ctx.fill();
    // 转轴
    ctx.fillStyle = '#ffd166'; ctx.beginPath(); ctx.arc(armA[0], armA[1], 5, 0, TAU); ctx.fill();
    ctx.fillStyle = '#1a2a40'; ctx.beginPath(); ctx.arc(armA[0], armA[1], 2, 0, TAU); ctx.fill();

    // 末端轨迹
    if (s && ts - S.lastTrail > 40) {
      S.lastTrail = ts;
      S.trail.push({ x: rodX, y: rodY, z: rodZ });
      if (S.trail.length > 140) S.trail.shift();
    }
    if (S.trail.length > 2) {
      ctx.strokeStyle = 'rgba(179,136,255,.6)'; ctx.lineWidth = 1.4;
      ctx.beginPath();
      S.trail.forEach((p, i) => {
        const q = proj(p.x, p.y, p.z);
        if (i === 0) ctx.moveTo(q[0], q[1]); else ctx.lineTo(q[0], q[1]);
      });
      ctx.stroke();
    }

    // α 弧
    ctx.strokeStyle = 'rgba(255,209,102,.45)'; ctx.lineWidth = 1.5;
    ctx.beginPath();
    const steps = 40;
    for (let i = 0; i <= steps; i++) {
      const a = al * i / steps;
      const q = proj(Math.cos(a) * 0.42, GROUND_Y, Math.sin(a) * 0.42);
      if (i === 0) ctx.moveTo(q[0], q[1]); else ctx.lineTo(q[0], q[1]);
    }
    ctx.stroke();

    // 文字
    ctx.font = '11px "Cascadia Mono", Consolas, monospace';
    ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    ctx.fillStyle = 'rgba(83,217,255,.85)'; ctx.fillText('rod', rodB[0] + 10, rodB[1] - 8);
    ctx.fillStyle = 'rgba(255,158,94,.85)'; ctx.fillText('arm', (armA[0] + armB[0]) / 2, (armA[1] + armB[1]) / 2 - 10);
    ctx.fillStyle = 'rgba(255,209,102,.8)'; ctx.fillText(`α ${fmt(deg(al), 1)}°`, 12, 14);
    ctx.fillStyle = 'rgba(83,217,255,.8)'; ctx.fillText(`θ ${fmt(deg(th), 1)}°`, 12, 30);

    if (!s) {
      ctx.fillStyle = 'rgba(127,147,173,.8)'; ctx.textAlign = 'center';
      ctx.font = '12px "Cascadia Mono", Consolas, monospace';
      ctx.fillText('等待串口数据…', w / 2, h - 18);
    }
    const tip = $('tipPos');
    if (tip) tip.textContent = `摆杆末端 x/y/z：${fmt(rodX, 3)} / ${fmt(rodY, 3)} / ${fmt(rodZ, 3)} m（归一化模型）`;
  }

  /* ======================================================================
   * 读数 / 指标 / 看门狗
   * ==================================================================== */
  function updateReadouts() {
    const s = S.last;
    if (s) {
      $('rdTheta').textContent = fmt(deg(s.theta), 2);
      $('rdDtheta').textContent = fmt(s.dtheta, 2);
      $('rdAlpha').textContent = fmt(deg(s.alpha), 1);
      $('rdDalpha').textContent = fmt(s.dalpha, 2);
      $('rdDuty').textContent = String(s.duty);
      const sp = $('rdState');
      sp.textContent = STATE_NAMES[s.fsm] + (s.motor ? ' · M' : ' · 停');
      sp.className = 'state-pill ' + STATE_CLASS[s.fsm];
      $('rateText').textContent = `${S.fps.toFixed(1)} 帧/s`;
    }
    const interval = S.frames ? ((S.samples.length && S.samples.length > 1) ? (S.samples[S.samples.length - 1].t - S.samples[0].t) / Math.max(1, S.samples.length - 1) : 10) : 0;
    const m = computeMetrics(S.samples);
    S.lastMetrics = m;
    if (m) {
      // 稳定段样本太少（例如只剩 3 点 BALANCE）时，σ 会退化成 0 并被误判为“很稳”。
      // 此时只报时长/捕获，不报稳定度与噪音。
      const lowN = m.stableN < 32;
      $('mThetaRms').textContent = lowN ? '--' : fmt(m.thetaRms, 2);
      $('mAlphaRms').textContent = lowN ? '--' : fmt(m.alphaRms, 2);
      $('mDutyStd').textContent = lowN ? '--' : fmt(m.dutyStd, 0);
      $('mTone').textContent = fmt(m.tone, 0);
      $('mFreq').textContent = fmt(m.freq, 2);
      $('mSat').textContent = lowN ? '--' : fmt(m.satPct, 1);
      $('mFirstHold').textContent = fmt(m.firstHold, 2);
      $('mCapture').textContent = fmt(m.captureSec, 2);
      $('mBestHold').textContent = fmt(m.bestHold, 2);
      $('mHfNoise').textContent = lowN ? '--' : fmt(m.hfNoise, 2);
      $('mFrames').textContent = `${S.frames} / ${S.bad}`;
      const scope = m.stableStart != null ? `稳定段 ${fmt((m.stableEnd - m.stableStart) / 1000, 1)} s` : '全段（无 BALANCE）';
      const fftNote = m.specN ? ` · FFT N=${m.specN}` : (m.stableStart != null ? ` · 频谱窗 ${fmt(m.stableN / FS, 2)} s < 2.56 s` : ' · 无 BALANCE，不报频域');
      $('metricsScope').textContent = scope + fftNote + (lowN ? ` · 样本少（${m.stableN} 点），不报稳定度` : '');
      $('mThetaRms').parentElement.classList.toggle('good', !lowN && m.thetaRms < 2.5);
      $('mThetaRms').parentElement.classList.toggle('bad', !lowN && m.thetaRms > 4);
      $('mSat').parentElement.classList.toggle('bad', !lowN && m.satPct > 2);
    }
    const f = $('mFrames');
    if (f) f.textContent = `${S.frames} / ${S.bad}`;
    // 文本
    const txt = `桥接：${BRIDGE} · 采样间隔中位 ${fmt(interval, 1)} ms · 识别帧 ${S.frames} · 坏帧 ${S.bad}`;
    const bt = $('bridgeText'); if (bt) bt.textContent = txt;
    watchdog();
  }

  function watchdog() {
    const box = $('warnBox');
    let msg = '看门狗：正常。';
    let bad = false;
    const s = S.last;
    if (S.connected && S.lastRx && nowMs() - S.lastRx > 1500) {
      msg = '看门狗：串口已连接但 1.5 s 没有合法帧，检查波特率/CH340/烧录。'; bad = true;
    } else if (s && Math.abs(s.duty) >= 1000) {
      msg = '看门狗：duty 接近 ±1000 饱和，检查增益/机械限位。'; bad = true;
    } else if (s && s.fsm === 2 && Math.abs(deg(s.theta)) > 30) {
      msg = '看门狗：状态 BALANCE 但 |θ| > 30°，可能即将跌落。'; bad = true;
    } else if (s && !s.motor && s.fsm !== 0) {
      msg = '看门狗：电机使能关闭（SW3），状态机不会输出有效控制。'; bad = true;
    }
    if (msg !== S.lastWarn) {
      S.lastWarn = msg;
      box.textContent = msg;
      box.classList.toggle('warn', bad);
    }
  }

  /* ======================================================================
   * 历史 / 对比
   * ==================================================================== */
  function parseCsv(text, name) {
    const lines = text.replace(/\r/g, '').split('\n').filter(x => x.trim());
    if (lines.length < 3) throw new Error('CSV 行数不足');
    const head = lines[0].split(',').map(x => x.trim());
    const idx = {};
    head.forEach((h, i) => idx[h] = i);
    const need = ['theta_rad', 'dtheta_rad_s', 'alpha_rad', 'dalpha_rad_s', 'duty', 'status'];
    for (const k of need) if (!(k in idx)) throw new Error('CSV 缺少列 ' + k);
    const timeName = ('time' in idx) ? 'time' : null;
    const samples = [];
    let dayOffset = 0, prevSec = null;
    for (let i = 1; i < lines.length; i++) {
      const p = lines[i].split(',');
      if (p.length < head.length) continue;
      const rawSec = timeName ? parseClock(p[idx.time]) : (samples.length ? samples[samples.length - 1].t / 1000 + 0.01 : 0);
      if (prevSec !== null && rawSec < prevSec - 43200) dayOffset += 86400;
      prevSec = rawSec;
      const tSec = rawSec + dayOffset;
      const alpha = Number(p[idx.alpha_rad]);
      const status = Number(p[idx.status]) | 0;
      samples.push({
        t: tSec * 1000,
        theta: Number(p[idx.theta_rad]),
        dtheta: Number(p[idx.dtheta_rad_s]),
        alpha, alphaU: alpha,
        dalpha: Number(p[idx.dalpha_rad_s]),
        duty: Number(p[idx.duty]),
        status, fsm: status & 3,
      });
    }
    return { name, samples };
  }
  function parseClock(s) {
    if (s === undefined) return 0;
    const m = String(s).match(/(\d+):(\d+):(\d+(?:\.\d+)?)/);
    if (!m) return Number(s) || 0;
    return (+m[1]) * 3600 + (+m[2]) * 60 + (+m[3]);
  }

  function addCompare(name, samples, colorIdx = null) {
    const m = computeMetrics(samples);
    if (!m) throw new Error('无法计算指标（样本不足）');
    const seg = balanceSegments(samples);
    const longest = seg.length ? seg.slice().sort((a, b) => (b.end - b.start) - (a.end - a.start))[0] : null;
    let trace = [];
    if (longest) {
      const from = longest.start, to = Math.min(longest.start + 12000, longest.end);
      const arr = resampleRange(samples, 'theta', from, to, 240);
      if (arr) trace = Array.from(arr).map((v, i) => ({ t: i * (to - from) / 239, v }));
    }
    const item = {
      name, m, trace,
      color: COLORS[(colorIdx === null ? S.compare.length : colorIdx) % COLORS.length],
      added: new Date().toISOString(),
    };
    S.compare.push(item);
    if (S.compare.length > 8) S.compare.shift();
    renderCompare();
    return item;
  }

  function renderCompare() {
    const tb = document.querySelector('#compareTable tbody');
    tb.innerHTML = S.compare.map(r => `<tr>
      <td title="${escapeHtml(r.name)}">${escapeHtml(r.name)}</td>
      <td>${fmt(r.m.thetaRms, 2)}</td><td>${fmt(r.m.dutyStd, 0)}</td>
      <td>${fmt(r.m.freq, 2)}</td><td>${r.m.satPct == null ? '--' : fmt(r.m.satPct, 1) + '%'}</td><td>${fmt(r.m.bestHold, 1)}</td>
    </tr>`).join('');
    $('compareEmpty').style.display = S.compare.length ? 'none' : 'block';
    drawCompare();
  }

  function drawCompare() {
    const c = $('chartCompare');
    const { ctx, w, h } = canvasCtx(c);
    const m = { l: 40, r: 10, t: 18, b: 18 };
    const pw = w - m.l - m.r, ph = h - m.t - m.b;
    ctx.fillStyle = '#060c15'; ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = 'rgba(29,45,70,.8)'; ctx.fillStyle = '#5b6d85';
    ctx.font = '10px "Cascadia Mono", Consolas, monospace'; ctx.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++) {
      const y = m.t + ph * i / 4;
      ctx.beginPath(); ctx.moveTo(m.l, y); ctx.lineTo(m.l + pw, y); ctx.stroke();
    }
    const all = S.compare.flatMap(r => r.trace.map(p => p.v));
    if (!all.length) {
      ctx.fillStyle = '#5b6d85'; ctx.textAlign = 'center';
      ctx.fillText('载入历史 CSV 或保存本次后显示', w / 2, h / 2);
      return;
    }
    const lo = Math.min(...all), hi = Math.max(...all);
    const pad = Math.max(0.4, (hi - lo) * .12);
    const y0 = lo - pad, y1 = hi + pad;
    const xOf = t => m.l + t / 12000 * pw;
    const yOf = v => m.t + (y1 - v) / (y1 - y0) * ph;
    ctx.textAlign = 'right';
    for (let i = 0; i <= 4; i++) {
      const y = m.t + ph * i / 4;
      ctx.fillText(fmt(y1 - (y1 - y0) * i / 4, 1), m.l - 5, y);
    }
    let lx = m.l + 3;
    for (const r of S.compare) {
      ctx.strokeStyle = r.color; ctx.lineWidth = 1.3;
      ctx.beginPath();
      r.trace.forEach((p, i) => {
        const x = xOf(p.t), y = yOf(p.v);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.stroke();
      ctx.fillStyle = r.color; ctx.textAlign = 'left';
      const label = r.name.length > 16 ? r.name.slice(0, 15) + '…' : r.name;
      ctx.fillText(label, lx, 9);
      lx += ctx.measureText(label).width + 12;
    }
    ctx.fillStyle = '#5b6d85'; ctx.textAlign = 'right';
    ctx.fillText('稳定段前 12 s · θ(°)', m.l + pw, h - 5);
  }

  async function loadHistoryList() {
    try {
      const j = await fetchJSON(BRIDGE + '/runs', {}, 5000);
      const sel = $('historySelect');
      const cur = sel.value;
      sel.innerHTML = '<option value="">选择 data/ 里的历史 CSV…</option>' +
        (j.runs || []).map(r => `<option value="${escapeHtml(r.name)}">${escapeHtml(r.name)} · ${(r.size / 1024).toFixed(0)} KB</option>`).join('');
      if (cur) sel.value = cur;
      setBadge($('healthBadge'), '8788 在线', true);
    } catch {
      setBadge($('healthBadge'), '8788 未连', false);
    }
  }

  async function loadHistorical() {
    const name = $('historySelect').value;
    if (!name) return;
    try {
      const r = await fetch(BRIDGE + '/run?file=' + encodeURIComponent(name));
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const text = await r.text();
      const { samples } = parseCsv(text, name);
      addCompare(name, samples);
    } catch (e) {
      alert('载入失败：' + e.message);
    }
  }

  async function saveCurrentRun() {
    if (!S.samples.length) { alert('还没有数据。'); return; }
    const name = `live_${stamp()}_${S.frames}f`;
    try {
      const item = addCompare(name, S.samples.slice());
      S.history.push({ name, added: item.added, m: item.m, trace: item.trace });
      saveHistory();
      renderHistorySaved();
    } catch (e) { alert('保存失败：' + e.message); return; }
    try {
      const csv = csvText();
      const j = await fetchJSON(BRIDGE + '/runs/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, csv }),
      }, 8000);
      if (j && j.file) $('buildState').textContent = `已保存项目 CSV：data/${j.file}`;
    } catch (e) {
      $('buildState').textContent = '已保存浏览器对比；桥接离线，未写入项目 data/';
    }
  }

  function renderHistorySaved() {
    // 已保存的项也进入对比下拉之外的存储；这里只保持 compare 表
    if (!S.compare.length) return;
  }

  /* ======================================================================
   * 参数面板
   * ==================================================================== */
  async function loadSchema(opts = {}) {
    try {
      const j = await fetchJSON(BRIDGE + '/tune/schema', {}, 6000);
      S.schema = j;
      // 默认保留用户尚未提交的草稿；只有缺失/越界的项才回落到源码值。
      // 否则每次“检查桥接”都会把正在编辑的参数表悄悄清掉。
      // 恢复备份后必须传 overwriteDraft: true，用恢复后的源码值覆盖旧草稿，
      // 否则输入框仍显示构建失败前改过的值，下一次构建会把它又写回去。
      const prev = opts.overwriteDraft ? {} : (S.draft || {});
      const draft = {};
      for (const g of j.groups) for (const p of g.params) {
        const cand = Number(prev[p.id]);
        draft[p.id] = (Number.isFinite(cand) && cand >= p.min && cand <= p.max) ? cand : p.value;
      }
      S.draft = draft;
      S.activeGroup = S.activeGroup && j.groups.some(g => g.id === S.activeGroup) ? S.activeGroup : j.groups[0]?.id;
      setBadge($('bridgeBadge'), '8788 在线', true);
      $('paramStatus').textContent = `源码：${j.project} · 已读取 ${j.groups.reduce((a, g) => a + g.params.length, 0)} 个可写参数`;
      renderParams();
      updateVersion(j.version);
      $('buildBtn').disabled = false;
      $('flashBtn').disabled = false;
      return true;
    } catch (e) {
      setBadge($('bridgeBadge'), '桥接未连接', false);
      $('paramStatus').textContent = '桥接 8788 未连接：参数表和构建/烧录不可用。先运行 python live_bridge.py。串口绘图不受影响。';
      // 旧 schema 可能对应已被 restore 覆盖的源码；禁用构建的同时清掉，
      // 防止“载入当前源码值”把旧 schema 的值重新写回草稿。
      S.schema = null;
      $('buildBtn').disabled = true;
      $('flashBtn').disabled = true;
      return false;
    }
  }

  function updateVersion(v) {
    if (!v) return;
    const el = $('bitstreamText');
    if (v.checksum) el.textContent = `impl 位流：${v.checksum} · ${v.mtime || ''}`;
    else el.textContent = `impl 位流：${v.file || '未构建'}`;
  }

  function renderParams() {
    if (!S.schema) return;
    const tabs = $('paramTabs');
    tabs.innerHTML = S.schema.groups.map(g =>
      `<button data-group="${g.id}" class="${g.id === S.activeGroup ? 'active' : ''}">${escapeHtml(g.label)}</button>`
    ).join('');
    tabs.querySelectorAll('button').forEach(b => b.addEventListener('click', () => {
      S.activeGroup = b.dataset.group; renderParams();
    }));
    const group = S.schema.groups.find(g => g.id === S.activeGroup) || S.schema.groups[0];
    const list = $('paramList');
    list.innerHTML = group.params.map(p => {
      const v = S.draft[p.id] ?? p.value;
      const dirty = Math.abs(Number(v) - Number(p.value)) > 1e-9;
      return `<div class="param-row ${dirty ? 'dirty' : ''}" data-id="${p.id}">
        <div class="plabel">${escapeHtml(p.label)} <span class="badge">${escapeHtml(p.unit || '')}</span>
          <small>${escapeHtml(p.description)}</small>
        </div>
        <input type="number" step="${p.step}" min="${p.min}" max="${p.max}" value="${v}" data-id="${p.id}">
        <div class="praw">源码：${p.rawText} · ${escapeHtml(p.file)} · 范围 ${p.min}…${p.max}</div>
      </div>`;
    }).join('');
    list.querySelectorAll('input[type=number]').forEach(inp => {
      inp.addEventListener('input', () => {
        const id = inp.dataset.id;
        S.draft[id] = Number(inp.value);
        const p = group.params.find(x => x.id === id);
        if (!p) return;
        inp.closest('.param-row').classList.toggle('dirty', Math.abs(Number(inp.value) - Number(p.value)) > 1e-9);
      });
    });
  }

  function resetDraft() {
    if (!S.schema) return;
    for (const g of S.schema.groups) for (const p of g.params) S.draft[p.id] = p.value;
    renderParams();
    $('buildState').textContent = '已恢复为源码当前值';
  }

  function saveDraft() {
    try { localStorage.setItem('j280_param_draft_v1', JSON.stringify(S.draft)); $('buildState').textContent = '草稿已保存到浏览器'; } catch {}
  }
  function restoreDraft() {
    try {
      const d = JSON.parse(localStorage.getItem('j280_param_draft_v1') || 'null');
      if (d) { Object.assign(S.draft, d); renderParams(); }
    } catch {}
  }

  async function restoreLastBackup() {
    if (S.busy) { alert('构建/烧录运行中，不能恢复源码。'); return; }
    try {
      const j = await fetchJSON(BRIDGE + '/tune/backups', {}, 5000);
      const items = (j && j.backups) || [];
      if (!items.length) { alert('没有可用备份。'); return; }
      const name = items[0].name;
      if (!confirm(`用构建前备份 ${name} 覆盖 src/ 中的源文件？\n这会把源码恢复到那次构建前的状态，之后需要重新构建。\n浏览器里未保存的草稿也会被清除。`)) return;
      const r = await fetchJSON(BRIDGE + '/tune/restore', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ backup: name }),
      }, 10000);
      $('buildState').textContent = `已恢复备份 ${name}：${(r.restored || []).join(', ')}`;
      // 旧草稿仍在 localStorage 里，刷新页面或点“载入草稿”会把恢复前的值又盖回来。
      // 同时清空内存草稿：即使随后 schema 刷新失败，之后任何一次成功加载也不会复活旧值。
      try { localStorage.removeItem('j280_param_draft_v1'); } catch {}
      S.draft = {};
      const ok = await loadSchema({ overwriteDraft: true });
      if (!ok) $('buildState').textContent += '（参数表刷新失败：构建/烧录已禁用，请检查桥接后重试）';
    } catch (e) {
      $('buildState').textContent = '恢复备份失败：' + e.message;
    }
  }

  async function startBuild(flash) {
    if (S.busy) return;
    if (!S.schema) { alert('桥接未连接，无法构建。'); return; }
    if (flash) {
      if (!$('safeCheck').checked) { alert('请先确认摆杆已扶稳、SW3 电机使能为关。'); return; }
      const burnRaw = $('burnConfirm').value;
      if (burnWord(burnRaw) !== 'BURN') {
        alert(`烧录确认词不对：收到“${burnRaw}”（${burnRaw.length} 字符）。请输入英文大写 BURN。`);
        return;
      }
    }
    const params = {};
    for (const g of S.schema.groups) for (const p of g.params) {
      const v = Number(S.draft[p.id]);
      if (!isFinite(v)) { alert(`${p.label} 不是数字`); return; }
      if (v < p.min || v > p.max) { alert(`${p.label} 超出范围 ${p.min}…${p.max}`); return; }
      params[p.id] = v;
    }
    S.busy = true;
    $('buildBtn').disabled = true; $('flashBtn').disabled = true;
    $('buildLog').textContent = '提交构建任务…\n';
    $('buildState').textContent = flash ? '排队：构建 + 烧录' : '排队：仅构建';
    try {
      const j = await fetchJSON(BRIDGE + '/tune/build', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          params, flash,
          safe_ack: flash ? $('safeCheck').checked : false,
          confirm: burnWord($('burnConfirm').value),
        }),
      }, 10000);
      S.job = j.job_id || j.job?.id;
      $('buildState').textContent = '运行中…';
      pollBuild();
    } catch (e) {
      $('buildState').textContent = '提交失败：' + e.message;
      S.busy = false;
      $('buildBtn').disabled = false; $('flashBtn').disabled = false;
    }
  }

  async function pollBuild() {
    if (!S.job) return;
    try {
      const j = await fetchJSON(BRIDGE + '/tune/status?id=' + encodeURIComponent(S.job), {}, 5000);
      const job = j.job || j;
      $('buildState').textContent = `${job.state} · ${job.phase || ''}`;
      if (job.log) {
        $('buildLog').textContent = job.log;
        $('buildLog').scrollTop = $('buildLog').scrollHeight;
      }
      if (job.state === 'done' || job.state === 'failed' || job.state === 'ok' || job.state === 'error') {
        S.busy = false;
        $('buildBtn').disabled = false; $('flashBtn').disabled = false;
        if (job.result) {
          const r = job.result;
          $('buildLog').textContent += `\n\n[结果] ${JSON.stringify(r, null, 2)}`;
          if (r.checksum) updateVersion({ checksum: r.checksum, mtime: r.built_at });
          if (r.flashed) alert(`烧录完成：${r.checksum}\n请扶稳摆杆，短按 SW1 重标定，再按 SW3 使能。`);
        }
        await loadSchema();
        return;
      }
      clearTimeout(S.poll);
      S.poll = setTimeout(pollBuild, 900);
    } catch (e) {
      $('buildState').textContent = '轮询失败：' + e.message;
      clearTimeout(S.poll);
      S.poll = setTimeout(pollBuild, 2000);
    }
  }

  /* ======================================================================
   * 桥接 / 诊断 / 导出
   * ==================================================================== */
  async function checkHealth() {
    try {
      const j = await fetchJSON(BRIDGE + '/health', {}, 2500);
      setBadge($('healthBadge'), '8788 在线', true);
      setBadge($('bridgeBadge'), '8788 在线', true);
      $('bridgeText').textContent = `桥接在线 · live: ${j.live_dir}`;
      await loadSchema();
      await loadHistoryList();
    } catch {
      setBadge($('healthBadge'), '8788 未连', false);
      setBadge($('bridgeBadge'), '桥接未连接', false);
      $('bridgeText').textContent = `桥接离线：${BRIDGE}（先运行 python live_bridge.py）`;
    }
  }

  function csvText() {
    const lines = ['time,theta_rad,dtheta_rad_s,alpha_rad,dalpha_rad_s,duty,status'];
    for (const s of S.samples) {
      lines.push([fmtWall(s.t), s.theta.toFixed(6), s.dtheta.toFixed(6), s.alpha.toFixed(6),
        s.dalpha.toFixed(6), s.duty, s.status].join(','));
    }
    return lines.join('\n');
  }

  function exportCsv() {
    if (!S.samples.length) { alert('还没有数据。'); return; }
    download(`j280_${stamp()}.csv`, csvText(), 'text/csv');
  }

  function exportCompare() {
    if (!S.compare.length) { alert('没有对比项。'); return; }
    const lines = ['name,theta_std_deg,alpha_std_deg,duty_std,tone,freq_hz,sat_pct,first_hold_s,best_hold_s,hf_noise_deg'];
    for (const r of S.compare) {
      const low = r.m.stableN < 32;
      lines.push([JSON.stringify(r.name), low ? '' : fmt(r.m.thetaRms, 4), low ? '' : fmt(r.m.alphaRms, 4), low ? '' : fmt(r.m.dutyStd, 2),
        low ? '' : fmt(r.m.tone, 2), low ? '' : fmt(r.m.freq, 4), low ? '' : fmt(r.m.satPct, 3), fmt(r.m.firstHold, 3),
        fmt(r.m.bestHold, 3), low ? '' : fmt(r.m.hfNoise, 4)].join(','));
    }
    download(`j280_compare_${stamp()}.csv`, lines.join('\n'), 'text/csv');
  }

  function download(name, text, type) {
    const blob = new Blob([text], { type: type + ';charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob); a.download = name; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }
  function stamp() {
    const d = new Date(), p = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
  }

  async function copyDiagnostics() {
    const diag = {
      generated: new Date().toISOString(),
      firmware: $('bitstreamText').textContent,
      serial: { connected: S.connected, frames: S.frames, bad: S.bad, fps: S.fps, samples: S.samples.length },
      params: S.draft,
      metrics: S.lastMetrics,
      lastSample: S.last,
      markers: S.markers.slice(-10),
    };
    const text = JSON.stringify(diag, null, 2);
    try { await navigator.clipboard.writeText(text); $('buildState').textContent = '诊断 JSON 已复制'; }
    catch { download(`j280_diag_${stamp()}.json`, text, 'application/json'); }
  }

  function renderRaw() {
    const el = $('rawBytes');
    if (!S.rawVisible) { el.classList.add('hidden'); return; }
    el.classList.remove('hidden');
    el.textContent = S.rawTail.slice(-96).map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
  }

  /* ======================================================================
   * 事件
   * ==================================================================== */
  function bind() {
    $('connectBtn').addEventListener('click', connect);
    $('disconnectBtn').addEventListener('click', disconnect);
    $('pauseBtn').addEventListener('click', () => {
      S.paused = !S.paused;
      $('pauseBtn').textContent = S.paused ? '继续绘图' : '暂停绘图';
    });
    $('exportCsvBtn').addEventListener('click', exportCsv);
    $('saveRunBtn').addEventListener('click', saveCurrentRun);
    $('clearBtn').addEventListener('click', () => {
      S.samples = []; S.markers = []; S.trail = []; S.specCols = []; S.last = null; S.lastSpecT = 0; S.lastMetrics = null;
      S.frames = 0; S.bad = 0; S.bytes = 0; S.rx = new Uint8Array(0); S.lastRx = 0;
      S.renderTheta = 0; S.renderAlpha = 0;
      renderMarkers(); updateReadouts(); drawAll();
    });
    $('markBtn').addEventListener('click', () => addMarker('手动标记'));
    $('windowSelect').addEventListener('change', e => { S.windowSec = Number(e.target.value); drawAll(); });
    $('fftSelect').addEventListener('change', e => { S.fftMax = Number(e.target.value) || 1024; drawSpectrum(); });
    ['showTheta', 'showAlpha', 'showDuty'].forEach(id => $(id).addEventListener('change', drawAll));
    $('resetDraftBtn').addEventListener('click', resetDraft);
    $('saveDraftBtn').addEventListener('click', saveDraft);
    $('loadDraftBtn').addEventListener('click', restoreDraft);
    $('restoreBackupBtn').addEventListener('click', restoreLastBackup);
    $('buildBtn').addEventListener('click', () => startBuild(false));
    $('flashBtn').addEventListener('click', () => startBuild(true));
    $('healthBtn').addEventListener('click', checkHealth);
    $('diagBtn').addEventListener('click', copyDiagnostics);
    $('rawToggleBtn').addEventListener('click', () => {
      S.rawVisible = !S.rawVisible;
      $('rawToggleBtn').textContent = S.rawVisible ? '隐藏字节' : '原始字节';
      renderRaw();
    });
    $('smooth3d').addEventListener('change', e => { S.smooth = e.target.checked; });
    $('loadHistoryBtn').addEventListener('click', loadHistorical);
    $('clearCompareBtn').addEventListener('click', () => { S.compare = []; renderCompare(); });
    $('exportCompareBtn').addEventListener('click', exportCompare);
    document.addEventListener('keydown', e => {
      if (e.target.matches('input, select, textarea')) return;
      if (e.key === 'c' || e.key === 'C') connect();
      if (e.key === 'd' || e.key === 'D') disconnect();
      if (e.key === ' ') { e.preventDefault(); $('pauseBtn').click(); }
      if (e.key === 'm' || e.key === 'M') addMarker('手动标记');
    });
    navigator.serial?.addEventListener('disconnect', () => {
      if (S.connected) { S.connected = false; setSerialStatus('offline', '串口已拔出'); logEvent('serial_status', { connected: false }); }
    });
  }

  /* ======================================================================
   * 主循环
   * ==================================================================== */
  function drawAll() {
    drawTimeCharts();
    drawSpectrum();
    drawSpectrogram();
    drawCompare();
  }

  function loop(ts) {
    if (!S.paused) {
      update3D(ts);
      if (ts - S.lastChartT > 100) { S.lastChartT = ts; drawTimeCharts(); }
      if (ts - S.lastReadoutT > 300) { S.lastReadoutT = ts; updateReadouts(); }
      if (ts - S.lastSpec > 500) { S.lastSpec = ts; drawSpectrum(); }
      if (ts - S.lastSpecDraw > 220) { S.lastSpecDraw = ts; pushSpectrogram(); drawSpectrogram(); }
    }
    requestAnimationFrame(loop);
  }

  function init() {
    S.smooth = $('smooth3d').checked;
    S.fftMax = Number($('fftSelect').value) || 1024;
    bind();
    renderMarkers();
    renderCompare();
    restoreDraft();
    setInterval(() => { $('clockText').textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false }); }, 1000);
    $('clockText').textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false });
    checkHealth();
    update3D(0);
    drawAll();
    requestAnimationFrame(loop);
    logEvent('monitor_ready', { version: '3.0' });
  }

  function logEvent(type, data) {
    try {
      fetch(BRIDGE + '/event', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type, data, source: 'web', at: new Date().toISOString() }),
      }).catch(() => {});
    } catch {}
  }

  window.J280 = { S, computeMetrics, parseCsv, drawAll };
  document.addEventListener('DOMContentLoaded', init);
})();