"""POC Selector 2.5.0-rc3 plugin — target_sticky + smt_fallback + early_clear + early_recent_core toggles."""

from PyQt5.QtWidgets import QCheckBox, QHBoxLayout
import os

SYSCTL_TARGET_STICKY = "/proc/sys/kernel/sched_poc_target_sticky"
SYSCTL_SMT_FALLBACK = "/proc/sys/kernel/sched_poc_smt_fallback"
SYSCTL_EARLY_CLEAR = "/proc/sys/kernel/sched_poc_early_clear"
SYSCTL_EARLY_RECENT_CORE = "/proc/sys/kernel/sched_poc_early_recent_core"


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

    _make_toggle(row, "Target sticky",
        "sched_poc_target_sticky: if target CPU is idle, return it "
        "immediately for L1/TLB cache affinity (default: ON)",
        SYSCTL_TARGET_STICKY, writable)
    row.addSpacing(15)

    _make_toggle(row, "SMT fallback",
        "sched_poc_smt_fallback: bail out to CFS when has_idle_cores "
        "is false (default: OFF)",
        SYSCTL_SMT_FALLBACK, writable)
    row.addSpacing(15)

    _make_toggle(row, "Early clear",
        "sched_poc_early_clear: atomically clear selected CPU's bit "
        "at selection time to close the race window (default: ON)",
        SYSCTL_EARLY_CLEAR, writable)
    row.addSpacing(15)

    _make_toggle(row, "Early recent core",
        "sched_poc_early_recent_core: return recent_used_cpu immediately "
        "if its core is fully idle, before POC bitmap search (default: OFF)",
        SYSCTL_EARLY_RECENT_CORE, writable)

    row.addStretch()
    layout.addLayout(row)
