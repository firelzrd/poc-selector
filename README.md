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

- **O(1) idle CPU discovery** via a single atomic64 bitmask per LLC
- **7-level priority hierarchy** for cache locality optimization
- **Affinity-aware** — filters by task's `cpus_ptr` before search
- **Load balancer acceleration** — O(1) idle lookup in `sched_balance_find_dst_group_cpu` and `update_sg_lb_stats`
- **Prefetch-optimized** — strategic cacheline prefetching across all hot paths
- **Zero-overhead when disabled** via static keys
- **Supports up to 64 CPUs per LLC** (single 64-bit word)

## Features

- **Fast idle CPU search** — Bitmap-based O(1) lookup replaces O(n) linear scan
- **SMT contention avoidance** — Strict preference for idle physical cores over SMT siblings
- **Cache hierarchy awareness** — L1 → L2 → L3 locality optimization
- **Load balancer fast paths** — Bitmap-based idle CPU counting and lookup for periodic load balancing

## How It Works

POC Selector maintains **per-LLC `atomic64_t` bitmasks** that track which CPUs (and which physical cores) are idle.
When the scheduler needs an idle CPU for task wakeup, it consults these bitmasks instead of scanning every CPU in the domain.

When the fast path cannot handle the request (LLC > 64 CPUs, asymmetric CPU capacity, etc.), the standard `select_idle_cpu()` takes over transparently.

### Key Properties

- **Lock-free** — each CPU only modifies its own bit; no spinlocks needed
- **SMT-aware** — prefers idle physical cores over idle SMT siblings
- **Affinity-aware** — intersects idle mask with task's `cpus_ptr` before any search
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

### 7-Level Priority Hierarchy

POC implements a strict 7-level priority hierarchy optimized for cache locality:

```
Phase 1: Early Return
  Level 0: Saturation check     — No idle CPUs → return -1 (fallback to CFS)
  Level 1: Target sticky        — Target CPU itself is idle (best L1/L2/L3 locality)

Phase 2: Core Search (SMT systems only, no contention)
  Level 2: L2 cluster idle core — Idle core within L2 cluster
  Level 3: LLC-wide idle core   — Idle core anywhere in LLC (round-robin)

Phase 3: CPU Search (all cores busy, SMT fallback)
  Level 4: Target SMT sibling   — Idle sibling of target (L1+L2 shared)
  Level 5: L2 cluster SMT       — Any idle CPU within L2 cluster
  Level 6: LLC-wide CPU         — Any idle CPU via round-robin
```

On non-SMT systems, Phase 3 is skipped entirely and Phase 2 operates on logical CPUs instead of physical cores.

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

## Accelerated Code Paths

POC accelerates three distinct scheduler paths:

### 1. Task Wakeup (`select_idle_sibling`)

The hottest path — called on every task wakeup. Replaces both `select_idle_smt()` and `select_idle_cpu()` with a single bitmap-based O(1) lookup.

### 2. Load Balancer Group CPU (`sched_balance_find_dst_group_cpu`)

Replaces the `for_each_cpu_and` loop that searches for an idle CPU within a scheduling group. O(1) lookup via `atomic64_read` + bitwise AND + CTZ.

### 3. Load Balancer Stats (`update_sg_lb_stats`)

Replaces per-CPU `idle_cpu()` calls with:
- **POPCNT** for O(1) idle CPU counting
- **Bitmap test** for O(1) per-CPU idle check in the stats loop

---

## Prefetch Strategy

POC uses strategic prefetching to hide cache miss latency on the `poc_idle_cpus` cacheline, which is `____cacheline_aligned` and separated from the fields read during eligibility checks:

