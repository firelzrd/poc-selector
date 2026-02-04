use crate::system::BenchParams;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::sync::Arc;
use std::thread;

// ---------------------------------------------------------------------------
// Shadow thread context
// ---------------------------------------------------------------------------

struct ShadowCtx {
    target_cpu: AtomicI32, // -1 = idle
    ack: AtomicI32,        // 0 = request pending, 1 = done
    stop: AtomicBool,
}

impl ShadowCtx {
    fn new() -> Self {
        Self {
            target_cpu: AtomicI32::new(-1),
            ack: AtomicI32::new(1),
            stop: AtomicBool::new(false),
        }
    }
}

fn shadow_thread(ctx: &ShadowCtx) {
    let mut cur_cpu: i32 = -1;
    while !ctx.stop.load(Ordering::Relaxed) {
        if ctx.ack.load(Ordering::Acquire) == 0 {
            let target = ctx.target_cpu.load(Ordering::Acquire);
            if target >= 0 {
                if target != cur_cpu {
                    pin_self(target as usize);
                    cur_cpu = target;
                }
                ctx.ack.store(1, Ordering::Release);
            }
        }
        // Busy-poll with low overhead
        for _ in 0..1000u32 {
            core::hint::spin_loop();
        }
    }
}

// ---------------------------------------------------------------------------
// Worker thread context
// ---------------------------------------------------------------------------

struct WorkerCtx {
    efd: i32,
    warmup: usize,
    total: usize,
    shadows: Vec<Arc<ShadowCtx>>,
    sync_done: Arc<AtomicU32>,
    ts_wake: Vec<AtomicU64>,
    latencies: Vec<AtomicU64>,
}

// AtomicU64 wrapper (stable since 1.34)
use std::sync::atomic::AtomicU64;

