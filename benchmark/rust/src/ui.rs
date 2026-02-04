use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Gauge, Paragraph};
use ratatui::Frame;

use crate::calibrate::CalibrationResult;
use crate::stats::{Histogram, StatResult, BUCKET_LABELS, NUM_BUCKETS};
use crate::system::{BenchParams, SystemInfo};

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

#[derive(Clone)]
pub enum Phase {
    Calibrating,
    Discard,
    Running {
        round: usize,
        total_rounds: usize,
        poc_on: bool,
    },
    Error(String),
    Done,
}

pub struct App {
    pub system: SystemInfo,
    pub params: BenchParams,
    pub phase: Phase,
    pub progress: f64,
    pub calibration: Option<CalibrationResult>,
    pub hist_on: Option<Histogram>,
    pub hist_off: Option<Histogram>,
    pub final_on: Option<StatResult>,
    pub final_off: Option<StatResult>,
    pub finished: bool,
}

impl App {
    pub fn new(system: SystemInfo, params: BenchParams) -> Self {
        Self {
            system,
            params,
            phase: Phase::Calibrating,
            progress: 0.0,
            calibration: None,
            hist_on: None,
            hist_off: None,
            final_on: None,
            final_off: None,
            finished: false,
        }
    }
}

// ---------------------------------------------------------------------------
// Color constants
// ---------------------------------------------------------------------------

const COL_POC: Color = Color::Green;
const COL_CFS: Color = Color::Yellow;
const COL_BETTER: Color = Color::Green;
const COL_WORSE: Color = Color::Red;
const COL_DIM: Color = Color::DarkGray;
const COL_LABEL: Color = Color::Cyan;

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

pub fn draw(f: &mut Frame, app: &App) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(4), // header
            Constraint::Length(3), // progress
            Constraint::Min(12),   // histogram
            Constraint::Length(8), // summary
            Constraint::Length(1), // footer
        ])
        .split(f.area());

    draw_header(f, chunks[0], app);
    draw_progress(f, chunks[1], app);
    draw_histogram(f, chunks[2], app);
    draw_summary(f, chunks[3], app);
    draw_footer(f, chunks[4], app);
}

fn draw_header(f: &mut Frame, area: Rect, app: &App) {
    let hw = &app.system.hw_features;
    let lines = vec![
        Line::from(vec![
            Span::styled(
                &app.system.cpu_model,
                Style::default()
                    .fg(Color::White)
                    .add_modifier(Modifier::BOLD),
            ),
            Span::styled(
                format!(" \u{2502} {} CPUs", app.system.ncpus),
                Style::default().fg(COL_DIM),
            ),
            Span::styled(
                format!(
                    " \u{2502} POPCNT={} CTZ={} PTSelect={}",
                    hw.popcnt, hw.ctz, hw.ptselect
                ),
                Style::default().fg(COL_DIM),
            ),
        ]),
        Line::from(vec![
            Span::styled(
                format!(
                    "{} worker{} \u{00b7} {} bg \u{00b7} {} idle \u{00b7} {} shadow/w",
                    app.params.n_workers,
                    if app.params.n_workers > 1 { "s" } else { "" },
                    app.params.n_background,
                    app.params.n_idle,
                    app.params.shadows_per_worker,
                ),
                Style::default().fg(COL_DIM),
            ),
            if let Some(ref cal) = app.calibration {
                Span::styled(
                    format!(
                        " \u{00b7} {} iterations (auto: \u{03bc}={:.1}\u{03bc}s \u{03c3}={:.1}\u{03bc}s)",
                        cal.iterations, cal.probe_mean_us, cal.probe_stddev_us,
                    ),
                    Style::default().fg(COL_DIM),
                )
            } else {
                Span::raw("")
            },
        ]),
    ];

    let block = Block::default()
        .title(" POC Selector Benchmark ")
        .title_style(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        )
        .borders(Borders::TOP | Borders::LEFT | Borders::RIGHT);
    let paragraph = Paragraph::new(lines).block(block);
    f.render_widget(paragraph, area);
}

