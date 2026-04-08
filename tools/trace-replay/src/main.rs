// trace-replay — agentOS IPC trace validator and replay tool
//
// Port of tools/trace_replay.mjs
//
// Usage:
//   trace-replay <trace.jsonl|-> [--validate] [--summary]
//
// Copyright (c) 2026 The agentOS Project
// SPDX-License-Identifier: BSD-2-Clause

use anyhow::Result;
use clap::Parser;
use serde::Deserialize;
use std::collections::HashMap;
use std::io::{BufRead, BufReader};

// ── CLI ───────────────────────────────────────────────────────────────────────

#[derive(Parser, Debug)]
#[command(
    name = "trace-replay",
    about = "agentOS IPC trace validator and replay tool"
)]
struct Cli {
    /// Path to JSONL trace file (use '-' for stdin)
    file: String,

    /// Check against expected_sequences (exits 1 on failure)
    #[arg(long)]
    validate: bool,

    /// Print PD pair stats, label frequency, timeline overview
    #[arg(long)]
    summary: bool,
}

// ── Data types ────────────────────────────────────────────────────────────────

#[derive(Deserialize, Debug, Clone)]
struct TraceEvent {
    tick: u64,
    src: String,
    dst: String,
    label: Option<String>,
}

struct ExpectedSeq {
    src: &'static str,
    dst: &'static str,
    label: Option<&'static str>, // None = match any
    desc: &'static str,
}

// ── Expected sequences ────────────────────────────────────────────────────────

const EXPECTED_SEQUENCES: &[ExpectedSeq] = &[
    ExpectedSeq {
        src: "controller",
        dst: "watchdog_pd",
        label: Some("0x50"),
        desc: "OP_WD_REGISTER",
    },
    ExpectedSeq {
        src: "controller",
        dst: "watchdog_pd",
        label: Some("0x51"),
        desc: "OP_WD_HEARTBEAT",
    },
    ExpectedSeq {
        src: "controller",
        dst: "mem_profiler",
        label: Some("0x60"),
        desc: "OP_MEM_ALLOC",
    },
    ExpectedSeq {
        src: "controller",
        dst: "agentfs",
        label: Some("0x30"),
        desc: "OP_AGENTFS_PUT",
    },
    ExpectedSeq {
        src: "controller",
        dst: "event_bus",
        label: Some("0x01"),
        desc: "MSG_EVENTBUS_INIT",
    },
    ExpectedSeq {
        src: "controller",
        dst: "worker_0",
        label: None,
        desc: "any controller→worker_0",
    },
];

// ── I/O ───────────────────────────────────────────────────────────────────────

fn read_events(path: &str) -> Result<Vec<TraceEvent>> {
    let reader: Box<dyn BufRead> = if path == "-" {
        Box::new(BufReader::new(std::io::stdin()))
    } else {
        Box::new(BufReader::new(std::fs::File::open(path)?))
    };

    let mut events = Vec::new();
    for (line_num, line_result) in reader.lines().enumerate() {
        let line = line_result?;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        match serde_json::from_str::<TraceEvent>(trimmed) {
            Ok(ev) => events.push(ev),
            Err(e) => {
                eprintln!("[warn] line {}: JSON parse error — {}", line_num + 1, e);
            }
        }
    }
    Ok(events)
}

// ── Summary printer ───────────────────────────────────────────────────────────

fn print_summary(events: &[TraceEvent]) {
    println!();
    println!("── agentOS IPC Trace Summary ──────────────────────────────");
    println!("  Total events : {}", events.len());

    if events.is_empty() {
        println!("  (no events recorded)");
        return;
    }

    let first = &events[0];
    let last = &events[events.len() - 1];
    println!(
        "  Tick range   : {} → {}  (span: {})",
        first.tick,
        last.tick,
        last.tick - first.tick
    );

    // Collect pair counts, label counts, and unique PDs
    let mut pair_count: HashMap<String, u64> = HashMap::new();
    let mut label_count: HashMap<String, u64> = HashMap::new();
    let mut pd_set: std::collections::BTreeSet<String> = std::collections::BTreeSet::new();

    for ev in events {
        let pair = format!("{} → {}", ev.src, ev.dst);
        *pair_count.entry(pair).or_insert(0) += 1;

        let lbl = ev.label.clone().unwrap_or_else(|| "?".to_string());
        *label_count.entry(lbl).or_insert(0) += 1;

        pd_set.insert(ev.src.clone());
        pd_set.insert(ev.dst.clone());
    }

    // Unique PDs
    println!();
    println!("  Unique PDs   : {}", pd_set.len());
    for pd in &pd_set {
        println!("    {}", pd);
    }

    // Top 10 PD pairs
    println!();
    println!("  Top PD pairs (by message count):");
    let mut sorted_pairs: Vec<(String, u64)> = pair_count.into_iter().collect();
    sorted_pairs.sort_by(|a, b| b.1.cmp(&a.1));
    for (pair, count) in sorted_pairs.iter().take(10) {
        println!("    {:>6}  {}", count, pair);
    }

    // Top 10 labels
    println!();
    println!("  Most frequent labels:");
    let mut sorted_labels: Vec<(String, u64)> = label_count.into_iter().collect();
    sorted_labels.sort_by(|a, b| b.1.cmp(&a.1));
    for (label, count) in sorted_labels.iter().take(10) {
        println!("    {:>6}  {}", count, label);
    }

    // Timeline: 10 equal tick-window buckets
    println!();
    println!("  Timeline (10 buckets):");
    let span = last.tick - first.tick;
    if span > 0 {
        let mut buckets = [0u64; 10];
        for ev in events {
            let idx = (((ev.tick - first.tick) as f64 / span as f64) * 10.0) as usize;
            let idx = idx.min(9);
            buckets[idx] += 1;
        }
        let max_b = *buckets.iter().max().unwrap_or(&1);
        let max_b = max_b.max(1);
        for (i, &count) in buckets.iter().enumerate() {
            let bar_len = ((count as f64 / max_b as f64) * 30.0).round() as usize;
            let bar: String = "█".repeat(bar_len);
            println!("    [{:>3}%] {:<30} {}", i * 10, bar, count);
        }
    }

    println!("──────────────────────────────────────────────────────────");
    println!();
}

