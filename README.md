# Piece-Of-Cake (POC) CPU Selector

A kernel patch that accelerates idle CPU discovery using per-LLC shared flag arrays
with lock-free stores and O(1) reader aggregation, replacing linear scanning.

<div align="center"><img width="256" height="224" alt="poc" src="https://github.com/user-attachments/assets/c9452565-3498-430b-9d87-706662956968" /></div>

## Inspiration

This project was born from the ideas pioneered by
[RitzDaCat](https://github.com/RitzDaCat) in
[scx_cake](https://github.com/RitzDaCat/scx_cake) — a sched_ext BPF scheduler of extraordinary originality and ambition.  
Where most scheduler projects (including mine) iterate on well-known designs, scx_cake charted its own course: a from-scratch architecture that boldly rethinks how scheduling decisions should be made.  
The creative vision and technical depth behind scx_cake are truly remarkable, and studying it was a catalyst for exploring what a similar bitmask-driven approach could look like inside the mainline CFS code path.

POC Selector distills one specific insight from scx_cake — fast idle-CPU selection via cached bitmasks — and transplants it into the kernel's `select_idle_cpu()` hot path as a lightweight, non-invasive patch.

## Key Characteristics

- **O(1) idle CPU discovery** via shared `u8[64]` flag arrays per LLC with lock-free `WRITE_ONCE` stores and multiply-and-shift reader aggregation
- **7-level priority hierarchy** for cache locality optimization
- **Affinity-aware** — filters by task's `cpus_ptr` before search
- **SIS_UTIL-aware** — respects CFS's overload detection before LLC-wide search
- **Prefetch-optimized** — conditional cacheline prefetching with "fire early, use late" pipeline
- **Zero-overhead when disabled** via static keys
- **Supports up to 64 CPUs per LLC** (single 64-bit word after aggregation)

## Features

- **Fast idle CPU search** — Bitmap-based O(1) lookup replaces O(n) linear scan
- **Lock-free write path** — Plain `WRITE_ONCE` (MOV) with no LOCK prefix, no pipeline stall
- **Static key binary dispatch** — 3-bit encoded chunk count eliminates loop overhead in reader aggregation
- **BMI2 PEXT acceleration** — Single-instruction flag extraction on supported hardware (Intel, AMD Zen 3+)
- **SMT contention avoidance** — Strict preference for idle physical cores over SMT siblings
- **Cache hierarchy awareness** — L1 → L2 → L3 locality optimization

## How It Works

POC Selector maintains **per-LLC `u8[64]` flag arrays** that track which CPUs (and which physical cores) are idle.
Each array occupies exactly one cache line (64 bytes). Writers use plain `WRITE_ONCE` stores (no LOCK prefix),
and readers aggregate the flags into a u64 bitmask via a multiply-and-shift trick in O(1).

When the scheduler needs an idle CPU for task wakeup, it consults these bitmasks instead of scanning every CPU in the domain.

When the fast path cannot handle the request (LLC > 64 CPUs, asymmetric CPU capacity, etc.), the standard `select_idle_cpu()` takes over transparently.

### Key Properties

- **Lock-free** — writers use `WRITE_ONCE` (plain MOV); no LOCK prefix, no atomic RMW
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

**POC behavior**: Always executes idle core search first, falling back to SMT siblings only when all physical cores are busy.

### 7-Level Priority Hierarchy

POC implements a strict 7-level priority hierarchy optimized for cache locality:

```
Phase 1: Early Return
  Level 0: Saturation check       — No idle CPUs → return -1 (CFS fallback)

Phase 2: Sticky (prev affinity)
  Level 1: prev sticky            — prev is idle → return prev (cache-hot)
                                    (SMT: prev's core must also be idle)
  Level 4: prev/sibling sticky    — SMT only: all cores busy, prev or sibling idle → return

Phase 3: Core/CPU Search
  SIS_UTIL gate                   — Respect CFS overload detection before LLC-wide search
  Level 2: L2 cluster idle core   — Idle core within target's L2 cluster (round-robin)
  Level 3: LLC-wide idle core     — Idle core anywhere in LLC (round-robin)
  Level 5: L2 cluster CPU         — Any idle CPU within target's L2 cluster (round-robin)
  Level 6: LLC-wide CPU           — Any idle CPU via round-robin
```

On non-SMT systems, Level 1 checks the prev idle CPU directly. Levels 2-3, 4 are skipped.
On SMT systems, Level 1 and Level 4 are mutually exclusive: Level 1 runs when idle cores exist, Level 4 runs when all cores are busy (and also checks SMT sibling).
Levels 2-3 search the idle-core bitmap; levels 5-6 search the idle-CPU bitmap (fallback when no full cores are free).

### SIS_UTIL Gate

Before the LLC-wide search (Levels 2-3, 5-6), POC respects CFS's SIS_UTIL overload detection:

- **Gate 1**: `nr_idle_scan == 0` — LLC utilization exceeds ~85%, skip search
- **Gate 2**: `E[hits] = idle × nr / total < 1` — Models CFS's budget-limited scan. If the expected number of idle hits in CFS's scan budget is less than 1, POC also bails out

When either gate triggers, POC returns -1 to let CFS handle the fallback path.

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

## Accelerated Code Path

### Task Wakeup (`select_idle_sibling`)

The hottest path — called on every task wakeup. Replaces both `select_idle_smt()` and `select_idle_cpu()` with a single bitmap-based O(1) lookup.

---

## Prefetch Strategy

POC issues prefetches when it becomes clear that a variable will be needed,
as long as there is enough intervening work for the prefetch to take effect.
Prefetches are skipped when the data would be consumed in the very next instruction.

| # | Target | Placement | Cover (work between prefetch and load) | Waste |
|---|--------|-----------|----------------------------------------|-------|
| 1 | `poc_idle_cpus[64]` | Function entry | `poc_cpumask_to_u64()` computation | 0% — always needed |
| 2 | `poc_idle_cores[64]` (SMT) | After affinity computation | `poc_read_idle_cpus()` + saturation check | Low — wasted only when fully saturated |
| 3 | `poc_smt_mask[prev_bit]` (SMT) | After affinity computation | `poc_read_idle_cpus()` + saturation check | Low — wasted when prev not local or fully saturated |
| 4 | `poc_cluster_mask[target_bit]` | Start of cluster/RR block | Seed computation (per-CPU RMW + MUL) | 0% on non-cluster (static branch NOP) |

---

## POC Optimization Techniques

### Shared Flag Arrays and Reader Aggregation

```c
u8  poc_idle_cpus[64]  ____cacheline_aligned;  // Logical CPUs
u8  poc_idle_cores[64]  ____cacheline_aligned;  // Physical cores (SMT only)
```

**Write path** (idle transitions):
- `WRITE_ONCE(sd_share->poc_idle_cpus[bit], state ? 1 : 0)` — plain MOV, no LOCK prefix
- No pipeline stall, no store buffer drain
- `smp_wmb()` for SMT core flag ordering — on x86 TSO: compiler barrier only (0 cycles); on ARM64: `dmb ishst`

**Read path** (flag aggregation via `poc_flags_to_u64`):
- Converts `u8[64]` flag array to `u64` bitmask using multiply-and-shift trick (or PEXT on BMI2)
- Static key binary dispatch eliminates loop overhead — 3 static keys encode chunk count as 3-bit binary, dispatching to the exact number of operations needed

**Cacheline layout** — hot/cold separation:
- Hot write path (`poc_idle_cpus`, `poc_idle_cores`): each on its own aligned cache line
- Read-only lookup tables (`poc_cluster_mask`, `poc_smt_mask`): separate aligned cache lines, written once at init
- Prevents false sharing between write-hot flags and read-only tables

### Static Key Binary Dispatch (`poc_flags_to_u64`)

Three static keys (`poc_chunks_bit[2:0]`) encode `ceil(nr_cpus / 8) - 1` as a 3-bit value, set at boot.
Nested `static_branch_unlikely` calls form a binary search tree that dispatches to 1 of 8 fully-unrolled paths:

```
bit2=0, bit1=0, bit0=0 → 1 chunk  (≤8 CPUs)
bit2=0, bit1=0, bit0=1 → 2 chunks (9-16 CPUs)
bit2=0, bit1=1, bit0=0 → 3 chunks (17-24 CPUs)
  ...
bit2=1, bit1=1, bit0=1 → 8 chunks (57-64 CPUs)
```

Each static branch is patched to NOP or JMP at boot — zero runtime dispatch cost.
Only the exact number of multiply-and-shift (or PEXT) operations are executed.

### Bit Manipulation Primitives

#### POC_CHUNK (Flag-to-Bit Conversion)

Two-tier implementation for each 8-byte chunk of the flag array:

| Tier | Platform | Implementation | Per-Chunk Latency |
|------|----------|----------------|-------------------|
| 1 | x86-64 + BMI2 (excl. Zen 1/2) | PEXT + SHL | ~3 cycles |
| 2 | Fallback | AND + MUL + SHR + SHL | ~5 cycles |

**Note**: AMD Zen 1/2 excluded from PEXT path due to slow microcode implementation (~18 cycles).

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
| 1 | x86-64 + BMI2 (excl. Zen 1/2) | PDEP + TZCNT | O(1) |
| 2 | Fallback | Iterative bit-clear | O(j) |

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
| `poc_chunks_bit2` | false | Binary dispatch bit 2 (chunks > 4) |
| `poc_chunks_bit1` | false | Binary dispatch bit 1 |
| `poc_chunks_bit0` | false | Binary dispatch bit 0 |
| `sched_poc_count_enabled` | false | Debug counter collection |
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

### Pre-computed Masks

| Mask | Purpose | Lookup Complexity |
|------|---------|-------------------|
| `poc_smt_mask[bit]` | SMT sibling mask per CPU (excl. self) | O(1) |
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
- **Max 64 logical CPUs per LLC**: The flag array covers up to 64 CPUs per Last-Level Cache domain, aggregated into a single u64 via multiply-and-shift
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
| `kernel.sched_poc_count` | 0 | Enable/disable per-level hit counter collection |

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
├── chunk             # Chunk aggregation in use (e.g. "HW (PEXT)" or "SW (MUL)")
└── popcnt            # POPCNT implementation in use (e.g. "HW (POPCNT)")
```

### Hit Counters (enabled by `kernel.sched_poc_count=1`)

```
/sys/kernel/poc_selector/count/
├── l1                # Level 1 hits (prev sticky)
├── l2                # Level 2 hits (idle core in L2 cluster)
├── l3                # Level 3 hits (idle core across LLC)
├── l4                # Level 4 hits (prev/sibling sticky, SMT)
├── l5                # Level 5 hits (idle CPU in L2 cluster)
├── l6                # Level 6 hits (idle CPU across LLC)
├── fallback          # Fallback hits (POC returned -1, CFS took over)
└── reset             # Write 1 to reset all counters
```

---

## Patch

Apply the patch to a Linux source tree:

```bash
cd /path/to/linux-x.y.z
git apply /path/to/poc-selector/patches/0001-x.y.z-poc-selector.patch
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

## Special Thanks

RitzDaCat - of course, for giving birth to scx_cake inspiring me of implementing the selector.  
Mario Roy - for advising me about the PTSelect algorithm use, providing me lots of test suites and more.  
The CachyOS Community Members - who patiently contributed with many useful feedbacks.

## License

GPL-2.0 — see [LICENSE](LICENSE).