| Site | Target | Hiding Window | Latency Reduced |
|------|--------|---------------|-----------------|
| `select_idle_sibling` (fair.c) | `poc_idle_cpus` | eligible check + function call + cpumask conversion | ~25-35 cyc |
| `select_idle_cpu_poc` (Level 1 miss) | `poc_cluster_mask[tgt_bit]`, `poc_smt_siblings[tgt_bit]` | seed computation + `poc_idle_cores` read | ~11-14 cyc |
| `poc_find_idle_cpu_in_group` | `poc_idle_cpus` | `cpumask_subset` scan | ~34-50 cyc |
| `poc_lb_prepare_idle_check` | `poc_idle_cpus` | `cpumask_subset` scan | ~34-50 cyc |

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
| `sched_poc_aligned` | true | Fast cpumask conversion (disabled if any LLC base is non-64-aligned) |
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
atomic64_t poc_idle_cpus ____cacheline_aligned;  // Logical CPUs
atomic64_t poc_idle_cores;                        // Physical cores (SMT only)
```

**Update operations**:
- `atomic64_or()`: Set bit (CPU goes idle)
- `atomic64_andnot()`: Clear bit (CPU goes busy)
- Each CPU only modifies its own bit → no locking required

**Memory barriers**:
- `smp_mb__after_atomic()`: On x86, compiles to compiler barrier only (0 cycles)
- On ARM64: emits `dmb ish`

**Cacheline isolation**: `____cacheline_aligned` ensures LOCK-prefixed writes to these bitmaps on idle transitions do not invalidate the cacheline containing `nr_busy_cpus` / `has_idle_cores` / `nr_idle_scan`.

---

### Pre-computed Masks

| Mask | Purpose | Lookup Complexity |
|------|---------|-------------------|
| `poc_smt_siblings[bit]` | SMT sibling mask per CPU (excl. self) | O(1) |
| `poc_cluster_mask[bit]` | L2 cluster mask per CPU (excl. self) | O(1) |

- Computed at boot time in topology.c
- Avoids runtime cpumask iteration
- Read-only after initialization → stable in L2/L3 cache

---

### Affinity-Aware Filtering

```c
static __always_inline u64 poc_cpumask_to_u64(const struct cpumask *mask,
                                              struct sched_domain_shared *sd_share)
```

Converts a full cpumask to a POC-relative u64 for bitwise intersection with idle bitmaps. Two paths:

- **Aligned** (common): Single word load when LLC base is 64-aligned
- **Unaligned** (e.g. Threadripper CCDs): Two-word load + shift

The `sched_poc_aligned` static key eliminates the branch at runtime.

---

## Requirements / Limitations

- **Kernel**: Linux kernel built with `CONFIG_SCHED_POC_SELECTOR=y` (default)
- **SMP**: Requires `CONFIG_SMP` (multi-processor kernel)
- **Max 64 logical CPUs per LLC**: The bitmask is backed by a single `atomic64_t` word covering up to 64 CPUs per Last-Level Cache domain
- **Symmetric CPU capacity only**: Disabled on big.LITTLE / hybrid architectures (`sched_asym_cpucap_active`)
- **Graceful fallback**: When the LLC contains more than 64 CPUs, the system has asymmetric CPU capacity, or no idle CPUs exist in the LLC, the selector transparently falls back to the standard `select_idle_cpu()` — no error, no performance penalty beyond losing the fast path
- **Runtime toggle**: Can be disabled at runtime via `sysctl kernel.sched_poc_selector=0`

---

## Configuration

### POC Parameters (sysctl)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `kernel.sched_poc_selector` | 1 | Enable/disable POC selector |
| `kernel.sched_poc_l2_cluster_search` | 1 | Enable/disable L2 cluster search |

---

## Sysfs Interface

### Status (always available with `CONFIG_SYSFS`)

```
/sys/kernel/poc_selector/status/
├── active              # 1 if POC is fully active (enabled + symmetric + eligible)
├── symmetric_cpucap    # 1 if CPU capacity is symmetric (not big.LITTLE)
├── all_llc_eligible    # 1 if all LLCs have ≤64 CPUs
└── version             # POC Selector version string
```

### Hardware Acceleration Info

```
/sys/kernel/poc_selector/hw_accel/
├── ctz               # CTZ implementation in use (e.g. "HW (TZCNT)")
├── ptselect          # PTSelect implementation in use (e.g. "HW (PDEP)")
└── popcnt            # POPCNT implementation in use (e.g. "HW (POPCNT)")
```

### Debug Counters (requires `CONFIG_SCHED_POC_SELECTOR_DEBUG=y`)

```
/sys/kernel/poc_selector/counters/
├── hit               # Total POC successes
├── fallthrough       # POC failures (fell through to CFS)
├── sticky            # Level 1 hits (target was idle)
├── l2_hit            # Level 2 hits (L2 cluster idle core)
├── llc_hit           # Level 3 hits (LLC-wide idle core)
├── smt_tgt           # Level 4 hits (target SMT sibling) [SMT only]
├── l2_smt            # Level 5 hits (L2 cluster SMT) [SMT only]
├── reset             # Write to reset all counters (root only)
└── cpu/
    └── cpu{N}        # Per-CPU selection counts
```

**Derived metric**: SMT search count (Level 4+5+6) = hit - sticky - l2_hit - llc_hit

---

## Patch

Apply the patch to a Linux source tree:

```bash
cd /path/to/linux-6.18.3
git apply /path/to/poc-selector/patches/0001-6.18.3-poc-selector-v1.9.0-beta1.patch
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