fn draw_progress(f: &mut Frame, area: Rect, app: &App) {
    let label = match &app.phase {
        Phase::Calibrating => "Calibrating...".to_string(),
        Phase::Discard => "Warmup (discard)...".to_string(),
        Phase::Running {
            round,
            total_rounds,
            poc_on,
        } => {
            let mode = if *poc_on { "POC ON" } else { "CFS" };
            format!("Round {}/{} [{}]", round, total_rounds, mode)
        }
        Phase::Error(msg) => format!("Error: {}", msg),
        Phase::Done => "Complete".to_string(),
    };

    let gauge = Gauge::default()
        .block(Block::default().borders(Borders::LEFT | Borders::RIGHT))
        .gauge_style(
            Style::default()
                .fg(match &app.phase {
                    Phase::Running { poc_on: true, .. } => COL_POC,
                    Phase::Running { poc_on: false, .. } => COL_CFS,
                    Phase::Error(_) => Color::Red,
                    Phase::Done => Color::Green,
                    _ => Color::Blue,
                })
                .add_modifier(Modifier::BOLD),
        )
        .label(label)
        .ratio(app.progress.clamp(0.0, 1.0));
    f.render_widget(gauge, area);
}

fn draw_histogram(f: &mut Frame, area: Rect, app: &App) {
    let block = Block::default()
        .title(" Latency Distribution (\u{03bc}s) ")
        .title_style(Style::default().fg(COL_LABEL))
        .borders(Borders::ALL);
    let inner = block.inner(area);
    f.render_widget(block, area);

    if inner.height < 3 || inner.width < 30 {
        return;
    }

    // Header line
    let half_w = (inner.width as usize - 8) / 2; // 8 for label + padding
    let header = Line::from(vec![
        Span::styled(format!("{:>6}", ""), Style::default()),
        Span::raw(" "),
        Span::styled(
            center_pad("POC ON", half_w),
            Style::default().fg(COL_POC).add_modifier(Modifier::BOLD),
        ),
        Span::raw(" "),
        Span::styled(
            center_pad("CFS (POC OFF)", half_w),
            Style::default().fg(COL_CFS).add_modifier(Modifier::BOLD),
        ),
    ]);

    let mut lines = vec![header];

    // Find global max for scaling
    let max_frac = max_histogram_frac(app.hist_on.as_ref(), app.hist_off.as_ref());

    for bucket in 0..NUM_BUCKETS {
        if lines.len() >= inner.height as usize {
            break;
        }
        let bar_w = half_w.saturating_sub(1);
        let on_frac = app
            .hist_on
            .as_ref()
            .map(|h| h.fraction(bucket))
            .unwrap_or(0.0);
        let off_frac = app
            .hist_off
            .as_ref()
            .map(|h| h.fraction(bucket))
            .unwrap_or(0.0);

        let on_bar = render_bar(on_frac, max_frac, bar_w, COL_POC);
        let off_bar = render_bar(off_frac, max_frac, bar_w, COL_CFS);

        let mut spans = vec![
            Span::styled(
                format!("{} ", BUCKET_LABELS[bucket]),
                Style::default().fg(COL_DIM),
            ),
            Span::raw("\u{2502}"),
        ];
        spans.extend(on_bar);
        spans.push(Span::raw("\u{2502} \u{2502}"));
        spans.extend(off_bar);
        spans.push(Span::raw("\u{2502}"));

        lines.push(Line::from(spans));
    }

    let paragraph = Paragraph::new(lines);
    f.render_widget(paragraph, inner);
}

