#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
POC Latency Spectrum Analyzer - Real-time GUI Monitor

Spectrum analyzer-style GUI that visualizes scheduling wakeup latency
in real-time. Workers perform rapid nanosleep cycles to stress the
select_idle_sibling() path and measure wakeup latency.

Requirements: Python 3.8+, PyQt5
Usage:
    python3 poc_monitor.py
    sudo python3 poc_monitor.py   # enables POC toggle
"""

import sys
import os
import time
import ctypes
import threading
import tempfile
import subprocess
from collections import deque

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QSpinBox, QCheckBox,
)
from PyQt5.QtCore import Qt, QTimer, QRectF, QPointF
from PyQt5.QtGui import (
    QPainter, QColor, QLinearGradient, QPen, QFont, QBrush, QPainterPath,
)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

HIST_BOUNDS_NS = [500, 1000, 2000, 4000, 8000, 16000, 32000, float("inf")]
HIST_LABELS = [
    "0\u20130.5\u00b5s", "0.5\u20131\u00b5s", "1\u20132\u00b5s", "2\u20134\u00b5s",
    "4\u20138\u00b5s", "8\u201316\u00b5s", "16\u201332\u00b5s", ">32\u00b5s",
]
NUM_BUCKETS = 8

SYSCTL_POC_PATH = "/proc/sys/kernel/sched_poc_selector"

BAR_COLORS = [
    QColor(0, 230, 118),
    QColor(76, 230, 76),
    QColor(156, 240, 0),
    QColor(240, 230, 0),
    QColor(255, 193, 7),
    QColor(255, 138, 0),
    QColor(255, 61, 0),
    QColor(244, 0, 0),
]

BG_COLOR = QColor(18, 18, 36)
BG_DARKER = QColor(12, 12, 28)
GRID_COLOR = QColor(40, 40, 70)
TEXT_COLOR = QColor(200, 200, 220)
TEXT_DIM = QColor(100, 100, 130)

DEFAULT_SLEEP_US = 50
UPDATE_MS = 33  # ~30 fps
WINDOW_MS = 500
TIMELINE_MAX = 300

# ---------------------------------------------------------------------------
# libc helpers
# ---------------------------------------------------------------------------

PR_SET_TIMERSLACK = 29
MAX_CSTATES = 8

try:
    _libc = ctypes.CDLL("libc.so.6", use_errno=True)
    _libc.sched_getcpu.restype = ctypes.c_int
    _libc.prctl.restype = ctypes.c_int
    _libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong,
                            ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    def _sched_getcpu():
        return _libc.sched_getcpu()
    def _prctl_set_timerslack(ns):
        return _libc.prctl(PR_SET_TIMERSLACK, ns, 0, 0, 0)
except Exception:
    def _sched_getcpu():
        return -1
    def _prctl_set_timerslack(ns):
        return -1

# ---------------------------------------------------------------------------
# C spin-wait (releases the GIL so the GUI thread stays responsive)
# ---------------------------------------------------------------------------

_spin_until_ns = None

def _build_spin_lib():
    """Compile a tiny C helper for GIL-free spin-wait."""
    src = r"""
