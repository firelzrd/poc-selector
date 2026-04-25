# Piece-Of-Cake (POC) CPU Selector

A kernel patch that accelerates idle CPU discovery using per-LLC
cached bitmasks for O(1) idle CPU lookup. Supports two writer modes:
LOCK-prefixed atomic64 bitmaps (default) and lock-free u8 flag arrays
with multiply-and-shift readers.

<div align="center"><img width="256" height="224" alt="poc" src="https://github.com/user-attachments/assets/c9452565-3498-430b-9d87-706662956968" /></div>

## Inspiration

This project was born from the ideas pioneered by
[RitzDaCat](https://github.com/RitzDaCat) in
[scx_cake](https://github.com/RitzDaCat/scx_cake) — a sched_ext BPF scheduler of extraordinary originality and ambition.  
Where most scheduler projects (including mine) iterate on well-known designs, scx_cake charted its own course: a from-scratch architecture that boldly rethinks how scheduling decisions should be made.  
The creative vision and technical depth behind scx_cake are truly remarkable, and studying it was a catalyst for exploring what a similar bitmask-driven approach could look like inside the mainline CFS code path.

POC Selector distills one specific insight from scx_cake — fast idle-CPU selection via cached bitmasks — and transplants it into the kernel's `select_idle_cpu()` hot path as a lightweight, non-invasive patch.

## Key Characteristics

- **O(1) idle CPU discovery** via per-LLC bitmaps — single `atomic64_read` (MOV on x86) in the default bitmap mode, or stack-snapshotted u8[64] aggregated via PEXT/multiply-and-shift in the lock-free mode
- **12-level priority hierarchy** with sub-levels for cache locality optimization (L1s/L1t/L1p/L1r → L2 → L3 → L4s/L4p/L4t/L4r → L5 → L6)
- **Packed priority search** (LLC ≤ 32 CPUs): cluster + LLC-wide candidates packed in a single u64, resolved by one TZCNT
- **Three-tier SMT topology detection** — consecutive 2-way / uniform stride-N 2-way / exotic — most layouts derive idle-core mask at read time without any write-path overhead
- **Affinity-aware** — filters by task's `cpus_ptr` before search
- **RT saturation avoidance** — when saturated, avoids enqueuing behind RT tasks on target CPU
- **Eager commit** — selected CPU's bit is cleared from the bitmap at selection time, closing the race window for concurrent burst wakeups
- **sched_ext aware** — bitmap maintenance is automatically suppressed while an scx scheduler is active and resynced on hand-back
- **Prefetch-optimized** — conditional cacheline prefetching with "fire early, use late" pipeline
- **Zero-overhead when disabled** via static keys
- **Supports up to 64 CPUs per LLC** (single 64-bit word)

## Features

- **Fast idle CPU search** — Bitmap-based O(1) lookup replaces O(n) linear scan
- **Static key binary dispatch** — Runtime paths are patched to NOP/JMP at boot for zero dispatch overhead
- **BMI2 PEXT/PDEP acceleration** — Single-instruction flag extraction (lock-free mode) and PTSELECT on supported hardware (Intel, AMD Zen 3+); excluded on Zen 1/2 due to slow microcode
- **SMT contention avoidance** — Strict preference for idle physical cores over SMT siblings
- **Cache hierarchy awareness** — L1 → L2 → L3 locality optimization
- **Improved round-robin** — Case-split (1 / 2 / ≥3) with golden-ratio scrambling and Lemire fastrange for uniform distribution

## How It Works

POC Selector maintains **per-LLC idle state** that tracks which CPUs (and which physical cores) are idle.
Two single-write storage modes are selectable at runtime:

- **Bitmap mode** (default): `atomic64_t` bitmaps. Writers use `atomic64_or` / `atomic64_andnot` (LOCK'd on x86), readers use `atomic64_read` (plain MOV on x86) for O(1) snapshot access.
- **Lock-free mode**: `u8[64]` flag arrays sized to exactly one cache line per array. Writers use plain `WRITE_ONCE` (no LOCK prefix); readers `memcpy` the cache line to the stack, then aggregate via PEXT (BMI2) or multiply-and-shift into a u64 bitmask.

Only one mode is active at a time. Switching via sysctl resyncs the newly-active representation before any reader can observe it.

When the scheduler needs an idle CPU for task wakeup, it consults this bitmap state instead of scanning every CPU in the domain.

When the fast path cannot handle the request (LLC > 64 CPUs, asymmetric CPU capacity, scx active, etc.), the standard `select_idle_cpu()` takes over transparently.

### Key Properties

- **Lock-free reads** — `atomic64_read` (bitmap mode) or stack-snapshot + PEXT/multiply-and-shift (lock-free mode)
- **SMT-aware** — prefers idle physical cores over idle SMT siblings; on uniform 2-way SMT, the idle-core mask is derived at read time from the idle-CPU mask via bit-parallel ops (no separate write-path)
- **Affinity-aware** — intersects idle mask with task's `cpus_ptr` before any search
- **RT-aware** — on saturation, avoids placing CFS tasks behind RT tasks on the target CPU
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

### Selection Levels

POC implements a fine-grained priority hierarchy. The exact set of levels evaluated depends on whether the LLC has any fully-idle core (`core_mask`) at the time of the snapshot:

```
Phase 1: Saturation
  Level 0  : No idle CPUs in bitmap → return -1 (CFS fallback)

Phase 2: Pre-search shortcuts (always evaluated when applicable)
  Level 1r : Recent's core fully idle → return recent
             (skipped when sched_poc_early_select=1, which moves this
              check into select_idle_sibling before POC entry)
  Level 1s : Target CPU idle in bitmap → return target
             (sched_poc_target_sticky=1 only; L1/TLB affinity shortcut)

Phase 3a: Idle-core path (core_mask != 0)
  Level 1t : Target's core fully idle → return target
             (skipped when sched_poc_early_select=1)
  Level 1p : Prev's core fully idle → return prev
  Level 2  : Idle core within target's L2 cluster
  Level 3  : Idle core anywhere in LLC (round-robin)

Phase 3b: No-idle-core path (core_mask == 0)
  Level 4s : sync wakeup + target CPU idle → return target
             (waker yields, freeing the core)
  Level 4p : Prev's SMT sibling idle (cache locality)
  Level 4t : Target's SMT sibling idle
  Level 4r : Recent's SMT sibling idle (warm cache)
  [SIS_UTIL gate: nr_idle_scan == 0 → return -2 unless greedy_search=1]
  Level 5  : Idle CPU within target's L2 cluster
  Level 6  : Any idle CPU in LLC (round-robin)
```

On non-SMT systems, Levels 1r/1t/1p directly check the idle-CPU bitmap, then Levels 2/3 search the same bitmap. The 4s/4p/4t/4r/5/6 levels are SMT-only.

When `sched_poc_packed=1` (LLC ≤ 32 CPUs), Levels 2 + 3 (or 5 + 6) are merged into a single packed search: cluster candidates in the lower 32 bits, full-LLC candidates in the upper 32 bits, both rotated by a counter-derived amount, resolved by one TZCNT. Level discrimination is `raw >> 5`.

### Return Codes

| Value | Meaning | Caller behavior |
|-------|---------|-----------------|
| ≥ 0 | Selected CPU | Use directly |
| `-1` | Saturation: no idle CPU in POC bitmap | CFS may still find sched_idle CPUs — fall through to standard search |
| `-2` | SIS_UTIL overload (no-idle-core path, `greedy_search=0`) | Skip `select_idle_smt` / `select_idle_cpu` — POC has already exhausted the worthwhile candidates |

### RT Saturation Avoidance

When all standard paths return without finding an idle CPU, the scheduler checks whether the target CPU is currently running an RT task. If so, and `prev` is not running an RT task, it returns `prev` instead of `target` to avoid enqueuing a CFS task behind a higher-priority task that may not yield.

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

The hottest path — called on every task wakeup. POC runs after the
recent-CPU shortcut and before the standard `select_idle_smt()` /
`select_idle_cpu()` chain, replacing both with a single bitmap-based
O(1) lookup.

When `sched_poc_early_select=1` (default), select_idle_sibling
performs idle-core checks for `recent_used_cpu` and `target` *before*
entering POC search — both checks must be toggled together to
preserve POC's internal Level 1r → 1t priority order.

After POC returns, an additional last-resort RT-avoidance check
returns `prev` if `target` is running an RT task while `prev` is not.

### sched_ext Coordination

POC integrates with `CONFIG_SCHED_CLASS_EXT` to avoid wasted bitmap
maintenance when an scx scheduler owns task placement:

- `poc_notify_scx(true)` on scx enable → `poc_selector_skip = true` → static key disabled, `do_idle()` stops touching the bitmap
- `poc_notify_scx(false)` on scx disable → static key re-enabled, bitmaps resynced from `idle_cpu(cpu)` for every online CPU
- If an scx scheduler still calls `select_idle_sibling` (partial mode, or schedulers that delegate placement back to CFS), the hot path detector `poc_check_skip_fallback()` flips `poc_selector_skip` back to false and queues a workqueue item. The current call falls through to standard CFS (counted as `fallback`); the workqueue then re-enables `poc_selector_active` and runs `poc_resync_idle_state()`, which walks every online CPU and pushes its current `idle_cpu(cpu)` state into the bitmap. Subsequent wakeups use POC against consistent data. `WRITE_ONCE` and `schedule_work()` are idempotent, so concurrent first-callers safely collapse into a single workqueue dispatch.

---

## Prefetch Strategy

POC issues prefetches when it becomes clear that a variable will be needed,
as long as there is enough intervening work for the prefetch to take effect.
Prefetches are skipped when the data would be consumed in the very next instruction.

| # | Target | Placement | Cover (work between prefetch and load) |
|---|--------|-----------|----------------------------------------|
| 1 | `poc_idle_cpus_mask` / `poc_idle_cpus[]` | Function entry | `poc_cpumask_to_u64()` computation |
| 2 | `poc_idle_cores_mask` / `poc_idle_cores[]` (SMT, exotic only) | After mode dispatch | `poc_idle_cpu_mask()` + saturation check |
| 3 | `poc_smt_mask[rct/tgt/prv_bit]` (SMT, exotic only) | After mode dispatch | `poc_idle_cpu_mask()` + saturation check |
| 4 | `poc_cluster_mask[tgt_bit]` | Start of cluster/RR block | Seed computation (per-CPU RMW + MUL) |

Prefetches #2 and #3 are skipped on uniform 2-way SMT (Tier 1 & 2):
the idle-core mask is derived at read time from `cpu_mask` via
bit-parallel operations, so neither `poc_idle_cores_mask` nor the
per-CPU SMT sibling table is consulted.

---

## POC Optimization Techniques

### Idle State Storage — Two Modes

```c
/* Bitmap mode (default, sched_poc_lockless_bitmap=0) */
atomic64_t  poc_idle_cpus_mask  ____cacheline_aligned;  /* Logical CPUs */
atomic64_t  poc_idle_cores_mask ____cacheline_aligned;  /* Physical cores (exotic SMT only) */

/* Lock-free mode (sched_poc_lockless_bitmap=1) */
u8          poc_idle_cpus[64]   ____cacheline_aligned;  /* one cache line */
u8          poc_idle_cores[64]  ____cacheline_aligned;  /* exotic SMT only */
```

**Bitmap mode write path** (idle transitions):
- `atomic64_or(bit_mask, ...)` — set idle (LOCK OR on x86)
- `atomic64_andnot(bit_mask, ...)` — set busy (LOCK AND NOT on x86)
- `smp_mb__after_atomic()` for SMT core flag ordering — on x86 TSO: LOCK'd ops provide full fence so this is a compiler barrier (~0 cycles); on ARM64: `dmb ish`

**Bitmap mode read path** (idle state query):
- `atomic64_read(...)` — single MOV on x86-TSO, O(1)
- Masked by `poc_llc_members` to exclude non-existent CPUs

**Lock-free mode write path**:
- `WRITE_ONCE(poc_idle_cpus[bit], state)` — plain store, no LOCK prefix
- `smp_wmb()` before sibling reads on exotic SMT — `dmb ishst` on ARM64, compiler barrier on x86

**Lock-free mode read path** (`poc_flags_to_u64`):
- Phase 1: `memcpy(stack[8], flags, 64)` — snapshot the cache line
- Phase 2: pack stack copy into a u64 via 8 × `POC_BMP8` operations
  - BMI2 path: `pext` of `0x01010101...` — single instruction per 8 bytes
  - Fallback: `(w & 0x01...) * 0x01020408... >> 56` — multiply-and-shift trick

**Cacheline layout** — hot/cold separation:
- Hot write path (`poc_idle_cpus_*`, `poc_idle_cores_*`): each on its own aligned cache line
- Read-only lookup tables (`poc_cluster_mask`, `poc_smt_mask`): separate aligned cache lines, written once at init
- Prevents false sharing between write-hot bitmaps and read-only tables

### Eager Commit (poc_commit_selection)

When POC selects an idle CPU, it immediately clears that CPU's bit
from the idle bitmap (gated by `nr_running ≤ 2`). This closes the
race window where multiple waker CPUs read the same stale snapshot
and select the same idle CPU. The `poc_idle_committed` flag in `rq`
records the eager clear, so the matching `do_idle()` exit path skips
the redundant `atomic64_andnot` on the shared cacheline.

### Three-Tier SMT Topology Detection

| Tier | Topology | `poc_idle_core_mask()` derivation | Write-path cost |
|------|----------|------------------------------------|-----------------|
| 1 | Consecutive 2-way (siblings at 0,1 / 2,3 / ...) | `cpu_mask & (cpu_mask >> 1) & 0x5555...` (compile-time constants) | None |
| 2 | Uniform stride-N 2-way (e.g., Intel Xeon stride-8) | `cpu_mask & (cpu_mask >> shift) & primary_mask` (per-LLC `poc_smt_shift`, `poc_primary_mask`) | None |
| 3 | Exotic (>2-way SMT or non-uniform) | Reads separate `poc_idle_cores_mask` bitmap | Maintained on every idle transition |

Tier classification happens at boot in `topology.c`. Static keys
(`sched_poc_smt_consecutive`, `sched_poc_smt_uniform`) are disabled
on detection of a non-conforming LLC; the binary-patched fast path
disappears for that CPU at runtime.

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
| 1 | x86-64 + BMI2 (excl. Zen 1/2) | PDEP + TZCNT | O(1) |
| 2 | Fallback | Iterative bit-clear | O(j) |

**Reference**: Pandey, Bender, Johnson, "A Fast x86 Implementation of Select" (arXiv:1706.00990, 2017)

#### POC_BMP8 (Lock-free Mode Aggregation)

Pack one 8-byte slice of the flag array into 8 contiguous bits:

| Tier | Platform | Implementation |
|------|----------|----------------|
| 1 | x86-64 + BMI2 (excl. Zen 1/2) | PEXT with `0x0101010101010101` mask |
| 2 | Fallback | `(w & 0x01...) * 0x01020408... >> 56` |

#### POPCNT (Population Count)

- **x86-64**: Runtime detection via `boot_cpu_has(X86_FEATURE_POPCNT)`
- **ARM64**: CNT instruction
- **RISC-V Zbb**: cpop instruction
- **Fallback**: hweight64() software implementation

---

### Division-Free Mapping

```c
/* 32-bit fastrange: maps [0, 2^32) → [0, range) */
#define POC_FASTRANGE(seed, range) ((u32)(((u64)(seed) * (u32)(range)) >> 32))

/* 16-bit fixed-point modulo: maps phase ∈ [0, 2^16) → [0, range) */
#define POC_FIXED_MOD16(phase, range) ((u32)(((u32)(phase) * (u32)(range)) >> 16))
```

`POC_FASTRANGE` implements Lemire's fastrange algorithm.
`POC_FIXED_MOD16` is a 16-bit specialization combined with the
precomputed `poc_rr_step[]` reciprocal table — together they replace
modulo with two multiplications and a shift, valid for `range ≤ 64`.

**Reference**: Lemire, "Fast Random Integer Generation in an Interval" (ACM TOMACS, 2019)

---

### Static Keys (Zero-Cost Runtime Switching)

| Key | Default | Purpose |
|-----|---------|---------|
| `poc_selector_active` | true | Master gate (`sched_poc_selector && !poc_selector_skip`) |
| `sched_poc_smt_consecutive` | true | Tier 1 SMT detection (siblings at 0,1 / 2,3 / ...) |
| `sched_poc_smt_uniform` | true | Tier 2 SMT detection (uniform stride-N 2-way) |
| `sched_poc_smt_fallback` | false | Bail to CFS for SMT sibling selection (no-idle-core path) |
| `sched_poc_target_sticky` | false | Level 1s — return target CPU if idle, ignoring core idle state |
| `sched_poc_early_select` | true | Hoist Level 1r/1t idle-core checks into select_idle_sibling pre-POC |
| `sched_poc_greedy_search` | true | Always run Level 5/6 even under SIS_UTIL overload |
| `sched_poc_packed` | true | Packed priority search (LLC ≤ 32 CPUs) |
| `sched_poc_aligned` | true | Fast cpumask conversion (disabled if any LLC base is non-64-aligned) |
| `sched_poc_rr_improved` | true | Improved RR (case-split + golden-ratio + fastrange) vs poc_rr_step[] table |
| `sched_poc_lockless_bitmap` | false | Storage mode: u8[64] flag arrays vs atomic64_t bitmaps |
| `sched_poc_count_enabled` | false | Debug counter collection |
| `sched_cluster_active` | auto | Cluster topology detection |

- When disabled: Compiles to NOP (complete zero overhead)
- Dynamically patches kernel text at runtime

---

### Per-CPU Round-Robin Counter

```c
static DEFINE_PER_CPU(u32, poc_rr_counter);
#define POC_HASH_MULT 0x9E3779B9U  /* golden ratio * 2^32 */
#define POC_SCRAMBLE(counter) ((u32)(counter) * POC_HASH_MULT)
```

Initialization: `per_cpu(poc_rr_counter, cpu) = (u32)cpu` so different
CPUs start at different offsets, reducing cross-CPU collision
probability when multiple CPUs perform burst wakeups against the same
idle bitmap snapshot.

**Two RR strategies are A/B-selectable** via `sched_poc_rr_improved`:

| Mode | Algorithm |
|------|-----------|
| Improved (default) | Case-split: total=1 (direct CTZ), total=2 (cmov-selected mask + CTZ, guaranteed non-repeat), total≥3 (`POC_FASTRANGE(POC_SCRAMBLE(counter), total)`) |
| Legacy | `poc_rr_step[]` reciprocal table + `POC_FIXED_MOD16` (perfect RR) |

The packed priority search uses an analogous split: rotation amount
is `(POC_SCRAMBLE(counter) >> 27)` when improved is on, or
`(counter & 31)` when off. Eager commit prevents burst wake-ups from
re-selecting the same CPU regardless of RR strategy.

---

### Pre-computed Masks

| Mask | Purpose | Lookup Complexity |
|------|---------|-------------------|
| `poc_smt_mask[bit]` | SMT sibling mask per CPU (incl. self) — used only on exotic SMT (Tier 3); Tier 1/2 derive at read time | O(1) |
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
- **Max 64 logical CPUs per LLC**: The bitmap covers up to 64 CPUs per Last-Level Cache domain as a single word
- **Symmetric CPU capacity only**: Disabled on big.LITTLE / hybrid architectures (`sched_asym_cpucap_active`)
- **Suspended while sched_ext is active**: A running scx scheduler suppresses POC bitmap maintenance; POC is automatically resynced and re-enabled when scx is unloaded
- **Graceful fallback**: When the LLC contains more than 64 CPUs, the system has asymmetric CPU capacity, scx is active, or no idle CPUs exist in the LLC, the selector transparently falls back to the standard `select_idle_cpu()` — no error, no performance penalty beyond losing the fast path
- **Runtime toggle**: Can be disabled at runtime via `sysctl kernel.sched_poc_selector=0`

---

## Configuration

### POC Parameters (sysctl)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `kernel.sched_poc_selector` | 1 | Master enable/disable |
| `kernel.sched_poc_smt_fallback` | 0 | Bail to CFS for SMT sibling selection when no idle cores exist |
| `kernel.sched_poc_target_sticky` | 0 | Level 1s — return target if idle, regardless of core idle state |
| `kernel.sched_poc_early_select` | 1 | Hoist Level 1r/1t into `select_idle_sibling` pre-POC entry |
| `kernel.sched_poc_greedy_search` | 1 | Always run Level 5/6 even under SIS_UTIL overload |
| `kernel.sched_poc_rr_improved` | 1 | Improved RR (case-split + golden-ratio + fastrange) |
| `kernel.sched_poc_lockless_bitmap` | 0 | Storage mode: 1 = u8[64] flag arrays, 0 = atomic64_t bitmaps |
| `kernel.sched_poc_count` | 0 | Per-level hit counter collection |

Boot-time-only static keys (`sched_poc_smt_consecutive`,
`sched_poc_smt_uniform`, `sched_poc_packed`, `sched_poc_aligned`) are
configured automatically based on detected LLC topology and are not
exposed as sysctls.

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

### Hit Counters (enabled by `kernel.sched_poc_count=1`)

```
/sys/kernel/poc_selector/count/
├── l1s               # Level 1s hits (target sticky, L1/TLB affinity)
├── l1t               # Level 1t hits (target's core fully idle)
├── l1p               # Level 1p hits (prev's core fully idle)
├── l1r               # Level 1r hits (recent's core fully idle)
├── l2                # Level 2  hits (idle core in L2 cluster)
├── l3                # Level 3  hits (idle core across LLC, RR)
├── l4s               # Level 4s hits (sync + target CPU idle)
├── l4p               # Level 4p hits (prev's SMT sibling)
├── l4t               # Level 4t hits (target's SMT sibling)
├── l4r               # Level 4r hits (recent's SMT sibling)
├── l5                # Level 5  hits (idle CPU in L2 cluster)
├── l6                # Level 6  hits (idle CPU across LLC, RR)
├── fallback          # Fallback hits (POC returned -1, CFS took over)
└── reset             # Write 1 to reset all counters
```

---

## Patch

Apply the patch to a Linux source tree:

```bash
cd /path/to/linux-x.y.z
git apply /path/to/poc-selector/patches/stable/0001-x.y.z-poc-selector-vN.N.N.patch
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
