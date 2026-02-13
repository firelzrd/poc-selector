"""POC Selector 2.0.0-rc3 plugin — l2_cluster_search, prefer_idle_smt & fallback_target toggles."""

from PyQt5.QtWidgets import QCheckBox
import os

SYSCTL_L2_CLUSTER_SEARCH = "/proc/sys/kernel/sched_poc_l2_cluster_search"
SYSCTL_PREFER_IDLE_SMT = "/proc/sys/kernel/sched_poc_prefer_idle_smt"
SYSCTL_FALLBACK_TARGET = "/proc/sys/kernel/sched_poc_fallback_target"


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


def setup(layout):
    """Called by MainWindow to populate plugin controls row."""
    writable = os.access(SYSCTL_PREFER_IDLE_SMT, os.W_OK)

    # l2_cluster_search toggle
    chk_l2 = QCheckBox("L2 cluster search")
    chk_l2.setToolTip(
        "sched_poc_l2_cluster_search: search within L2 cluster first "
        "before LLC-wide search")
    cur = _sysctl_read(SYSCTL_L2_CLUSTER_SEARCH)
    if cur >= 0:
        chk_l2.setChecked(bool(cur))
    if not writable:
        chk_l2.setEnabled(False)
        chk_l2.setToolTip("root required")
    chk_l2.stateChanged.connect(
        lambda s: _sysctl_write(SYSCTL_L2_CLUSTER_SEARCH, 1 if s else 0))
    layout.addWidget(chk_l2)

    layout.addSpacing(15)

    # prefer_idle_smt toggle
    chk_smt = QCheckBox("Prefer idle SMT")
    chk_smt.setToolTip(
        "sched_poc_prefer_idle_smt: try prev/sibling first "
        "regardless of core_mask")
    cur = _sysctl_read(SYSCTL_PREFER_IDLE_SMT)
    if cur >= 0:
        chk_smt.setChecked(bool(cur))
    if not writable:
        chk_smt.setEnabled(False)
        chk_smt.setToolTip("root required")
    chk_smt.stateChanged.connect(
        lambda s: _sysctl_write(SYSCTL_PREFER_IDLE_SMT, 1 if s else 0))
    layout.addWidget(chk_smt)

    layout.addSpacing(15)

    # fallback_target toggle
    chk_fb = QCheckBox("Fallback \u2192 target")
    chk_fb.setToolTip(
        "sched_poc_fallback_target: return target instead of -1 "
        "on saturation")
    cur = _sysctl_read(SYSCTL_FALLBACK_TARGET)
    if cur >= 0:
        chk_fb.setChecked(bool(cur))
    if not writable:
        chk_fb.setEnabled(False)
        chk_fb.setToolTip("root required")
    chk_fb.stateChanged.connect(
        lambda s: _sysctl_write(SYSCTL_FALLBACK_TARGET, 1 if s else 0))
    layout.addWidget(chk_fb)
