mod bench;
mod calibrate;
mod stats;
mod system;
mod ui;

use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use clap::Parser;
use crossterm::event::{self, Event, KeyCode, KeyEventKind, KeyModifiers};
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use crossterm::ExecutableCommand;
use ratatui::backend::CrosstermBackend;
use ratatui::Terminal;

use crate::stats::{Histogram, StatResult};
use crate::system::{BenchParams, SystemInfo};
use crate::ui::{App, Phase};

const DEFAULT_ROUNDS: usize = 4;

// ---------------------------------------------------------------------------
// Global quit flag — set by SIGINT handler or key events
// ---------------------------------------------------------------------------

static QUIT: AtomicBool = AtomicBool::new(false);

extern "C" fn handle_sigint(_: libc::c_int) {
    QUIT.store(true, Ordering::Relaxed);
}

fn quitting() -> bool {
    QUIT.load(Ordering::Relaxed)
}

fn is_quit_event(ev: &Event) -> bool {
    match ev {
        Event::Key(key) if key.kind == KeyEventKind::Press => {
            key.code == KeyCode::Char('q')
                || (key.code == KeyCode::Char('c') && key.modifiers.contains(KeyModifiers::CONTROL))
        }
        _ => false,
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

#[derive(Parser)]
#[command(name = "poc-bench", about = "POC Selector Benchmark with TUI")]
struct Cli {
    /// Override iteration count (0 = auto-calibrate)
    #[arg(short, long, default_value_t = 0)]
    iterations: usize,

    /// Override worker thread count
    #[arg(short = 't', long)]
    threads: Option<usize>,

    /// Override background thread count
    #[arg(short, long)]
    background: Option<usize>,

    /// Number of comparison rounds
    #[arg(short, long, default_value_t = DEFAULT_ROUNDS)]
    rounds: usize,

    /// Skip POC ON/OFF comparison
    #[arg(long)]
    no_compare: bool,
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

fn main() {
    let cli = Cli::parse();
    let sysinfo = SystemInfo::detect();
    let params = BenchParams::with_overrides(
        sysinfo.ncpus,
        sysinfo.physical_cores,
        cli.threads,
        cli.background,
    );

    // Lock memory
    unsafe {
        libc::mlockall(libc::MCL_CURRENT | libc::MCL_FUTURE);
    }

    // Prevent deep C-states for accurate latency measurement.
    // Writing 0 to /dev/cpu_dma_latency keeps all CPUs in C0 while the fd is open.
    let dma_latency_fd = unsafe {
        let fd = libc::open(
            b"/dev/cpu_dma_latency\0".as_ptr() as *const libc::c_char,
            libc::O_WRONLY,
        );
        if fd >= 0 {
            let val: i32 = 0;
            libc::write(fd, &val as *const i32 as *const libc::c_void, 4);
        }
        fd
    };

    // Install SIGINT handler (Ctrl+C before raw mode / during calibration)
    unsafe {
        libc::signal(
            libc::SIGINT,
            handle_sigint as *const () as libc::sighandler_t,
        );
    }

    // Pre-check sysctl: readable AND writable?
    let sysctl_readable = system::poc_sysctl_read().is_some();
    let (sysctl_writable, sysctl_err) = if sysctl_readable {
        let val = system::poc_sysctl_read().unwrap_or(1);
        match system::poc_sysctl_write(val) {
            Ok(()) => (true, None),
            Err(e) => (false, Some(e)),
        }
    } else {
        (false, None)
    };
    let compare = !cli.no_compare && sysctl_writable;
    let orig_poc = if sysctl_readable {
        system::poc_sysctl_read().unwrap_or(1)
    } else {
        -1
    };

    // Set up terminal
    enable_raw_mode().expect("failed to enable raw mode");
    io::stdout()
        .execute(EnterAlternateScreen)
        .expect("failed to enter alternate screen");
    let backend = CrosstermBackend::new(io::stdout());
    let mut terminal = Terminal::new(backend).expect("failed to create terminal");

    let mut app = App::new(sysinfo, params.clone());
    terminal.draw(|f| ui::draw(f, &app)).ok();

    // --- Phase 1: Calibration ---
    let (iterations, warmup) = if cli.iterations > 0 {
        app.calibration = None;
        let warmup = (cli.iterations / 5).max(100);
        (cli.iterations, warmup)
    } else {
        app.phase = Phase::Calibrating;
        app.progress = 0.0;
        terminal.draw(|f| ui::draw(f, &app)).ok();

        let cal = calibrate::calibrate(&params);
        app.calibration = Some(cal.clone());
        app.progress = 1.0;
        terminal.draw(|f| ui::draw(f, &app)).ok();

        (cal.iterations, cal.warmup)
    };

    // --- Phase 2: Benchmark ---
    if !quitting() {
        if compare {
            run_comparison(
                &mut terminal,
                &mut app,
                &params,
                iterations,
                warmup,
                orig_poc,
                cli.rounds,
            );
        } else {
            // Single run, no comparison
            if !sysctl_writable && sysctl_readable {
                let msg = match &sysctl_err {
                    Some(e) => format!("sysctl: {}", e),
                    None => "sysctl not writable (need root?)".into(),
                };
                app.phase = Phase::Error(msg);
                terminal.draw(|f| ui::draw(f, &app)).ok();
                std::thread::sleep(Duration::from_secs(3));
            }
            if !quitting() {
                app.phase = Phase::Running {
                    round: 1,
                    total_rounds: 1,
                    poc_on: sysctl_readable && orig_poc > 0,
                };
                let handle = bench::bench_burst_async(&params, iterations, warmup);
                let samples = run_with_progress(&mut terminal, &mut app, &handle);

                if !samples.is_empty() {
                    let mut s = samples.clone();
                    let sr = StatResult::compute(&mut s);
                    app.hist_on = Some(Histogram::from_samples(&samples));
                    app.final_on = Some(sr);
                }
            }
        }
    }

    // --- Phase 3: Wait for quit (only if benchmark ran to completion) ---
    let show_summary = !quitting();
    if !quitting() {
        app.phase = Phase::Done;
        app.finished = true;
        app.progress = 1.0;
        terminal.draw(|f| ui::draw(f, &app)).ok();

        loop {
            if quitting() {
                break;
            }
            if event::poll(Duration::from_millis(100)).unwrap_or(false) {
                if let Ok(ev) = event::read() {
                    if is_quit_event(&ev) {
                        break;
                    }
                }
            }
        }
    }

    // --- Cleanup (always runs) ---
    if dma_latency_fd >= 0 {
        unsafe {
            libc::close(dma_latency_fd);
        }
    }
    if sysctl_writable && orig_poc >= 0 {
        system::poc_sysctl_write(orig_poc).ok();
    }
    disable_raw_mode().ok();
    io::stdout().execute(LeaveAlternateScreen).ok();
    terminal.show_cursor().ok();
    if show_summary {
        ui::print_summary(&app);
    }
}

fn run_comparison(
    terminal: &mut Terminal<CrosstermBackend<io::Stdout>>,
    app: &mut App,
    params: &BenchParams,
    iterations: usize,
    warmup: usize,
    orig_poc: i32,
    rounds: usize,
) {
    // --- Discard round ---
    app.phase = Phase::Discard;
    app.progress = 0.0;
    terminal.draw(|f| ui::draw(f, app)).ok();

    let discard_n = (iterations / 5).max(500);
    let discard_w = (warmup / 5).max(100);

    system::poc_sysctl_write(1).ok();
    let h = bench::bench_burst_async(params, discard_n, discard_w);
    let _ = run_with_progress(terminal, app, &h);
    if quitting() {
        return;
    }

    system::poc_sysctl_write(0).ok();
    app.progress = 0.5;
    terminal.draw(|f| ui::draw(f, app)).ok();
    let h = bench::bench_burst_async(params, discard_n, discard_w);
    let _ = run_with_progress(terminal, app, &h);
    if quitting() {
        return;
    }

    // --- Measured rounds ---
    let mut results_on = Vec::new();
    let mut results_off = Vec::new();
    let mut all_on = Vec::new();
    let mut all_off = Vec::new();

    'rounds: for round in 0..rounds {
        let on_first = round % 2 == 0;
        let order: [(bool, &str); 2] = if on_first {
            [(true, "POC ON"), (false, "CFS")]
        } else {
            [(false, "CFS"), (true, "POC ON")]
        };

        for &(poc_on, _label) in &order {
            if quitting() {
                break 'rounds;
            }

            app.phase = Phase::Running {
                round: round + 1,
                total_rounds: rounds,
                poc_on,
            };
            app.progress = 0.0;
            terminal.draw(|f| ui::draw(f, app)).ok();

            system::poc_sysctl_write(if poc_on { 1 } else { 0 }).ok();
            let h = bench::bench_burst_async(params, iterations, warmup);
            let samples = run_with_progress(terminal, app, &h);

            if quitting() {
                break 'rounds;
            }

            if !samples.is_empty() {
                let mut s = samples.clone();
                let sr = StatResult::compute(&mut s);
                if poc_on {
                    all_on.extend_from_slice(&samples);
                    results_on.push(sr);
                } else {
                    all_off.extend_from_slice(&samples);
                    results_off.push(sr);
                }
            }

            // Update histograms with cumulative data
            if !all_on.is_empty() {
                app.hist_on = Some(Histogram::from_samples(&all_on));
            }
            if !all_off.is_empty() {
                app.hist_off = Some(Histogram::from_samples(&all_off));
            }
            if !results_on.is_empty() {
                app.final_on = Some(StatResult::merge(&results_on));
            }
            if !results_off.is_empty() {
                app.final_off = Some(StatResult::merge(&results_off));
            }

            terminal.draw(|f| ui::draw(f, app)).ok();
        }
    }

    // Restore original POC setting
    system::poc_sysctl_write(orig_poc).ok();
}

fn run_with_progress(
    terminal: &mut Terminal<CrosstermBackend<io::Stdout>>,
    app: &mut App,
    handle: &bench::BenchHandle,
) -> Vec<u64> {
    loop {
        if quitting() {
            return Vec::new();
        }

        let p = handle.progress.load(Ordering::Relaxed);
        app.progress = if handle.total > 0 {
            p as f64 / handle.total as f64
        } else {
            0.0
        };
        terminal.draw(|f| ui::draw(f, app)).ok();

        if let Some(result) = handle.try_recv() {
            app.progress = 1.0;
            return result;
        }

        if event::poll(Duration::from_millis(50)).unwrap_or(false) {
            if let Ok(ev) = event::read() {
                if is_quit_event(&ev) {
                    QUIT.store(true, Ordering::Relaxed);
                    return Vec::new();
                }
            }
        }
    }
}

impl Clone for calibrate::CalibrationResult {
    fn clone(&self) -> Self {
        Self {
            iterations: self.iterations,
            warmup: self.warmup,
            probe_mean_us: self.probe_mean_us,
            probe_stddev_us: self.probe_stddev_us,
        }
    }
}