fn worker_thread(ctx: &WorkerCtx) {
    let n_shadows = ctx.shadows.len();
    let mut sidx: usize = 0;

    // Initial shadow setup
    let cpu = sched_getcpu();
    ctx.shadows[0].ack.store(0, Ordering::Release);
    ctx.shadows[0]
        .target_cpu
        .store(cpu as i32, Ordering::Release);
    bounded_spin_wait(&ctx.shadows[0].ack);
    ctx.sync_done.fetch_add(1, Ordering::Release);

    let mut buf = [0u8; 8];
    for i in 0..ctx.total {
        // Block on eventfd
        let n = unsafe { libc::read(ctx.efd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
        if n != 8 {
            break;
        }

        let t1 = now_ns();
        let t0 = ctx.ts_wake[i].load(Ordering::Acquire);
        if i >= ctx.warmup {
            ctx.latencies[i - ctx.warmup].store(t1.wrapping_sub(t0), Ordering::Relaxed);
        }

        // Brief compute
        let mut x: u32 = 0;
        for j in 0..100u32 {
            x = x.wrapping_add(j);
        }
        std::hint::black_box(x);

        // Tell shadow to pin to our current CPU
        let cpu = sched_getcpu();
        ctx.shadows[sidx].ack.store(0, Ordering::Release);
        ctx.shadows[sidx]
            .target_cpu
            .store(cpu as i32, Ordering::Release);
        bounded_spin_wait(&ctx.shadows[sidx].ack);

        if n_shadows > 1 {
            sidx ^= 1;
        }
        ctx.sync_done.fetch_add(1, Ordering::Release);
    }
}

fn bounded_spin_wait(ack: &AtomicI32) {
    for _ in 0..2000u32 {
        if ack.load(Ordering::Acquire) != 0 {
            return;
        }
        core::hint::spin_loop();
    }
}

// ---------------------------------------------------------------------------
// Async benchmark handle
// ---------------------------------------------------------------------------

pub struct BenchHandle {
    pub progress: Arc<AtomicU32>,
    pub total: u32,
    rx: Receiver<Vec<u64>>,
}

impl BenchHandle {
    pub fn try_recv(&self) -> Option<Vec<u64>> {
        self.rx.try_recv().ok()
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

pub fn bench_burst_async(params: &BenchParams, iterations: usize, warmup: usize) -> BenchHandle {
    let progress = Arc::new(AtomicU32::new(0));
    let (tx, rx) = mpsc::channel();
    let total_iters = (warmup + iterations) as u32;

    let params = params.clone();
    let progress_clone = progress.clone();

    thread::spawn(move || {
        let result = bench_burst_inner(&params, iterations, warmup, &progress_clone);
        let _ = tx.send(result);
    });

    BenchHandle {
        progress,
        total: total_iters,
        rx,
    }
}

pub fn bench_burst_sync(params: &BenchParams, iterations: usize, warmup: usize) -> Vec<u64> {
    let progress = Arc::new(AtomicU32::new(0));
    bench_burst_inner(params, iterations, warmup, &progress)
}

fn bench_burst_inner(
    params: &BenchParams,
    iterations: usize,
    warmup: usize,
    progress: &AtomicU32,
) -> Vec<u64> {
    let ncpus = unsafe { libc::sysconf(libc::_SC_NPROCESSORS_ONLN) as usize };
    let total = warmup + iterations;
    let n_workers = params.n_workers;
    let n_background = params.n_background.min(ncpus - 1);
    let spw = params.shadows_per_worker;
    let total_shadows = n_workers * spw;

    // Save original affinity
    let orig_affinity = get_affinity();

    // --- 1. Create shadow contexts ---
    let shadow_ctxs: Vec<Arc<ShadowCtx>> = (0..total_shadows)
        .map(|_| Arc::new(ShadowCtx::new()))
        .collect();

    let shadow_handles: Vec<_> = shadow_ctxs
        .iter()
        .map(|ctx| {
            let ctx = Arc::clone(ctx);
            thread::spawn(move || shadow_thread(&ctx))
        })
        .collect();

    // --- 2. Create worker contexts ---
    let sync_done = Arc::new(AtomicU32::new(0));

    let mut worker_efds = Vec::with_capacity(n_workers);
    let mut worker_ctxs: Vec<Arc<WorkerCtx>> = Vec::with_capacity(n_workers);

    for w in 0..n_workers {
        let efd = unsafe { libc::eventfd(0, libc::EFD_SEMAPHORE) };
        assert!(efd >= 0, "eventfd failed");
        worker_efds.push(efd);

        let shadows: Vec<Arc<ShadowCtx>> = (0..spw)
            .map(|s| Arc::clone(&shadow_ctxs[w * spw + s]))
            .collect();

        let ts_wake: Vec<AtomicU64> = (0..total).map(|_| AtomicU64::new(0)).collect();
        let latencies: Vec<AtomicU64> = (0..iterations).map(|_| AtomicU64::new(0)).collect();

        worker_ctxs.push(Arc::new(WorkerCtx {
            efd,
            warmup,
            total,
            shadows,
            sync_done: Arc::clone(&sync_done),
            ts_wake,
            latencies,
        }));
    }

    let worker_handles: Vec<_> = worker_ctxs
        .iter()
        .map(|ctx| {
            let ctx = Arc::clone(ctx);
            thread::spawn(move || worker_thread(&ctx))
        })
        .collect();

    // --- 3. Background burn threads ---
    let bg_stop = Arc::new(AtomicBool::new(false));
    let bg_handles: Vec<_> = (0..n_background)
        .map(|i| {
            let stop = Arc::clone(&bg_stop);
            thread::spawn(move || {
                pin_self(i + 1); // skip CPU 0 (dispatcher)
                while !stop.load(Ordering::Relaxed) {
                    for _ in 0..10000u32 {
                        core::hint::spin_loop();
                    }
                }
            })
        })
        .collect();

    // --- 4. Pin dispatcher to CPU 0 with SCHED_FIFO ---
    pin_self(0);
    let orig_sched = set_fifo_self();
    thread::sleep(std::time::Duration::from_millis(50));

    // --- 5. Wait for initial shadow setup ---
    while sync_done.load(Ordering::Acquire) < n_workers as u32 {
        core::hint::spin_loop();
    }
    sync_done.store(0, Ordering::Release);
    thread::sleep(std::time::Duration::from_micros(200));

    // --- 6. Dispatch ---
    let wval: u64 = 1;
    for i in 0..total {
        if i > 0 {
            while sync_done.load(Ordering::Acquire) < n_workers as u32 {
                core::hint::spin_loop();
            }
            sync_done.store(0, Ordering::Release);

            // Let shadows settle + workers enter read()
            busy_wait_ns(10_000);
        }

        for w in 0..n_workers {
            let t0 = now_ns();
            worker_ctxs[w].ts_wake[i].store(t0, Ordering::Release);
            unsafe {
                libc::write(
                    worker_efds[w],
                    &wval as *const u64 as *const libc::c_void,
                    8,
                );
            }
        }

        progress.store(i as u32 + 1, Ordering::Relaxed);
    }

    // Join workers
    for h in worker_handles {
        h.join().ok();
    }

    // Stop background
    bg_stop.store(true, Ordering::Relaxed);
    for h in bg_handles {
        h.join().ok();
    }

    // Stop shadows
    for ctx in &shadow_ctxs {
        ctx.stop.store(true, Ordering::Relaxed);
    }
    for h in shadow_handles {
        h.join().ok();
    }

    // Collect latencies
    let mut all = Vec::with_capacity(iterations * n_workers);
    for w in 0..n_workers {
        for i in 0..iterations {
            all.push(worker_ctxs[w].latencies[i].load(Ordering::Relaxed));
        }
    }

    // Close eventfds
    for &efd in &worker_efds {
        unsafe {
            libc::close(efd);
        }
    }

    // Restore scheduler policy and affinity
    if let Some(sp) = orig_sched {
        restore_sched_self(&sp);
    }
    if let Some(mask) = orig_affinity {
        set_affinity_mask(&mask);
    }

    all
}

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

fn now_ns() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    unsafe {
        libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts);
    }
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

fn busy_wait_ns(ns: u64) {
    let deadline = now_ns() + ns;
    while now_ns() < deadline {
        core::hint::spin_loop();
    }
}

fn sched_getcpu() -> usize {
    unsafe { libc::sched_getcpu() as usize }
}

fn pin_self(cpu: usize) {
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        libc::CPU_ZERO(&mut set);
        libc::CPU_SET(cpu, &mut set);
        libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &set);
    }
}

fn get_affinity() -> Option<libc::cpu_set_t> {
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        if libc::sched_getaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &mut set) == 0 {
            Some(set)
        } else {
            None
        }
    }
}

struct SavedSchedPolicy {
    policy: libc::c_int,
    param: libc::sched_param,
}

fn set_fifo_self() -> Option<SavedSchedPolicy> {
    unsafe {
        let mut orig_param: libc::sched_param = std::mem::zeroed();
        let orig_policy = libc::sched_getscheduler(0);
        if orig_policy < 0 {
            return None;
        }
        libc::sched_getparam(0, &mut orig_param);

        let fifo_param = libc::sched_param { sched_priority: 1 };
        if libc::sched_setscheduler(0, libc::SCHED_FIFO, &fifo_param) == 0 {
            Some(SavedSchedPolicy {
                policy: orig_policy,
                param: orig_param,
            })
        } else {
            None
        }
    }
}

fn restore_sched_self(saved: &SavedSchedPolicy) {
    unsafe {
        libc::sched_setscheduler(0, saved.policy, &saved.param);
    }
}

fn set_affinity_mask(set: &libc::cpu_set_t) {
    unsafe {
        libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), set);
    }
}