// ── Validator ────────────────────────────────────────────────────────────────

fn validate(events: &[TraceEvent]) -> bool {
    println!();
    println!("── Regression Validation ──────────────────────────────────");
    let mut passed = 0u32;
    let mut failed = 0u32;

    for seq in EXPECTED_SEQUENCES {
        let found = events.iter().any(|ev| {
            if ev.src != seq.src {
                return false;
            }
            if ev.dst != seq.dst {
                return false;
            }
            if let Some(seq_label) = seq.label {
                let ev_label = ev.label.as_deref().unwrap_or("").to_lowercase();
                let seq_label_lc = seq_label.to_lowercase();
                if !ev_label.starts_with(&seq_label_lc) {
                    return false;
                }
            }
            true
        });

        let label_display = seq.label.unwrap_or("*");
        if found {
            println!(
                "  PASS  {} → {}  {}  ({})",
                seq.src, seq.dst, label_display, seq.desc
            );
            passed += 1;
        } else {
            println!(
                "  FAIL  {} → {}  {}  ({})",
                seq.src, seq.dst, label_display, seq.desc
            );
            failed += 1;
        }
    }

    println!();
    println!("  Result: {} passed, {} failed", passed, failed);
    println!("──────────────────────────────────────────────────────────");
    println!();

    failed == 0
}

// ── Main ──────────────────────────────────────────────────────────────────────

fn main() -> Result<()> {
    let cli = Cli::parse();

    let do_validate = cli.validate;
    let do_summary = cli.summary || !do_validate;

    let events = match read_events(&cli.file) {
        Ok(evs) => evs,
        Err(e) => {
            eprintln!("[error] Cannot read trace file '{}': {}", cli.file, e);
            std::process::exit(2);
        }
    };

    println!("Loaded {} trace events from '{}'", events.len(), cli.file);

    if do_summary {
        print_summary(&events);
    }

    if do_validate {
        let ok = validate(&events);
        std::process::exit(if ok { 0 } else { 1 });
    }

    Ok(())
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn make_event(tick: u64, src: &str, dst: &str, label: Option<&str>) -> TraceEvent {
        TraceEvent {
            tick,
            src: src.to_string(),
            dst: dst.to_string(),
            label: label.map(|s| s.to_string()),
        }
    }

    #[test]
    fn test_summary_empty() {
        // Should not panic on empty slice
        print_summary(&[]);
    }

    #[test]
    fn test_validate_pass() {
        let events = vec![
            make_event(1, "controller", "watchdog_pd", Some("0x50")),
            make_event(2, "controller", "watchdog_pd", Some("0x51")),
            make_event(3, "controller", "mem_profiler", Some("0x60")),
            make_event(4, "controller", "agentfs", Some("0x30")),
            make_event(5, "controller", "event_bus", Some("0x01")),
            make_event(6, "controller", "worker_0", Some("0x99")),
        ];
        assert!(validate(&events));
    }

    #[test]
    fn test_validate_fail() {
        // Missing the worker_0 event
        let events = vec![
            make_event(1, "controller", "watchdog_pd", Some("0x50")),
            make_event(2, "controller", "watchdog_pd", Some("0x51")),
            make_event(3, "controller", "mem_profiler", Some("0x60")),
            make_event(4, "controller", "agentfs", Some("0x30")),
            make_event(5, "controller", "event_bus", Some("0x01")),
            // intentionally omit worker_0
        ];
        assert!(!validate(&events));
    }

    #[test]
    fn test_label_match() {
        // "0x51abc" should match expected label "0x51" via prefix match
        let events = vec![
            make_event(1, "controller", "watchdog_pd", Some("0x50")),
            make_event(2, "controller", "watchdog_pd", Some("0x51abc")), // prefix match
            make_event(3, "controller", "mem_profiler", Some("0x60")),
            make_event(4, "controller", "agentfs", Some("0x30")),
            make_event(5, "controller", "event_bus", Some("0x01")),
            make_event(6, "controller", "worker_0", None),
        ];
        assert!(validate(&events));
    }
}