fn draw_summary(f: &mut Frame, area: Rect, app: &App) {
    let block = Block::default()
        .title(" Summary ")
        .title_style(Style::default().fg(COL_LABEL))
        .borders(Borders::ALL);
    let inner = block.inner(area);
    f.render_widget(block, area);

    let (on, off) = match (app.final_on.as_ref(), app.final_off.as_ref()) {
        (Some(on), Some(off)) => (on, off),
        _ => {
            let msg = if app.finished {
                "No comparison data available"
            } else {
                "Waiting for results..."
            };
            let p = Paragraph::new(Line::from(Span::styled(msg, Style::default().fg(COL_DIM))));
            f.render_widget(p, inner);
            return;
        }
    };

    let mut lines = vec![Line::from(vec![
        Span::styled(format!("{:>12}", ""), Style::default()),
        Span::styled(
            format!("{:>14}", "POC ON"),
            Style::default().fg(COL_POC).add_modifier(Modifier::BOLD),
        ),
        Span::styled(
            format!("{:>14}", "CFS"),
            Style::default().fg(COL_CFS).add_modifier(Modifier::BOLD),
        ),
        Span::styled(
            format!("{:>12}", "\u{0394}"),
            Style::default()
                .fg(Color::White)
                .add_modifier(Modifier::BOLD),
        ),
    ])];

    let rows: Vec<(&str, f64, f64, bool)> = vec![
        ("mean", on.mean / 1000.0, off.mean / 1000.0, true),
        (
            "trimmed",
            on.trimmed_mean / 1000.0,
            off.trimmed_mean / 1000.0,
            true,
        ),
        ("p50", on.p50 as f64 / 1000.0, off.p50 as f64 / 1000.0, true),
        ("p99", on.p99 as f64 / 1000.0, off.p99 as f64 / 1000.0, true),
        ("ops/sec", on.ops_per_sec(), off.ops_per_sec(), false),
    ];

    for (label, v_on, v_off, lower_is_better) in rows {
        let delta = if v_off != 0.0 {
            (v_on - v_off) / v_off * 100.0
        } else {
            0.0
        };

        let is_better = if lower_is_better {
            delta < 0.0
        } else {
            delta > 0.0
        };
        let delta_color = if is_better { COL_BETTER } else { COL_WORSE };
        let arrow = if delta < 0.0 { "\u{25bc}" } else { "\u{25b2}" };

        let (on_str, off_str) = if label == "ops/sec" {
            (format_int(v_on), format_int(v_off))
        } else {
            (
                format!("{:.2} \u{03bc}s", v_on),
                format!("{:.2} \u{03bc}s", v_off),
            )
        };

        lines.push(Line::from(vec![
            Span::styled(format!("{:>12}", label), Style::default().fg(Color::White)),
            Span::styled(format!("{:>14}", on_str), Style::default().fg(COL_POC)),
            Span::styled(format!("{:>14}", off_str), Style::default().fg(COL_CFS)),
            Span::styled(
                format!("{:>+8.1}% {}", delta, arrow),
                Style::default()
                    .fg(delta_color)
                    .add_modifier(Modifier::BOLD),
            ),
        ]));
    }

    let paragraph = Paragraph::new(lines);
    f.render_widget(paragraph, inner);
}

fn draw_footer(f: &mut Frame, area: Rect, app: &App) {
    let text = if app.finished {
        "Press q to exit"
    } else {
        "Press q to abort"
    };
    let p = Paragraph::new(Line::from(Span::styled(text, Style::default().fg(COL_DIM))))
        .alignment(ratatui::layout::Alignment::Center);
    f.render_widget(p, area);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn render_bar(frac: f64, max_frac: f64, width: usize, color: Color) -> Vec<Span<'static>> {
    if max_frac <= 0.0 || width == 0 {
        return vec![Span::raw(" ".repeat(width))];
    }
    let filled = ((frac / max_frac) * width as f64).round() as usize;
    let filled = filled.min(width);
    let empty = width - filled;

    let pct = if frac > 0.001 {
        format!("{:>4.1}%", frac * 100.0)
    } else {
        "     ".to_string()
    };

    // Overlay percentage on the bar
    let bar_str = "\u{2588}".repeat(filled) + &" ".repeat(empty);
    let bar_chars: Vec<char> = bar_str.chars().collect();

    if bar_chars.len() >= pct.len() + 1 && filled >= pct.len() + 1 {
        // Draw percentage inside the bar
        let before = filled - pct.len() - 1;
        let after = empty;
        vec![
            Span::styled("\u{2588}".repeat(before + 1), Style::default().fg(color)),
            Span::styled(pct, Style::default().fg(Color::Black).bg(color)),
            Span::styled(" ".repeat(after), Style::default().fg(COL_DIM)),
        ]
    } else {
        vec![
            Span::styled("\u{2588}".repeat(filled), Style::default().fg(color)),
            Span::styled(" ".repeat(empty), Style::default().fg(COL_DIM)),
        ]
    }
}

