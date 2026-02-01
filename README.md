# Piece-Of-Cake (POC) CPU Selector

A kernel patch that accelerates idle CPU discovery using per-LLC atomic bitmasks,
achieving O(1) lookup instead of linear scanning.

## Inspiration

This project was born from the ideas pioneered by
[RitzDaCat](https://github.com/RitzDaCat) in
[scx_cake](https://github.com/RitzDaCat/scx_cake) — a sched_ext BPF scheduler of extraordinary originality and ambition.  
Where most scheduler projects (including mine) iterate on well-known designs, scx_cake charted its own course: a from-scratch architecture that boldly rethinks how scheduling decisions should be made.  
The creative vision and technical depth behind scx_cake are truly remarkable, and studying it was a catalyst for exploring what a similar bitmask-driven approach could look like inside the mainline CFS code path.

POC Selector distills one specific insight from scx_cake — fast idle-CPU selection via cached bitmasks — and transplants it into the kernel's `select_idle_cpu()` hot path as a lightweight, non-invasive patch.

## How It Works

POC Selector maintains **per-LLC `atomic64_t` bitmasks** that track which CPUs (and which physical cores) are idle.  
When the scheduler needs an idle CPU for task wakeup, it consults these bitmasks instead of scanning every CPU in the domain.

```
Fast Path (per LLC, <= 64 CPUs):

  Level 0 — Saturation check           ~5 cycles
    mask == 0  →  no idle CPUs, bail out immediately

  Level 1 — Target sticky              ~8 cycles
    target CPU still idle?  →  reuse it (preserves cache locality)

  Level 2 — TZCNT search               ~12 cycles
    __builtin_ctzll on idle-core mask  →  first idle physical core
    fallback: idle-cpu mask            →  first idle logical CPU
```

When the fast path cannot handle the request (LLC > 64 CPUs, restricted affinity, etc.), the standard `select_idle_cpu()` takes over transparently.

### Key properties

- **Lock-free** — each CPU only modifies its own bit; no spinlocks needed
- **SMT-aware** — prefers idle physical cores over idle SMT siblings
- **Zero fairness impact** — only changes *where* a task is placed, not scheduling order
- **Runtime toggle** — `sysctl kernel.sched_poc_selector` (0/1, default 1)

## Benchmark

Measured with the included `poc_bench` tool on a partially saturated system (half of CPUs running background load), which represents the scenario where linear scanning is most expensive.

Ryzen 7 7840HS (8C/16T):

|               | POC ON       | POC OFF      |           |
|---------------|-------------:|-------------:|-----------|
| **mean**      |   9,842.3 ns |  31,025.7 ns |           |
| **p50**       |     8,382 ns |    21,947 ns |           |
| **p99**       |    25,247 ns |   380,577 ns |           |
| **ops/sec**   |      101,602 |       32,231 | **+215%** |

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

## License

GPL-2.0 — see [LICENSE](LICENSE).

## Special Thanks

RitzDaCat - of course, for giving birth to scx_cake inspiring me of implementing the selector.
