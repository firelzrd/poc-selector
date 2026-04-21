"""POC Selector 2.5.6 plugin — target_sticky + smt_fallback + eager_commit + early_select + greedy_search + lockless_bitmap toggles."""

from PyQt5.QtWidgets import QCheckBox, QHBoxLayout
import os

SYSCTL_TARGET_STICKY    = "/proc/sys/kernel/sched_poc_target_sticky"
SYSCTL_SMT_FALLBACK     = "/proc/sys/kernel/sched_poc_smt_fallback"
SYSCTL_EAGER_COMMIT     = "/proc/sys/kernel/sched_poc_eager_commit"
SYSCTL_EARLY_SELECT     = "/proc/sys/kernel/sched_poc_early_select"
SYSCTL_GREEDY_SEARCH    = "/proc/sys/kernel/sched_poc_greedy_search"
SYSCTL_LOCKLESS_BITMAP  = "/proc/sys/kernel/sched_poc_lockless_bitmap"


def _sysctl_read(path):
    try:
        with open(path) as f:
            return int(f.read().strip())
    except Exception:
        return -1


def _sysctl_write(path, val):
    try:
        with open(path, "w") as f:
            f.write(str(val))
        return True
    except Exception:
        return False


def _make_toggle(layout, label, tooltip, sysctl_path, writable):
    """Create a checkbox bound to a boolean sysctl."""
    chk = QCheckBox(label)
    chk.setToolTip(tooltip)
    cur = _sysctl_read(sysctl_path)
    if cur >= 0:
        chk.setChecked(bool(cur))
    if not writable:
        chk.setEnabled(False)
        chk.setToolTip("root required")
    chk.stateChanged.connect(
        lambda s: _sysctl_write(sysctl_path, 1 if s else 0))
    layout.addWidget(chk)
    return chk


def setup(layout):
    """Called by MainWindow to populate plugin controls row."""
    writable = os.access(SYSCTL_TARGET_STICKY, os.W_OK)

    row = QHBoxLayout()
    row.setContentsMargins(0, 0, 0, 0)

    _make_toggle(row, "Early select",
        "sched_poc_early_select: check recent_used_cpu and target for "
        "fully idle core before POC bitmap search, matching upstream "
        "CFS Gate 4 behavior (default: ON)",
        SYSCTL_EARLY_SELECT, writable)
    row.addSpacing(15)

    _make_toggle(row, "SMT fallback",
        "sched_poc_smt_fallback: bail out to CFS when has_idle_cores "
        "is false (default: OFF)",
        SYSCTL_SMT_FALLBACK, writable)
    row.addSpacing(15)

    _make_toggle(row, "Lockless bitmap",
        "sched_poc_lockless_bitmap: use u8[64] flag array with plain "
        "WRITE_ONCE (no LOCK prefix) for idle state tracking. "
        "ON=lockless flag array, OFF=atomic64 bitmap with LOCK'd "
        "writes (default). Toggle for A/B benchmarking.",
        SYSCTL_LOCKLESS_BITMAP, writable)

    _make_toggle(row, "Target sticky",
        "sched_poc_target_sticky: if target CPU is idle, return it "
        "immediately for L1/TLB cache affinity (default: OFF)",
        SYSCTL_TARGET_STICKY, writable)
    row.addSpacing(15)

    _make_toggle(row, "Greedy search",
        "sched_poc_greedy_search: always attempt Level 5/6 LLC-wide "
        "SMT sibling search regardless of utilization, ignoring the "
        "SIS_UTIL overload gate (default: ON)",
        SYSCTL_GREEDY_SEARCH, writable)
    row.addSpacing(15)

    _make_toggle(row, "Eager commit",
        "sched_poc_eager_commit: commit selection to idle bitmap "
        "(atomic64_andnot) at selection time to close the race window "
        "where multiple wakers select the same idle CPU (default: ON)",
        SYSCTL_EAGER_COMMIT, writable)
    row.addSpacing(15)

    row.addStretch()
    layout.addLayout(row)
