use std::collections::HashSet;
use std::fs;
use std::io::Write;

const SYSCTL_PATH: &str = "/proc/sys/kernel/sched_poc_selector";

#[derive(Clone)]
pub struct SystemInfo {
    pub ncpus: usize,
    pub physical_cores: usize,
    pub cpu_model: String,
    pub hw_features: HwFeatures,
}

#[derive(Clone)]
pub struct HwFeatures {
    pub popcnt: &'static str,
    pub ctz: &'static str,
    pub ptselect: &'static str,
}

#[derive(Clone)]
pub struct BenchParams {
    pub n_workers: usize,
    pub n_background: usize,
    pub n_idle: usize,
    pub shadows_per_worker: usize,
}

impl SystemInfo {
    pub fn detect() -> Self {
        let ncpus = unsafe { libc::sysconf(libc::_SC_NPROCESSORS_ONLN) as usize };
        let physical_cores = detect_physical_cores(ncpus);
        let cpu_model = read_cpu_model().unwrap_or_else(|| "Unknown".into());
        let hw_features = detect_hw_features();
        Self {
            ncpus,
            physical_cores,
            cpu_model,
            hw_features,
        }
    }
}

impl BenchParams {
    #[allow(dead_code)]
    pub fn calculate(ncpus: usize, physical_cores: usize) -> Self {
        let n_background = physical_cores * 3 / 4;
        Self::compute(ncpus, n_background, None)
    }

    pub fn with_overrides(
        ncpus: usize,
        physical_cores: usize,
        workers: Option<usize>,
        background: Option<usize>,
    ) -> Self {
        let n_background = background.unwrap_or(physical_cores * 3 / 4);
        Self::compute(ncpus, n_background, workers)
    }

    // ncpus = 1 (dispatcher) + bg + workers * (1 + shadows) + idle
    fn compute(ncpus: usize, n_background: usize, workers: Option<usize>) -> Self {
        let n_background = n_background.min(ncpus.saturating_sub(2));
        let available = ncpus.saturating_sub(1 + n_background);
        let shadows_per_worker = if available >= 3 { 2 } else { 1 };
        let group = 1 + shadows_per_worker;
        let n_workers = match workers {
            Some(w) => w.min(available / group).max(1),
            None => (available / group).max(1),
        };
        let n_idle = available.saturating_sub(n_workers * group);
        Self {
            n_workers,
            n_background,
            n_idle,
            shadows_per_worker,
        }
    }
}

pub fn poc_sysctl_read() -> Option<i32> {
    fs::read_to_string(SYSCTL_PATH)
        .ok()
        .and_then(|s| s.trim().parse().ok())
}

pub fn poc_sysctl_write(val: i32) -> Result<(), String> {
    let mut f = fs::OpenOptions::new()
        .write(true)
        .open(SYSCTL_PATH)
        .map_err(|e| format!("open({SYSCTL_PATH}): {e}"))?;
    // Single write_all call — writeln!/write! split output into multiple
    // write() syscalls, and procfs rejects the trailing "\n"-only write
    // with EINVAL. Formatting first ensures one atomic write(2).
    let buf = format!("{val}\n");
    f.write_all(buf.as_bytes())
        .map_err(|e| format!("write({SYSCTL_PATH}): {e}"))?;
    std::thread::sleep(std::time::Duration::from_millis(50));
    Ok(())
}

fn detect_physical_cores(ncpus: usize) -> usize {
    let mut cores = HashSet::new();
    for cpu in 0..ncpus {
        let pkg = fs::read_to_string(format!(
            "/sys/devices/system/cpu/cpu{cpu}/topology/physical_package_id"
        ));
        let core = fs::read_to_string(format!("/sys/devices/system/cpu/cpu{cpu}/topology/core_id"));
        if let (Ok(p), Ok(c)) = (pkg, core) {
            if let (Ok(p), Ok(c)) = (p.trim().parse::<i32>(), c.trim().parse::<i32>()) {
                cores.insert((p, c));
            }
        }
    }
    if cores.is_empty() {
        ncpus
    } else {
        cores.len()
    }
}

fn read_cpu_model() -> Option<String> {
    let contents = fs::read_to_string("/proc/cpuinfo").ok()?;
    for line in contents.lines() {
        if line.starts_with("model name") {
            if let Some(val) = line.split(':').nth(1) {
                return Some(val.trim().to_string());
            }
        }
    }
    None
}

#[cfg(target_arch = "x86_64")]
fn detect_hw_features() -> HwFeatures {
    use core::arch::x86_64::{__cpuid, __cpuid_count};

    let popcnt;
    let bmi1;
    let bmi2;

    unsafe {
        // CPUID leaf 1: POPCNT (ECX bit 23)
        let r1 = __cpuid(1);
        popcnt = (r1.ecx >> 23) & 1 == 1;

        // CPUID leaf 7, subleaf 0: BMI1 (EBX bit 3), BMI2 (EBX bit 8)
        let r7 = __cpuid_count(7, 0);
        bmi1 = (r7.ebx >> 3) & 1 == 1;
        bmi2 = (r7.ebx >> 8) & 1 == 1;
    }

    HwFeatures {
        popcnt: if popcnt { "yes" } else { "no" },
        ctz: if bmi1 { "TZCNT" } else { "BSF" },
        ptselect: if bmi2 { "PDEP" } else { "SW" },
    }
}

#[cfg(target_arch = "aarch64")]
fn detect_hw_features() -> HwFeatures {
    HwFeatures {
        popcnt: "CNT",
        ctz: "RBIT+CLZ",
        ptselect: "SW",
    }
}

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
fn detect_hw_features() -> HwFeatures {
    HwFeatures {
        popcnt: "?",
        ctz: "?",
        ptselect: "?",
    }
}
