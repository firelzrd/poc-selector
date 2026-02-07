# Piece-Of-Cake (POC) CPU Selector

A kernel patch that accelerates idle CPU discovery using per-LLC atomic bitmasks,
achieving O(1) lookup instead of linear scanning.

<div align="center"><img width="256" height="224" alt="poc" src="https://github.com/user-attachments/assets/c9452565-3498-430b-9d87-706662956968" /></div>

## Inspiration

This project was born from the ideas pioneered by
[RitzDaCat](https://github.com/RitzDaCat) in
[scx_cake](https://github.com/RitzDaCat/scx_cake) — a sched_ext BPF scheduler of extraordinary originality and ambition.  
Where most scheduler projects (including mine) iterate on well-known designs, scx_cake charted its own course: a from-scratch architecture that boldly rethinks how scheduling decisions should be made.  
The creative vision and technical depth behind scx_cake are truly remarkable, and studying it was a catalyst for exploring what a similar bitmask-driven approach could look like inside the mainline CFS code path.

POC Selector distills one specific insight from scx_cake — fast idle-CPU selection via cached bitmasks — and transplants it into the kernel's `select_idle_cpu()` hot path as a lightweight, non-invasive patch.

## Key Characteristics

- **O(1) idle CPU discovery** via atomic64 bitmasks
- **6-level priority hierarchy** for cache locality optimization
- **Zero-overhead when disabled** via static keys
- **Supports up to 128 CPUs per LLC** (2 × 64-bit words)

## Features

- **Fast idle CPU search** — Bitmap-based O(1) lookup replaces O(n) linear scan
- **SMT contention avoidance** — Strict preference for idle physical cores over SMT siblings
- **Cache hierarchy awareness** — L1 → L2 → L3 locality optimization

## How It Works

POC Selector maintains **per-LLC `atomic64_t` bitmasks** that track which CPUs (and which physical cores) are idle.
When the scheduler needs an idle CPU for task wakeup, it consults these bitmasks instead of scanning every CPU in the domain.

When the fast path cannot handle the request (LLC > 128 CPUs, restricted affinity, etc.), the standard `select_idle_cpu()` takes over transparently.

### Key Properties

- **Lock-free** — each CPU only modifies its own bit; no spinlocks needed
- **SMT-aware** — prefers idle physical cores over idle SMT siblings
- **Zero fairness impact** — only changes *where* a task is placed, not scheduling order
- **Runtime toggle** — `sysctl kernel.sched_poc_selector` (0/1, default 1)

---

## Technical Comparison

### SMT Sibling vs Idle Core Priority

The key philosophical difference between the iterational logic and POC lies in how they handle the trade-off between CPU selection latency and task execution throughput:

| Aspect | CFS (Standard) | POC |
|--------|----------------|-----|
| When `has_idle_core=false` | Returns SMT sibling immediately | Still searches for idle cores |
| Search strategy | Minimize selection cycles | Maximize task throughput |
| SMT contention | Accepted for faster selection | Avoided via strict core priority |

**CFS default behavior**:
```c
if (!has_idle_core && cpus_share_cache(prev, target)) {
    i = select_idle_smt(p, sd, prev);  // Return SMT sibling immediately
    if ((unsigned int)i < nr_cpumask_bits)
        return i;
}
```

**POC behavior**: Always executes idle core search first (Phase 2), falling back to SMT siblings only when all physical cores are busy (Phase 3).

### 6-Level Priority Hierarchy

POC implements a strict 6-level priority hierarchy optimized for cache locality:

```
Phase 1: Early Return
  Level 0: Saturation check     — No idle CPUs → return -1 (fallback to CFS)
  Level 1: Target sticky        — Target CPU itself is idle (best L1/L2/L3 locality)

Phase 2: Core Search (SMT systems only, no contention)
  Level 2: L2 cluster idle core — Idle core within L2 cluster
  Level 3: LLC-wide idle core   — Idle core anywhere in LLC

Phase 3: CPU Search (all cores busy, SMT fallback)
  Level 4: Target SMT sibling   — Idle sibling of target (L1+L2 shared)
  Level 5: L2 cluster SMT       — Any idle CPU within L2 cluster
  Level 6: LLC-wide CPU         — Any idle CPU via round-robin
```

### Performance Trade-off Analysis

The "inversion phenomenon": POC's strict idle core priority may appear to cost more CPU selection cycles, but delivers superior task throughput:

| Metric | Cost/Benefit |
|--------|--------------|
| CPU selection overhead | ~20-50 additional cycles (O(1) bitmap ops) |
| SMT contention avoidance | 15-40% throughput improvement |
| Break-even point | Task runtime > ~1000 cycles (virtually all workloads) |

**Why this trade-off favors POC:**
- SMT siblings share execution units (ALU, FPU, load/store units)
- Typical SMT throughput penalty: 15-40% depending on workload
- POC's additional selection cost (~50 cycles) is negligible compared to execution savings

---

## POC Optimization Techniques

### Bit Manipulation Primitives

#### POC_CTZ64 (Count Trailing Zeros)

Three-tier architecture detection for optimal CTZ implementation:

| Tier | Platform | Implementation | Typical Latency |
|------|----------|----------------|-----------------|
| 1 | x86-64 + BMI1 | TZCNT instruction | ~3 cycles |
| 1 | ARM64 | RBIT + CLZ | ~2 cycles |
| 1 | RISC-V Zbb | ctz instruction | ~1 cycle |
| 2 | x86-64 (no BMI1) | BSF + zero check | ~4 cycles |
| 3 | Fallback | De Bruijn lookup | ~10 cycles |

**De Bruijn fallback**: Based on Leiserson, Prokop, Randall (1998)
- 64-entry lookup table + multiplication
- Branchless O(1) operation

#### POC_PTSELECT (Position Select)

Select the position of the j-th set bit in a 64-bit word:

| Tier | Platform | Implementation | Complexity |
|------|----------|----------------|------------|
| 1 | x86-64 + BMI2 | PDEP + TZCNT | O(1) |
| 2 | Fallback | Iterative bit-clear | O(j) |

**Note**: AMD Zen 1/2 excluded from PDEP path due to slow microcode implementation.

**Reference**: Pandey, Bender, Johnson, "A Fast x86 Implementation of Select" (arXiv:1706.00990, 2017)

#### POPCNT (Population Count)

- **x86-64**: Runtime detection via `boot_cpu_has(X86_FEATURE_POPCNT)`
- **ARM64**: CNT instruction
- **RISC-V Zbb**: cpop instruction
- **Fallback**: hweight64() software implementation

---

### POC_FASTRANGE (Division-Free Range Mapping)

```c
#define POC_FASTRANGE(seed, range) ((u32)(((u64)(seed) * (u32)(range)) >> 32))
```

Maps [0, 2^32) → [0, range) without division using Lemire's fastrange algorithm.

**Reference**: Lemire, "Fast Random Integer Generation in an Interval" (ACM TOMACS, 2019)

---

### Static Keys (Zero-Cost Runtime Switching)

| Key | Default | Purpose |
|-----|---------|---------|
| `sched_poc_enabled` | true | Master POC on/off |
| `sched_poc_l2_cluster_search` | true | L2 cluster search |
| `sched_poc_single_word` | true | Single-word optimization (≤64 CPUs) |
| `sched_cluster_active` | auto | Cluster topology detection |

- When disabled: Compiles to NOP (complete zero overhead)
- Dynamically patches kernel text at runtime

---

### Per-CPU Round-Robin Counter

```c
static DEFINE_PER_CPU(u32, poc_rr_counter);
#define POC_HASH_MULT 0x9E3779B9U  /* golden ratio * 2^32 */

seed = __this_cpu_inc_return(poc_rr_counter) * POC_HASH_MULT;
```

**Benefits**:
- Zero atomic contention (per-CPU variable)
- CPU ID embedded in upper 8 bits → different CPUs produce different seeds
- Golden ratio multiplication → uniform distribution
- Initialization: `per_cpu(poc_rr_counter, cpu) = (u32)cpu << 24`

---

### Lock-Free Atomic Bitmask

```c
atomic64_t poc_idle_cpus[POC_MASK_WORDS_MAX];   // Logical CPUs
atomic64_t poc_idle_cores[POC_MASK_WORDS_MAX];  // Physical cores (SMT only)
```

**Update operations**:
- `atomic64_or()`: Set bit (CPU goes idle)
- `atomic64_andnot()`: Clear bit (CPU goes busy)
- Each CPU only modifies its own bit → no locking required

**Memory barriers**:
- `smp_mb__after_atomic()`: On x86, compiles to compiler barrier only (0 cycles)
- On ARM64: emits `dmb ish`

---

### Pre-computed Masks

| Mask | Purpose | Lookup Complexity |
|------|---------|-------------------|
| `poc_smt_siblings[bit]` | SMT sibling mask per CPU | O(1) |
| `poc_cluster_mask[bit]` | L2 cluster mask per CPU | O(1) |

- Computed at boot time in topology.c
- Avoids runtime cpumask iteration

---

### Macro-Expanded Variants

```c
DEFINE_SELECT_IDLE_CPU_POC(1)  // Up to 64 CPUs per LLC
DEFINE_SELECT_IDLE_CPU_POC(2)  // Up to 128 CPUs per LLC
```

- Fully expanded at compile time
- Loop counters are constants → complete unrolling

---

### Deferred Evaluation

- POPCNT calls are deferred until actually needed
- Level 1 (target sticky) early return avoids all POPCNT overhead

---

## Requirements / Limitations

- **Kernel**: Linux kernel built with `CONFIG_SCHED_POC_SELECTOR=y` (default)
- **SMP**: Requires `CONFIG_SMP` (multi-processor kernel)
- **Max 128 logical CPUs per LLC**: The bitmask is backed by 2 × `atomic64_t` words (`POC_MASK_WORDS_MAX = 2`), covering up to 128 CPUs per Last-Level Cache domain
- **Graceful fallback**: When the LLC contains more than 128 CPUs, a task has restricted CPU affinity (`taskset`, `cpuset`, etc.), or no idle CPUs exist in the LLC, the selector transparently falls back to the standard `select_idle_cpu()` — no error, no performance penalty beyond losing the fast path
- **Runtime toggle**: Can be disabled at runtime via `sysctl kernel.sched_poc_selector=0`

---

## Configuration

### POC Parameters (sysctl)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `kernel.sched_poc_selector` | 1 | Enable/disable POC selector |
| `kernel.sched_poc_l2_cluster_search` | 1 | Enable/disable L2 cluster search |

---

## Debug Interface

When `CONFIG_SCHED_POC_SELECTOR_DEBUG=y` is enabled, statistics are exposed via sysfs:

```
/sys/kernel/poc_selector/
├── hit               # Total POC successes
├── fallthrough       # POC failures (fell through to CFS)
├── sticky            # Level 1 hits (target was idle)
├── l2_hit            # Level 2 hits (L2 cluster idle core)
├── llc_hit           # Level 3 hits (LLC-wide idle core)
├── smt_tgt           # Level 4 hits (target SMT sibling)
├── l2_smt            # Level 5 hits (L2 cluster SMT)
├── reset             # Write to reset all counters (root only)
├── selected/
│   └── cpu{N}        # Per-CPU selection counts
└── hw_accel/
    ├── ctz           # CTZ implementation in use
    ├── ptselect      # PTSelect implementation in use
    └── popcnt        # POPCNT implementation in use
```

---

## Patch

Apply the patch to a Linux source tree:

```bash
cd /path/to/linux-6.18.3
git apply /path/to/poc-selector/patches/0001-6.18.3-poc-selector-v1.0.patch
```

After building and booting the patched kernel, the feature is enabled by default.
Toggle at runtime:

```bash
# Disable
sudo sysctl kernel.sched_poc_selector=0

# Enable
sudo sysctl kernel.sched_poc_selector=1
```

---

## Building the Benchmark

```bash
cd benchmark
make
sudo ./poc_bench
```

Options:

```
-i, --iterations <N>    Number of iterations (default: 100000)
-t, --threads <N>       Worker threads (default: nproc)
-b, --background <N>    Background burn threads (default: nproc/2)
-w, --warmup <N>        Warmup iterations (default: 5000)
--no-compare            Single run without ON/OFF comparison
```

The benchmark requires root to toggle `/proc/sys/kernel/sched_poc_selector`.

---

## Special Thanks

RitzDaCat - of course, for giving birth to scx_cake inspiring me of implementing the selector.
Mario Roy - for advising me about the PTSelect algorithm use

## License

GPL-2.0 — see [LICENSE](LICENSE).
