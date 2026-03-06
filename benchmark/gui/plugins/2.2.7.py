"""POC Selector 2.2.7 plugin — compat mode selector + l2_cluster_search, prefer_idle_smt, smt_fallback & early_clear toggles."""

from PyQt5.QtWidgets import QCheckBox, QComboBox, QLabel
import os

SYSCTL_L2_CLUSTER_SEARCH = "/proc/sys/kernel/sched_poc_l2_cluster_search"
SYSCTL_PREFER_IDLE_SMT = "/proc/sys/kernel/sched_poc_prefer_idle_smt"
SYSCTL_SMT_FALLBACK = "/proc/sys/kernel/sched_poc_smt_fallback"
SYSCTL_EARLY_CLEAR = "/proc/sys/kernel/sched_poc_early_clear"
SYSCTL_COMPAT = "/proc/sys/kernel/sched_poc_compat"

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


def setup(layout):
    """Called by MainWindow to populate plugin controls row."""
    writable = os.access(SYSCTL_L2_CLUSTER_SEARCH, os.W_OK)

    # --- Compat mode selector ---
    lbl = QLabel("Compat:")
    layout.addWidget(lbl)

    combo = QComboBox()
    combo.addItems(["Off (v2.2.7)", "v1.9.3", "v2.1.0"])
    combo.setToolTip(
        "sched_poc_compat: emulate v1.9.3 or v2.1.0 selection logic "
        "via static-key presets (0=native, 19=v1.9.3, 21=v2.1.0)")
    cur_compat = _sysctl_read(SYSCTL_COMPAT)
    if cur_compat in _COMPAT_VALUES:
        combo.setCurrentIndex(_COMPAT_VALUES.index(cur_compat))
    if not writable:
        combo.setEnabled(False)
        combo.setToolTip("root required")
    layout.addWidget(combo)

    layout.addSpacing(15)

    # --- Checkbox toggles ---
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

    chk_smt = QCheckBox("Prefer idle SMT")
    chk_smt.setToolTip(
        "sched_poc_prefer_idle_smt: try target/sibling first "
        "regardless of core_mask (Level 4 always runs)")
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

    chk_fb = QCheckBox("SMT fallback")
    chk_fb.setToolTip(
        "sched_poc_smt_fallback: when enabled, bail out to CFS when "
        "has_idle_cores is false; when disabled, POC handles SMT "
        "sibling selection itself")
    cur = _sysctl_read(SYSCTL_SMT_FALLBACK)
    if cur >= 0:
        chk_fb.setChecked(bool(cur))
    if not writable:
        chk_fb.setEnabled(False)
        chk_fb.setToolTip("root required")
    chk_fb.stateChanged.connect(
        lambda s: _sysctl_write(SYSCTL_SMT_FALLBACK, 1 if s else 0))
    layout.addWidget(chk_fb)

    layout.addSpacing(15)

    chk_ec = QCheckBox("Early clear")
    chk_ec.setToolTip(
        "sched_poc_early_clear: atomically clear selected CPU's bit in "
        "poc_idle_cpus_mask at selection time to close the race window "
        "where multiple wakers could pick the same idle CPU")
    cur = _sysctl_read(SYSCTL_EARLY_CLEAR)
    if cur >= 0:
        chk_ec.setChecked(bool(cur))
    if not writable:
        chk_ec.setEnabled(False)
        chk_ec.setToolTip("root required")
    chk_ec.stateChanged.connect(
        lambda s: _sysctl_write(SYSCTL_EARLY_CLEAR, 1 if s else 0))
    layout.addWidget(chk_ec)

    # --- Compat mode change handler ---
    # Re-read all toggles after compat preset applies its static-key changes.
    checkboxes = [
        (chk_smt, SYSCTL_PREFER_IDLE_SMT),
        (chk_fb,  SYSCTL_SMT_FALLBACK),
        (chk_ec,  SYSCTL_EARLY_CLEAR),
    ]

    def _on_compat_changed(index):
        val = _COMPAT_VALUES[index]
        _sysctl_write(SYSCTL_COMPAT, val)
        for chk, path in checkboxes:
            v = _sysctl_read(path)
            if v >= 0:
                chk.blockSignals(True)
                chk.setChecked(bool(v))
                chk.blockSignals(False)

    combo.currentIndexChanged.connect(_on_compat_changed)
