/// Log2-scaled histogram buckets in microseconds.
/// Buckets: [0,1), [1,2), [2,4), [4,8), [8,16), [16,32), [32,64), [64,128), [128+)
pub const NUM_BUCKETS: usize = 9;
pub const BUCKET_LABELS: [&str; NUM_BUCKETS] = [
    " <1 ", "  1 ", "  2 ", "  4 ", "  8 ", " 16 ", " 32 ", " 64 ", "128+",
];

#[derive(Clone, Default)]
pub struct StatResult {
    pub mean: f64,
    pub trimmed_mean: f64,
    pub stddev: f64,
    pub min: u64,
    pub max: u64,
    pub p50: u64,
    pub p99: u64,
    pub count: usize,
}

#[derive(Clone, Default)]
pub struct Histogram {
    pub buckets: [u32; NUM_BUCKETS],
    pub total: u32,
}

impl StatResult {
    pub fn compute(samples: &mut [u64]) -> Self {
        if samples.is_empty() {
            return Self::default();
        }
        samples.sort_unstable();
        let n = samples.len();
        let min = samples[0];
        let max = samples[n - 1];
        let p50 = samples[n / 2];
        let p99 = samples[((n - 1) as f64 * 0.99) as usize];

        let sum: f64 = samples.iter().map(|&v| v as f64).sum();
        let mean = sum / n as f64;

        let var: f64 = samples
            .iter()
            .map(|&v| {
                let d = v as f64 - mean;
                d * d
            })
            .sum::<f64>()
            / n as f64;

        // Trimmed mean: drop top/bottom 1% to remove outliers
        let lo = n / 100;
        let hi = n - lo;
        let trimmed = &samples[lo..hi];
        let trimmed_mean = if !trimmed.is_empty() {
            trimmed.iter().map(|&v| v as f64).sum::<f64>() / trimmed.len() as f64
        } else {
            mean
        };

        Self {
            mean,
            trimmed_mean,
            stddev: var.sqrt(),
            min,
            max,
            p50,
            p99,
            count: n,
        }
    }

    pub fn merge(results: &[StatResult]) -> Self {
        if results.is_empty() {
            return Self::default();
        }
        let n = results.len() as f64;
        let mean = results.iter().map(|r| r.mean).sum::<f64>() / n;
        let trimmed_mean = results.iter().map(|r| r.trimmed_mean).sum::<f64>() / n;
        let stddev = (results.iter().map(|r| r.stddev * r.stddev).sum::<f64>() / n).sqrt();
        let min = results.iter().map(|r| r.min).min().unwrap_or(0);
        let max = results.iter().map(|r| r.max).max().unwrap_or(0);
        let p50 = (results.iter().map(|r| r.p50 as f64).sum::<f64>() / n) as u64;
        let p99 = (results.iter().map(|r| r.p99 as f64).sum::<f64>() / n) as u64;
        let count = results.iter().map(|r| r.count).sum();
        Self {
            mean,
            trimmed_mean,
            stddev,
            min,
            max,
            p50,
            p99,
            count,
        }
    }

    pub fn ops_per_sec(&self) -> f64 {
        if self.mean <= 0.0 {
            0.0
        } else {
            1e9 / self.mean
        }
    }
}

impl Histogram {
    pub fn from_samples(samples: &[u64]) -> Self {
        let mut h = Self::default();
        for &ns in samples {
            let us = ns / 1000; // ns → μs
            let bucket = match us {
                0 => 0,
                1 => 1,
                2..=3 => 2,
                4..=7 => 3,
                8..=15 => 4,
                16..=31 => 5,
                32..=63 => 6,
                64..=127 => 7,
                _ => 8,
            };
            h.buckets[bucket] += 1;
            h.total += 1;
        }
        h
    }

    pub fn fraction(&self, bucket: usize) -> f64 {
        if self.total == 0 {
            0.0
        } else {
            self.buckets[bucket] as f64 / self.total as f64
        }
    }
}