fn max_histogram_frac(a: Option<&Histogram>, b: Option<&Histogram>) -> f64 {
    let mut max = 0.0_f64;
    for i in 0..NUM_BUCKETS {
        if let Some(h) = a {
            max = max.max(h.fraction(i));
        }
        if let Some(h) = b {
            max = max.max(h.fraction(i));
        }
    }
    max
}

fn center_pad(s: &str, width: usize) -> String {
    if s.len() >= width {
        return s[..width].to_string();
    }
    let pad = (width - s.len()) / 2;
    format!(
        "{}{}{}",
        " ".repeat(pad),
        s,
        " ".repeat(width - pad - s.len())
    )
}

fn format_int(v: f64) -> String {
    let v = v as u64;
    if v >= 1_000_000 {
        format!(
            "{},{:03},{:03}",
            v / 1_000_000,
            (v / 1_000) % 1_000,
            v % 1_000
        )
    } else if v >= 1_000 {
        format!("{},{:03}", v / 1_000, v % 1_000)
    } else {
        format!("{}", v)
    }
}

// ---------------------------------------------------------------------------
// Plain-text summary (printed after TUI exits)
// ---------------------------------------------------------------------------

pub fn print_summary(app: &App) {
    println!();
    println!("=== POC Selector Benchmark Results ===");
    println!("CPU: {}", app.system.cpu_model);
    let hw = &app.system.hw_features;
    println!(
        "HW:  POPCNT={} CTZ={} PTSelect={}",
        hw.popcnt, hw.ctz, hw.ptselect
    );
    println!(
        "Config: {} CPUs, {} workers, {} bg, {} idle, {} shadows/w",
        app.system.ncpus,
        app.params.n_workers,
        app.params.n_background,
        app.params.n_idle,
        app.params.shadows_per_worker,
    );
    if let Some(ref cal) = app.calibration {
        println!(
            "Calibrated: {} iterations (probe: mean={:.1}μs stddev={:.1}μs)",
            cal.iterations, cal.probe_mean_us, cal.probe_stddev_us,
        );
    }

    if let (Some(on), Some(off)) = (app.final_on.as_ref(), app.final_off.as_ref()) {
        println!();
        println!("{:>12} {:>14} {:>14} {:>12}", "", "POC ON", "CFS", "Δ");
        let rows: Vec<(&str, f64, f64, bool)> = vec![
            ("mean", on.mean / 1000.0, off.mean / 1000.0, true),
            (
                "trimmed",
                on.trimmed_mean / 1000.0,
                off.trimmed_mean / 1000.0,
                true,
            ),
            ("p50", on.p50 as f64 / 1000.0, off.p50 as f64 / 1000.0, true),
            ("p99", on.p99 as f64 / 1000.0, off.p99 as f64 / 1000.0, true),
            ("min", on.min as f64 / 1000.0, off.min as f64 / 1000.0, true),
            ("max", on.max as f64 / 1000.0, off.max as f64 / 1000.0, true),
            ("stddev", on.stddev / 1000.0, off.stddev / 1000.0, true),
            ("ops/sec", on.ops_per_sec(), off.ops_per_sec(), false),
        ];
        for (label, v_on, v_off, _lower_is_better) in rows {
            let delta = if v_off != 0.0 {
                (v_on - v_off) / v_off * 100.0
            } else {
                0.0
            };
            let (on_s, off_s) = if label == "ops/sec" {
                (format_int(v_on), format_int(v_off))
            } else {
                (format!("{:.2} μs", v_on), format!("{:.2} μs", v_off))
            };
            println!("{:>12} {:>14} {:>14} {:>+8.1}%", label, on_s, off_s, delta);
        }
    }
    println!();
}