#include <time.h>
#include <stdint.h>
void spin_until_ns(int64_t deadline_ns) {
    struct timespec ts;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if ((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec >= deadline_ns)
            return;
    }
}
"""
    d = tempfile.mkdtemp(prefix="poc_spin_")
    src_path = os.path.join(d, "spin.c")
    lib_path = os.path.join(d, "spin.so")
    with open(src_path, "w") as f:
        f.write(src)
    subprocess.run(
        ["gcc", "-O2", "-shared", "-fPIC", "-o", lib_path, src_path],
        check=True, capture_output=True,
    )
    lib = ctypes.CDLL(lib_path)
    lib.spin_until_ns.restype = None
    lib.spin_until_ns.argtypes = [ctypes.c_int64]
    return lib.spin_until_ns

try:
    _spin_until_ns = _build_spin_lib()
except Exception:
    pass  # fall back to Python spin loop

# ---------------------------------------------------------------------------
# cpuidle C-state helpers
# ---------------------------------------------------------------------------

def _sysfs_read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except Exception:
        return None

def _sysfs_write(path, val):
    try:
        with open(path, "w") as f:
            f.write(str(val))
        return True
    except Exception:
        return False

def cstate_detect():
    """Return list of (name, latency_us) for each C-state on cpu0."""
    states = []
    for s in range(MAX_CSTATES):
        name = _sysfs_read(
            f"/sys/devices/system/cpu/cpu0/cpuidle/state{s}/name")
        if name is None:
            break
        lat = _sysfs_read(
            f"/sys/devices/system/cpu/cpu0/cpuidle/state{s}/latency")
        states.append((name, int(lat) if lat else 0))
    return states

def cstate_save_disable(nr_cstates):
    """Save current disable flags from cpu0."""
    orig = []
    for s in range(nr_cstates):
        v = _sysfs_read(
            f"/sys/devices/system/cpu/cpu0/cpuidle/state{s}/disable")
        orig.append(int(v) if v is not None else -1)
    return orig

def cstate_apply(max_cstate, nr_cstates, nr_cpus):
    """Disable C-states deeper than max_cstate on all CPUs. -1 = no limit."""
    for cpu in range(nr_cpus):
        for s in range(nr_cstates):
            path = (f"/sys/devices/system/cpu/cpu{cpu}"
                    f"/cpuidle/state{s}/disable")
            _sysfs_write(path, 1 if (max_cstate >= 0 and s > max_cstate) else 0)

def cstate_restore(orig, nr_cpus):
    """Restore original disable flags on all CPUs."""
    for cpu in range(nr_cpus):
        for s, v in enumerate(orig):
            if v < 0:
                continue
            path = (f"/sys/devices/system/cpu/cpu{cpu}"
                    f"/cpuidle/state{s}/disable")
            _sysfs_write(path, v)

# ---------------------------------------------------------------------------
# POC sysctl helpers
# ---------------------------------------------------------------------------

def poc_get():
    try:
        with open(SYSCTL_POC_PATH) as f:
            return int(f.read().strip())
    except Exception:
        return -1

def poc_set(val):
    try:
        with open(SYSCTL_POC_PATH, "w") as f:
            f.write(str(val))
        return True
    except Exception:
        return False

def poc_writable():
    return os.access(SYSCTL_POC_PATH, os.W_OK)

# ---------------------------------------------------------------------------
# Worker thread
# ---------------------------------------------------------------------------

class LatencyWorker(threading.Thread):
    """Measure wakeup latency via nanosleep cycles."""

    def __init__(self, sleep_ns_ref, spin_ref, timer_slack_ref, queue):
        super().__init__(daemon=True)
        # All refs are mutable lists [value] shared across workers
        self._sleep_ref = sleep_ns_ref
        self._spin_ref = spin_ref
        self._slack_ref = timer_slack_ref
        self._queue = queue
        self._halt = threading.Event()

    def run(self):
        s_ref = self._sleep_ref
        sp_ref = self._spin_ref
        sl_ref = self._slack_ref
        q = self._queue
        getcpu = _sched_getcpu
        gettime = time.clock_gettime_ns
        CLK = time.CLOCK_MONOTONIC
        slp = time.sleep
        c_spin = _spin_until_ns  # None if build failed

        cur_slack = -1  # track to avoid redundant prctl

        while not self._halt.is_set():
            # apply timer slack if changed
            want_slack = sl_ref[0]
            if want_slack != cur_slack:
                _prctl_set_timerslack(want_slack)
                cur_slack = want_slack

            sleep_ns = s_ref[0]
            cpu0 = getcpu()
            t0 = gettime(CLK)

            if sp_ref[0]:
                if c_spin is not None:
                    # C spin-wait: releases the GIL
                    c_spin(t0 + sleep_ns)
                else:
                    # Python fallback (holds GIL, slow GUI)
                    deadline = t0 + sleep_ns
                    while gettime(CLK) < deadline:
                        pass
            else:
                slp(sleep_ns / 1e9)

            t1 = gettime(CLK)
            cpu1 = getcpu()

            lat = t1 - t0 - sleep_ns
            if lat < 0:
                lat = 0
            q.append((lat, t1, cpu0, cpu1))

    def halt(self):
        self._halt.set()

# ---------------------------------------------------------------------------
# Spectrum analyzer widget
# ---------------------------------------------------------------------------

class SpectrumWidget(QWidget):
    """Spectrum analyzer bars for latency histogram."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumSize(500, 250)
        self._vals = [0.0] * NUM_BUCKETS
        self._disp = [0.0] * NUM_BUCKETS
        self._peak = [0.0] * NUM_BUCKETS
        self._peak_age = [0] * NUM_BUCKETS
        self._smooth = 0.35

    def clear(self):
        self._disp = [0.0] * NUM_BUCKETS
        self._peak = [0.0] * NUM_BUCKETS
        self._peak_age = [0] * NUM_BUCKETS
        self.update()

    def set_values(self, vals):
        for i in range(NUM_BUCKETS):
            tgt = vals[i]
            cur = self._disp[i]
            self._disp[i] = cur + (tgt - cur) * (1.0 - self._smooth)
            if self._disp[i] >= self._peak[i]:
                self._peak[i] = self._disp[i]
                self._peak_age[i] = 0
            else:
                self._peak_age[i] += 1
                if self._peak_age[i] > 30:  # hold ~1s then decay
                    self._peak[i] = max(0.0, self._peak[i] - 0.008)
        self.update()

    def paintEvent(self, _ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        p.fillRect(0, 0, w, h, BG_COLOR)

        ml, mr, mt, mb = 55, 15, 20, 55
        cw = w - ml - mr
        ch = h - mt - mb
        cx, cy = ml, mt

        # grid
        pen_grid = QPen(GRID_COLOR, 1, Qt.DotLine)
        font_sm = QFont("monospace", 8)
        for i in range(5):
            y = int(cy + ch * (1 - i / 4))
            p.setPen(pen_grid)
            p.drawLine(cx, y, cx + cw, y)
            p.setPen(TEXT_DIM)
            p.setFont(font_sm)
            p.drawText(0, y - 8, ml - 6, 16, Qt.AlignRight | Qt.AlignVCenter,
                       f"{i * 25}%")

        # bars
        gap = 6
        bw = (cw - gap * (NUM_BUCKETS + 1)) / NUM_BUCKETS

        for i in range(NUM_BUCKETS):
            x = cx + gap + i * (bw + gap)
            v = self._disp[i]
            bh = v * ch
            by = cy + ch - bh

            if v > 0.002:
                col = BAR_COLORS[i]
                grad = QLinearGradient(x, cy + ch, x, by)
                grad.setColorAt(0.0, col.darker(220))
                grad.setColorAt(0.4, col)
                grad.setColorAt(1.0, col.lighter(140))
                p.setBrush(QBrush(grad))
                p.setPen(Qt.NoPen)
                p.drawRoundedRect(QRectF(x, by, bw, bh), 3, 3)

                # reflection (mirror below baseline)
                ref_h = min(bh * 0.25, 30)
                ref_grad = QLinearGradient(x, cy + ch, x, cy + ch + ref_h)
                rc = QColor(col)
                rc.setAlpha(60)
                ref_grad.setColorAt(0.0, rc)
                rc.setAlpha(0)
                ref_grad.setColorAt(1.0, rc)
                p.setBrush(QBrush(ref_grad))
                p.drawRect(QRectF(x, cy + ch, bw, ref_h))

                # pct label
                p.setPen(TEXT_COLOR)
                p.setFont(QFont("monospace", 8, QFont.Bold))
                p.drawText(int(x), int(by) - 16, int(bw), 14,
                           Qt.AlignCenter, f"{v * 100:.0f}%")

            # peak hold
            pk = self._peak[i]
            if pk > 0.01:
                py_ = int(cy + ch - pk * ch)
                pc = QColor(BAR_COLORS[i])
                pc.setAlpha(200)
                p.setPen(QPen(pc, 2))
                p.drawLine(int(x + 1), py_, int(x + bw - 1), py_)

            # x label
            p.setPen(TEXT_COLOR)
            p.setFont(font_sm)
            p.drawText(int(x), cy + ch + 8, int(bw), 18,
                       Qt.AlignCenter, HIST_LABELS[i])

        # baseline
        p.setPen(QPen(GRID_COLOR, 1))
        p.drawLine(cx, cy + ch, cx + cw, cy + ch)

        # axis title
        p.setPen(TEXT_DIM)
        p.setFont(QFont("monospace", 9))
        p.drawText(cx, cy + ch + 32, cw, 18,
                   Qt.AlignCenter, "Wakeup Latency Distribution")
        p.end()

# ---------------------------------------------------------------------------
# Timeline widget
# ---------------------------------------------------------------------------

class TimelineWidget(QWidget):
    """Time-series p50/p99 with POC state background."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(110)
        self.setMaximumHeight(170)
        self._p50 = deque(maxlen=TIMELINE_MAX)
        self._p99 = deque(maxlen=TIMELINE_MAX)
        self._poc = deque(maxlen=TIMELINE_MAX)
        self._ymax = 10.0

    def clear(self):
        self._p50.clear()
        self._p99.clear()
        self._poc.clear()
        self._ymax = 10.0
        self.update()

    def add(self, p50, p99, poc):
        self._p50.append(p50)
        self._p99.append(p99)
        self._poc.append(poc)
        if self._p99:
            self._ymax = max(2.0, max(self._p99) * 1.3)
        self.update()

    def paintEvent(self, _ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        p.fillRect(0, 0, w, h, BG_DARKER)

        ml, mr, mt, mb = 55, 15, 12, 18
        cw = w - ml - mr
        ch = h - mt - mb
        cx, cy = ml, mt
        n = len(self._p50)
        if n < 2:
            p.setPen(TEXT_DIM)
            p.setFont(QFont("monospace", 10))
            p.drawText(0, 0, w, h, Qt.AlignCenter, "Collecting data\u2026")
            p.end()
            return

        dx = cw / max(1, TIMELINE_MAX - 1)
        off = TIMELINE_MAX - n

        # POC state bands
        poc = list(self._poc)
        for i in range(n):
            x0 = cx + (off + i) * dx
            if poc[i] == 1:
                c = QColor(0, 80, 40, 35)
            elif poc[i] == 0:
                c = QColor(80, 0, 0, 35)
            else:
                c = QColor(40, 40, 40, 25)
            p.fillRect(QRectF(x0, cy, dx + 1, ch), c)

        # grid
        for i in range(5):
            y = int(cy + ch * (1 - i / 4))
            p.setPen(QPen(GRID_COLOR, 1, Qt.DotLine))
            p.drawLine(cx, y, cx + cw, y)
            p.setPen(TEXT_DIM)
            p.setFont(QFont("monospace", 7))
            v = self._ymax * i / 4
            p.drawText(0, y - 7, ml - 6, 14,
                       Qt.AlignRight | Qt.AlignVCenter, f"{v:.1f}\u00b5s")

        # lines
        self._line(p, list(self._p99), cx, cy, cw, ch, off, dx,
                   QColor(255, 90, 90, 180))
        self._line(p, list(self._p50), cx, cy, cw, ch, off, dx,
                   QColor(90, 255, 120, 220))

        # legend
        lx = cx + cw - 110
        p.setFont(QFont("monospace", 8))
        p.setPen(QPen(QColor(90, 255, 120), 2))
        p.drawLine(lx, cy + 6, lx + 14, cy + 6)
        p.setPen(TEXT_COLOR)
        p.drawText(lx + 18, cy, 30, 12, Qt.AlignLeft, "p50")
        p.setPen(QPen(QColor(255, 90, 90), 2))
        p.drawLine(lx + 50, cy + 6, lx + 64, cy + 6)
        p.setPen(TEXT_COLOR)
        p.drawText(lx + 68, cy, 30, 12, Qt.AlignLeft, "p99")
        p.end()

    def _line(self, p, data, cx, cy, cw, ch, off, dx, color):
        pts = []
        for i, v in enumerate(data):
            x = cx + (off + i) * dx
            y = cy + ch * (1 - min(1.0, v / self._ymax))
            pts.append(QPointF(x, y))
        if len(pts) < 2:
            return
        path = QPainterPath(pts[0])
        for pt in pts[1:]:
            path.lineTo(pt)
        p.setPen(QPen(color, 1.5))
        p.setBrush(Qt.NoBrush)
        p.drawPath(path)

# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

_DARK_STYLE = """
QMainWindow, QWidget { background: #121224; color: #c8c8dc; }
QLabel { color: #c8c8dc; font-family: monospace; }
QPushButton {
    background: #22224a; color: #c8c8dc; border: 1px solid #33336a;
    border-radius: 4px; padding: 6px 14px;
    font-family: monospace; font-weight: bold;
}
QPushButton:hover { background: #33335a; }
QPushButton:pressed { background: #18183a; }
QPushButton:disabled { color: #444; border-color: #2a2a40; }
QSpinBox {
    background: #22224a; color: #c8c8dc; border: 1px solid #33336a;
    border-radius: 3px; padding: 3px; font-family: monospace;
}
QCheckBox { color: #c8c8dc; font-family: monospace; spacing: 5px; }
QCheckBox::indicator {
    width: 14px; height: 14px; border: 1px solid #33336a;
    border-radius: 3px; background: #22224a;
}
QCheckBox::indicator:checked { background: #3a7a3a; border-color: #4a9a4a; }
"""


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("POC Latency Spectrum Analyzer")
        self.setMinimumSize(920, 660)
        self.setStyleSheet(_DARK_STYLE)

        root = QWidget()
        self.setCentralWidget(root)
        vbox = QVBoxLayout(root)
        vbox.setContentsMargins(10, 10, 10, 10)
        vbox.setSpacing(6)

        # ---- top bar ----
        top = QHBoxLayout()

        self._poc_lbl = QLabel("POC: ---")
        self._poc_lbl.setFont(QFont("monospace", 13, QFont.Bold))
        top.addWidget(self._poc_lbl)

        self._poc_btn = QPushButton("Toggle POC")
        self._poc_btn.setFixedWidth(120)
        self._poc_btn.clicked.connect(self._toggle_poc)
        if not poc_writable():
            self._poc_btn.setEnabled(False)
            self._poc_btn.setToolTip("root required")
        top.addWidget(self._poc_btn)

        top.addSpacing(20)

        self._stats_lbl = QLabel("mean: --  p50: --  p95: --  p99: --")
        self._stats_lbl.setFont(QFont("monospace", 10))
        top.addWidget(self._stats_lbl, 1)

        self._rate_lbl = QLabel("0 wakeups/s")
        self._rate_lbl.setFont(QFont("monospace", 10))
        self._rate_lbl.setAlignment(Qt.AlignRight)
        top.addWidget(self._rate_lbl)

        vbox.addLayout(top)

        # ---- migration stats ----
        mid = QHBoxLayout()
        self._migr_lbl = QLabel("migration: --")
        self._migr_lbl.setFont(QFont("monospace", 9))
        self._migr_lbl.setStyleSheet("color: #8888aa;")
        mid.addWidget(self._migr_lbl)
        mid.addStretch()
        self._workers_lbl = QLabel("")
        self._workers_lbl.setFont(QFont("monospace", 9))
        self._workers_lbl.setStyleSheet("color: #8888aa;")
        mid.addWidget(self._workers_lbl)
        vbox.addLayout(mid)

        # ---- spectrum ----
        self._spectrum = SpectrumWidget()
        vbox.addWidget(self._spectrum, 3)

        # ---- timeline ----
        self._timeline = TimelineWidget()
        vbox.addWidget(self._timeline, 1)

        # ---- controls row 1 ----
        ctrl = QHBoxLayout()

        ctrl.addWidget(QLabel("Workers:"))
        self._w_spin = QSpinBox()
        ncpu = os.cpu_count() or 4
        self._w_spin.setRange(1, ncpu * 4)
        self._w_spin.setValue(max(1, ncpu * 3 // 4))
        self._w_spin.setFixedWidth(70)
        ctrl.addWidget(self._w_spin)

        ctrl.addSpacing(15)
        ctrl.addWidget(QLabel("Sleep (\u00b5s):"))
        self._s_spin = QSpinBox()
        self._s_spin.setRange(10, 10000)
        self._s_spin.setValue(DEFAULT_SLEEP_US)
        self._s_spin.setFixedWidth(80)
        ctrl.addWidget(self._s_spin)

        ctrl.addStretch()

        self._cstates = cstate_detect()
        self._cs_orig_disable = None
        self._nr_cpus = ncpu

        self._clr_btn = QPushButton("Clear")
        self._clr_btn.setFixedWidth(70)
        self._clr_btn.clicked.connect(self._clear_graphs)
        ctrl.addWidget(self._clr_btn)

        self._go_btn = QPushButton("\u25b6 Start")
        self._go_btn.setFixedWidth(110)
        self._go_btn.clicked.connect(self._toggle_run)
        self._go_btn.setStyleSheet(
            "QPushButton{background:#1a4a1a;border-color:#2a6a2a;}"
            "QPushButton:hover{background:#2a5a2a;}")
        ctrl.addWidget(self._go_btn)
        vbox.addLayout(ctrl)

        # ---- controls row 2 ----
        ctrl2 = QHBoxLayout()

        self._cs_chk = QCheckBox("Max C-state \u2192 C0 (POLL)")
        self._cs_chk.setToolTip("Disable deep C-states, keep CPUs in C0")
        can_cstate = os.access(
            "/sys/devices/system/cpu/cpu0/cpuidle/state0/disable", os.W_OK
        ) if self._cstates else False
        if not can_cstate:
            self._cs_chk.setEnabled(False)
            self._cs_chk.setToolTip("root required")
        ctrl2.addWidget(self._cs_chk)

        ctrl2.addSpacing(15)
        self._ts_chk = QCheckBox("Timer slack \u2192 0")
        self._ts_chk.setToolTip("Set timer slack to 0 ns (minimal)")
        ctrl2.addWidget(self._ts_chk)

        ctrl2.addSpacing(15)
        self._spin_chk = QCheckBox("Spin wait")
        self._spin_chk.setToolTip(
            "Busy-wait instead of nanosleep (no scheduler, no C-state)")
        ctrl2.addWidget(self._spin_chk)

        ctrl2.addStretch()
        vbox.addLayout(ctrl2)

        # ---- state ----
        self._workers = []
        self._queue = deque(maxlen=600_000)
        self._running = False
        self._buf = []
        self._rate_cnt = 0
        self._rate_t = time.monotonic()
        self._cur_rate = 0
        self._cur_p50 = 0.0
        self._cur_p99 = 0.0
        self._sleep_ns_ref = [DEFAULT_SLEEP_US * 1000]  # shared mutable
        self._spin_ref = [False]
        self._timer_slack_ref = [0]

        # live parameter change
        self._w_spin.valueChanged.connect(self._on_workers_changed)
        self._s_spin.valueChanged.connect(self._on_sleep_changed)
        self._cs_chk.stateChanged.connect(self._on_cstate_changed)
        self._ts_chk.stateChanged.connect(self._on_timer_slack_changed)
        self._spin_chk.stateChanged.connect(self._on_spin_changed)

        # timers
        self._tick = QTimer()
        self._tick.timeout.connect(self._on_tick)
        self._tick.setInterval(UPDATE_MS)

        self._tl_tick = QTimer()
        self._tl_tick.timeout.connect(self._on_tl)
        self._tl_tick.setInterval(500)

        self._poc_tick = QTimer()
        self._poc_tick.timeout.connect(self._refresh_poc)
        self._poc_tick.start(1000)
        self._refresh_poc()

    # ---- run control ----

    def _toggle_run(self):
        if self._running:
            self._stop()
        else:
            self._start()

    def _start(self):
        nr = self._w_spin.value()
        self._sleep_ns_ref[0] = self._s_spin.value() * 1000
        self._queue.clear()
        self._buf.clear()

        for _ in range(nr):
            w = LatencyWorker(self._sleep_ns_ref, self._spin_ref,
                              self._timer_slack_ref, self._queue)
            w.start()
            self._workers.append(w)

        self._running = True
        self._go_btn.setText("\u25a0 Stop")
        self._go_btn.setStyleSheet(
            "QPushButton{background:#4a1a1a;border-color:#6a2a2a;}"
            "QPushButton:hover{background:#5a2a2a;}")
        self._update_workers_lbl()
        self._rate_t = time.monotonic()
        self._rate_cnt = 0
        self._tick.start()
        self._tl_tick.start()

    def _stop(self):
        for w in self._workers:
            w.halt()
        self._workers.clear()
        self._running = False
        self._go_btn.setText("\u25b6 Start")
        self._go_btn.setStyleSheet(
            "QPushButton{background:#1a4a1a;border-color:#2a6a2a;}"
            "QPushButton:hover{background:#2a5a2a;}")
        self._tick.stop()
        self._tl_tick.stop()

    def _on_workers_changed(self, new_nr):
        if not self._running:
            return
        cur = len(self._workers)
        if new_nr > cur:
            for _ in range(new_nr - cur):
                w = LatencyWorker(self._sleep_ns_ref, self._spin_ref,
                              self._timer_slack_ref, self._queue)
                w.start()
                self._workers.append(w)
        elif new_nr < cur:
            for _ in range(cur - new_nr):
                w = self._workers.pop()
                w.halt()
        self._update_workers_lbl()

    def _on_sleep_changed(self, val_us):
        self._sleep_ns_ref[0] = val_us * 1000
        if self._running:
            self._update_workers_lbl()

    def _clear_graphs(self):
        self._buf.clear()
        try:
            while True:
                self._queue.popleft()
        except IndexError:
            pass
        self._spectrum.clear()
        self._timeline.clear()
        self._cur_p50 = 0.0
        self._cur_p99 = 0.0
        self._stats_lbl.setText("mean: --  p50: --  p95: --  p99: --")
        self._migr_lbl.setText("migration: --")
        self._rate_cnt = 0
        self._rate_t = time.monotonic()
        self._rate_lbl.setText("0 wakeups/s")

    def _on_cstate_changed(self, state):
        if not self._cstates:
            return
        if self._cs_orig_disable is None:
            self._cs_orig_disable = cstate_save_disable(len(self._cstates))
        if state == Qt.Checked:
            cstate_apply(0, len(self._cstates), self._nr_cpus)
        else:
            cstate_restore(self._cs_orig_disable, self._nr_cpus)
        self._update_workers_lbl()

    def _on_timer_slack_changed(self, state):
        # prctl(PR_SET_TIMERSLACK, 0) resets to default; 1 = minimum
        self._timer_slack_ref[0] = 1 if state == Qt.Checked else 0
        self._update_workers_lbl()

    def _on_spin_changed(self, state):
        self._spin_ref[0] = (state == Qt.Checked)
        self._update_workers_lbl()

    def _update_workers_lbl(self):
        nr = len(self._workers) if self._running else self._w_spin.value()
        parts = [f"{nr} workers",
                 f"sleep {self._s_spin.value()}\u00b5s"]
        if self._cs_chk.isChecked():
            parts.append("C0")
        if self._ts_chk.isChecked():
            parts.append("slack=0")
        if self._spin_chk.isChecked():
            parts.append("spin")
        self._workers_lbl.setText("  \u00b7  ".join(parts))

    # ---- data collection ----

    def _on_tick(self):
        now = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        cutoff = now - WINDOW_MS * 1_000_000

        # bulk drain
        batch = []
        try:
            while True:
                batch.append(self._queue.popleft())
        except IndexError:
            pass

        self._buf.extend(batch)
        self._rate_cnt += len(batch)

        # trim old
        self._buf = [s for s in self._buf if s[1] >= cutoff]

        n = len(self._buf)
        if n < 10:
            return

        # histogram
        hist = [0] * NUM_BUCKETS
        lats = []
        migr = 0
        for lat, _ts, c0, c1 in self._buf:
            lats.append(lat)
            if c0 != c1 and c0 >= 0:
                migr += 1
            for b in range(NUM_BUCKETS):
                if lat <= HIST_BOUNDS_NS[b]:
                    hist[b] += 1
                    break

        total = sum(hist)
        if total == 0:
            return
        frac = [h / total for h in hist]
        self._spectrum.set_values(frac)

        # percentiles (subsample if huge)
        if n > 50_000:
            step = n // 50_000
            lats = lats[::step]
        lats.sort()
        sn = len(lats)
        p50 = lats[int(sn * 0.50)] / 1000
        p95 = lats[int(sn * 0.95)] / 1000
        p99 = lats[min(int(sn * 0.99), sn - 1)] / 1000
        mean = sum(lats) / sn / 1000
        self._cur_p50 = p50
        self._cur_p99 = p99

        self._stats_lbl.setText(
            f"mean: {mean:.2f}\u00b5s  p50: {p50:.2f}\u00b5s  "
            f"p95: {p95:.2f}\u00b5s  p99: {p99:.2f}\u00b5s")

        migr_pct = migr / n * 100 if n else 0
        self._migr_lbl.setText(
            f"migration: {migr_pct:.1f}%  ({migr}/{n} samples)")

        # rate
        t = time.monotonic()
        dt = t - self._rate_t
        if dt >= 1.0:
            self._cur_rate = int(self._rate_cnt / dt)
            self._rate_lbl.setText(f"{self._cur_rate:,} wakeups/s")
            self._rate_cnt = 0
            self._rate_t = t

    def _on_tl(self):
        self._timeline.add(self._cur_p50, self._cur_p99, poc_get())

    # ---- POC ----

    def _refresh_poc(self):
        s = poc_get()
        if s == 1:
            self._poc_lbl.setText("POC: ON")
            self._poc_lbl.setStyleSheet(
                "color: #00ff64; font-family: monospace;")
        elif s == 0:
            self._poc_lbl.setText("POC: OFF")
            self._poc_lbl.setStyleSheet(
                "color: #ff3c3c; font-family: monospace;")
        else:
            self._poc_lbl.setText("POC: N/A")
            self._poc_lbl.setStyleSheet(
                "color: #666; font-family: monospace;")

    def _toggle_poc(self):
        cur = poc_get()
        if cur >= 0:
            poc_set(1 - cur)
            self._refresh_poc()

    def closeEvent(self, ev):
        self._stop()
        # restore C-state limits
        if self._cs_orig_disable is not None:
            cstate_restore(self._cs_orig_disable, self._nr_cpus)
        ev.accept()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
