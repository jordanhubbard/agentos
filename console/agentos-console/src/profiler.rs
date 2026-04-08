//! Mock profiler snapshot generator.
//!
//! Ports generateProfilerSnapshot() from agentos_console.mjs exactly.

use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
pub struct ProfilerFrame {
    #[serde(rename = "fn")]
    pub fn_name: String,
    pub ticks:   u64,
    pub depth:   u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct ProfilerSlot {
    pub id:      u32,
    pub name:    String,
    pub cpu_pct: u32,
    pub mem_kb:  u64,
    pub ticks:   u64,
    pub frames:  Vec<ProfilerFrame>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ProfilerSnapshot {
    pub ts:    u64,
    pub slots: Vec<ProfilerSlot>,
}

fn jitter(base: i64, range: i64) -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    // Very lightweight pseudo-random using system time nanos
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.subsec_nanos() as i64)
        .unwrap_or(0);
    let offset = ((nanos % (range + 1)) - range / 2) as i64;
    (base + offset).max(0) as u64
}

fn cpu_jitter(base: i32, spread: i32) -> u32 {
    use std::time::{SystemTime, UNIX_EPOCH};
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.subsec_nanos() as i32)
        .unwrap_or(0);
    let offset = (nanos % (spread * 2 + 1)) - spread;
    ((base + offset).max(1).min(99)) as u32
}

pub fn generate_profiler_snapshot() -> ProfilerSnapshot {
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);

    ProfilerSnapshot {
        ts: now_ms,
        slots: vec![
            ProfilerSlot {
                id:      0,
                name:    "inference_worker".into(),
                cpu_pct: cpu_jitter(42, 5),
                mem_kb:  jitter(8192, 512),
                ticks:   jitter(12450, 200),
                frames:  vec![
                    ProfilerFrame { fn_name: "matmul_f32".into(),   ticks: jitter(5200, 200), depth: 0 },
                    ProfilerFrame { fn_name: "softmax".into(),       ticks: jitter(2100, 200), depth: 1 },
                    ProfilerFrame { fn_name: "embed_lookup".into(),  ticks: jitter(1800, 200), depth: 1 },
                    ProfilerFrame { fn_name: "layer_norm".into(),    ticks: jitter(900,  200), depth: 2 },
                    ProfilerFrame { fn_name: "rms_norm".into(),      ticks: jitter(620,  200), depth: 2 },
                    ProfilerFrame { fn_name: "rope_enc".into(),      ticks: jitter(480,  200), depth: 3 },
                ],
            },
            ProfilerSlot {
                id:      1,
                name:    "event_handler".into(),
                cpu_pct: cpu_jitter(8, 2),
                mem_kb:  jitter(512, 64),
                ticks:   jitter(2340, 150),
                frames:  vec![
                    ProfilerFrame { fn_name: "dispatch_event".into(), ticks: jitter(1200, 100), depth: 0 },
                    ProfilerFrame { fn_name: "cap_check".into(),      ticks: jitter(600,  80),  depth: 1 },
                    ProfilerFrame { fn_name: "ring_enqueue".into(),   ticks: jitter(340,  60),  depth: 2 },
                ],
            },
            ProfilerSlot {
                id:      2,
                name:    "vibe_validator".into(),
                cpu_pct: cpu_jitter(15, 3),
                mem_kb:  jitter(2048, 256),
                ticks:   jitter(4110, 300),
                frames:  vec![
                    ProfilerFrame { fn_name: "wasm_validate".into(), ticks: jitter(2800, 200), depth: 0 },
                    ProfilerFrame { fn_name: "section_parse".into(), ticks: jitter(1100, 100), depth: 1 },
                    ProfilerFrame { fn_name: "type_check".into(),    ticks: jitter(540,  80),  depth: 2 },
                ],
            },
        ],
    }
}
