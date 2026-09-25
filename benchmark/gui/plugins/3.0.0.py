"""POC Selector 3.0.0 plugin — waker_yield + target_sticky + smt_fallback + greedy_search + rr_improved toggles."""

from PyQt5.QtWidgets import QCheckBox, QHBoxLayout
import os

SYSCTL_WAKER_YIELD      = "/proc/sys/kernel/sched_poc_waker_yield"
SYSCTL_TARGET_STICKY    = "/proc/sys/kernel/sched_poc_target_sticky"
SYSCTL_SMT_FALLBACK     = "/proc/sys/kernel/sched_poc_smt_fallback"
SYSCTL_RR_IMPROVED      = "/proc/sys/kernel/sched_poc_rr_improved"
SYSCTL_GREEDY_SEARCH    = "/proc/sys/kernel/sched_poc_greedy_search"


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
    writable = os.access(SYSCTL_WAKER_YIELD, os.W_OK)

    row = QHBoxLayout()
    row.setContentsMargins(0, 0, 0, 0)

    _make_toggle(row, "Waker yield (L0w)",
        "sched_poc_waker_yield: on a WF_SYNC wakeup that wake_affine() "
        "pulled to the waker's CPU, place the wakee on that CPU itself "
        "(Level 0w): zero follow-on migration, warm L1/L2/TLB. Each "
        "waker's sync wakes are learned, and only a waker whose lies "
        "(keeping on running after the wake) cost less than its kept "
        "promises gain gets the handoff. OFF: every wakeup is placed as "
        "for a dishonest waker; the history is still learned. "
        "Default: ON",
        SYSCTL_WAKER_YIELD, writable)
    row.addSpacing(15)

    _make_toggle(row, "SMT fallback",
        "sched_poc_smt_fallback: bail out to CFS when has_idle_cores "
        "is false (default: OFF)",
        SYSCTL_SMT_FALLBACK, writable)
    row.addSpacing(15)

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

    _make_toggle(row, "Improved RR",
        "sched_poc_rr_improved: use the improved RR strategy for idle "
        "CPU selection in poc_select_rr, poc_cluster_search, and the "
        "packed priority search. ON=total-size case-split (1/2/>=3) "
        "combined with golden-ratio scrambling (Lemire fastrange). "
        "OFF=current strategy unchanged (poc_rr_step table / ctz "
        "lowest-bit / ror32). Toggle for A/B benchmarking (default: ON)",
        SYSCTL_RR_IMPROVED, writable)
    row.addSpacing(15)

    row.addStretch()
    layout.addLayout(row)
