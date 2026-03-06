"""POC Selector 2.2.7 plugin — compat preset + individual compat/feature toggles."""

from PyQt5.QtWidgets import (QCheckBox, QComboBox, QHBoxLayout, QLabel,
                              QVBoxLayout, QWidget)
import os

SYSCTL_L2_CLUSTER_SEARCH = "/proc/sys/kernel/sched_poc_l2_cluster_search"
SYSCTL_PREFER_IDLE_SMT = "/proc/sys/kernel/sched_poc_prefer_idle_smt"
SYSCTL_SMT_FALLBACK = "/proc/sys/kernel/sched_poc_smt_fallback"
SYSCTL_EARLY_CLEAR = "/proc/sys/kernel/sched_poc_early_clear"
SYSCTL_COMPAT = "/proc/sys/kernel/sched_poc_compat"
SYSCTL_COMPAT_LEVEL1_CPU = "/proc/sys/kernel/sched_poc_compat_level1_cpu"
SYSCTL_COMPAT_NO_CFS_GATE = "/proc/sys/kernel/sched_poc_compat_no_cfs_gate"
SYSCTL_COMPAT_CLUSTER_CTZ = "/proc/sys/kernel/sched_poc_compat_cluster_ctz"

# combo index → sysctl value
_COMPAT_VALUES = [0, 19, 21]


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
    writable = os.access(SYSCTL_L2_CLUSTER_SEARCH, os.W_OK)

    # Two-row container inside the passed QHBoxLayout
    container = QWidget()
    vbox = QVBoxLayout(container)
    vbox.setContentsMargins(0, 0, 0, 0)
    vbox.setSpacing(2)

    # === Row 1: Feature toggles ===
    row1 = QHBoxLayout()
    row1.setContentsMargins(0, 0, 0, 0)

    chk_l2 = _make_toggle(row1, "L2 cluster search",
        "sched_poc_l2_cluster_search: search within L2 cluster first "
        "before LLC-wide search",
        SYSCTL_L2_CLUSTER_SEARCH, writable)
    row1.addSpacing(15)

    chk_smt = _make_toggle(row1, "Prefer idle SMT",
        "sched_poc_prefer_idle_smt: try target/sibling first "
        "regardless of core_mask (Level 4 always runs)",
        SYSCTL_PREFER_IDLE_SMT, writable)
    row1.addSpacing(15)

    chk_fb = _make_toggle(row1, "SMT fallback",
        "sched_poc_smt_fallback: bail out to CFS when has_idle_cores "
        "is false",
        SYSCTL_SMT_FALLBACK, writable)
    row1.addSpacing(15)

    chk_ec = _make_toggle(row1, "Early clear",
        "sched_poc_early_clear: atomically clear selected CPU's bit "
        "at selection time to close the race window",
        SYSCTL_EARLY_CLEAR, writable)

    row1.addStretch()
    vbox.addLayout(row1)

    # === Row 2: Compat preset + individual toggles ===
    row2 = QHBoxLayout()
    row2.setContentsMargins(0, 0, 0, 0)

    lbl = QLabel("Compat:")
    row2.addWidget(lbl)

    combo = QComboBox()
    combo.addItems(["Off (v2.2.7)", "v1.9.3", "v2.1.0"])
    combo.setToolTip(
        "sched_poc_compat: preset that sets all compat and feature keys "
        "at once (0=native, 19=v1.9.3, 21=v2.1.0)")
    cur_compat = _sysctl_read(SYSCTL_COMPAT)
    if cur_compat in _COMPAT_VALUES:
        combo.setCurrentIndex(_COMPAT_VALUES.index(cur_compat))
    if not writable:
        combo.setEnabled(False)
        combo.setToolTip("root required")
    row2.addWidget(combo)
    row2.addSpacing(15)

    chk_l1cpu = _make_toggle(row2, "Compat: L1 CPU sticky",
        "sched_poc_compat_level1_cpu: check target CPU idle before "
        "core_mask (v1.9.3 Level 1 behavior)",
        SYSCTL_COMPAT_LEVEL1_CPU, writable)
    row2.addSpacing(15)

    chk_nocfs = _make_toggle(row2, "Compat: No CFS gate",
        "sched_poc_compat_no_cfs_gate: skip L1b/L4a/L4b/SIS_UTIL; "
        "Level 4 checks target SMT instead of prev (v1.9.3/v2.1.0)",
        SYSCTL_COMPAT_NO_CFS_GATE, writable)
    row2.addSpacing(15)

    chk_ctz = _make_toggle(row2, "Compat: Cluster CTZ",
        "sched_poc_compat_cluster_ctz: cluster search uses CTZ "
        "(lowest idle CPU) instead of round-robin (v1.9.3)",
        SYSCTL_COMPAT_CLUSTER_CTZ, writable)

    row2.addStretch()
    vbox.addLayout(row2)

    layout.addWidget(container)

    # --- Compat preset change handler ---
    all_toggles = [
        (chk_smt,   SYSCTL_PREFER_IDLE_SMT),
        (chk_fb,    SYSCTL_SMT_FALLBACK),
        (chk_ec,    SYSCTL_EARLY_CLEAR),
        (chk_l1cpu, SYSCTL_COMPAT_LEVEL1_CPU),
        (chk_nocfs, SYSCTL_COMPAT_NO_CFS_GATE),
        (chk_ctz,   SYSCTL_COMPAT_CLUSTER_CTZ),
    ]

    def _on_compat_changed(index):
        val = _COMPAT_VALUES[index]
        _sysctl_write(SYSCTL_COMPAT, val)
        for chk, path in all_toggles:
            v = _sysctl_read(path)
            if v >= 0:
                chk.blockSignals(True)
                chk.setChecked(bool(v))
                chk.blockSignals(False)

    combo.currentIndexChanged.connect(_on_compat_changed)
