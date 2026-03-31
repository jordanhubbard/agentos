#!/usr/bin/env node
/**
 * trace_replay.mjs — agentOS IPC trace validator and replay tool
 *
 * Reads a JSONL trace file (one TraceEvent JSON object per line) produced
 * by the trace_recorder PD's OP_TRACE_DUMP operation, then:
 *
 *   --summary   print unique PD pairs, most frequent message types, timeline
 *   --validate  check against expected_sequences[] for regression testing
 *               exits non-zero if any expected sequence is absent
 *
 * Usage:
 *   node tools/trace_replay.mjs trace.jsonl [--validate] [--summary]
 *   cat trace.jsonl | node tools/trace_replay.mjs - [--validate] [--summary]
 *
 * Event format (one JSON object per line):
 *   {"tick":N,"src":"controller","dst":"watchdog_pd","label":"0x51","mr0":0,"mr1":0}
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { createReadStream } from 'fs';
import { createInterface } from 'readline';
import { argv, stdin, stdout, stderr, exit } from 'process';

/* ── CLI argument parsing ─────────────────────────────────────────────── */
const args = argv.slice(2);

if (args.length === 0 || args.includes('--help') || args.includes('-h')) {
  stderr.write(`Usage: node trace_replay.mjs <trace.jsonl|--> [--validate] [--summary]\n`);
  stderr.write(`\n`);
  stderr.write(`  <trace.jsonl>  Path to JSONL trace file (use '-' for stdin)\n`);
  stderr.write(`  --validate     Check against expected_sequences[] (exits 1 on failure)\n`);
  stderr.write(`  --summary      Print PD pair stats, label frequency, timeline overview\n`);
  exit(0);
}

const filePath   = args.find(a => !a.startsWith('--')) ?? '-';
const doValidate = args.includes('--validate');
const doSummary  = args.includes('--summary') || (!doValidate);

/* ── Expected sequences for regression validation ───────────────────── */
/**
 * Each entry describes a src→dst message that MUST appear in the trace.
 * label is matched as a case-insensitive hex prefix (e.g. "0x51" matches
 * any label whose hex string starts with "0x51").
 *
 * Add entries here as the integration test suite grows.
 */
const EXPECTED_SEQUENCES = [
  /* Watchdog registration + heartbeat cycle */
  { src: 'controller',  dst: 'watchdog_pd',  label: '0x50', desc: 'OP_WD_REGISTER' },
  { src: 'controller',  dst: 'watchdog_pd',  label: '0x51', desc: 'OP_WD_HEARTBEAT' },

  /* MemProfiler alloc/free cycle */
  { src: 'controller',  dst: 'mem_profiler', label: '0x60', desc: 'OP_MEM_ALLOC' },

  /* AgentFS object store */
  { src: 'controller',  dst: 'agentfs',      label: '0x30', desc: 'OP_AGENTFS_PUT' },

  /* EventBus init */
  { src: 'controller',  dst: 'event_bus',    label: '0x01', desc: 'MSG_EVENTBUS_INIT' },

  /* Worker task dispatch */
  { src: 'controller',  dst: 'worker_0',     label: null,   desc: 'any controller→worker_0' },
];

/* ── Read JSONL from file or stdin ───────────────────────────────────── */
async function readEvents(path) {
  const stream = (path === '-')
    ? stdin
    : createReadStream(path, { encoding: 'utf8' });

  const rl = createInterface({ input: stream, crlfDelay: Infinity });
  const events = [];
  let lineNum = 0;

  for await (const line of rl) {
    lineNum++;
    const trimmed = line.trim();
    if (!trimmed) continue;
    try {
      const ev = JSON.parse(trimmed);
      if (typeof ev.tick !== 'number' ||
          typeof ev.src  !== 'string' ||
          typeof ev.dst  !== 'string') {
        stderr.write(`[warn] line ${lineNum}: missing required fields, skipping\n`);
        continue;
      }
      events.push(ev);
    } catch (e) {
      stderr.write(`[warn] line ${lineNum}: JSON parse error — ${e.message}\n`);
    }
  }
  return events;
}

/* ── Summary printer ─────────────────────────────────────────────────── */
function printSummary(events) {
  stdout.write(`\n── agentOS IPC Trace Summary ──────────────────────────────\n`);
  stdout.write(`  Total events : ${events.length}\n`);

  if (events.length === 0) {
    stdout.write(`  (no events recorded)\n`);
    return;
  }

  const first = events[0];
  const last  = events[events.length - 1];
  stdout.write(`  Tick range   : ${first.tick} → ${last.tick}  (span: ${last.tick - first.tick})\n`);

  /* PD pair counts */
  const pairCount = new Map();
  const labelCount = new Map();
  const pdSet = new Set();

  for (const ev of events) {
    const pair = `${ev.src} → ${ev.dst}`;
    pairCount.set(pair, (pairCount.get(pair) ?? 0) + 1);

    const lbl = ev.label ?? '?';
    labelCount.set(lbl, (labelCount.get(lbl) ?? 0) + 1);

    pdSet.add(ev.src);
    pdSet.add(ev.dst);
  }

  /* Unique PDs observed */
  stdout.write(`\n  Unique PDs   : ${pdSet.size}\n`);
  for (const pd of [...pdSet].sort()) {
    stdout.write(`    ${pd}\n`);
  }

  /* Top 10 PD pairs by message volume */
  stdout.write(`\n  Top PD pairs (by message count):\n`);
  const sortedPairs = [...pairCount.entries()].sort((a, b) => b[1] - a[1]).slice(0, 10);
  for (const [pair, count] of sortedPairs) {
    stdout.write(`    ${String(count).padStart(6)}  ${pair}\n`);
  }

  /* Top 10 most frequent message labels */
  stdout.write(`\n  Most frequent labels:\n`);
  const sortedLabels = [...labelCount.entries()].sort((a, b) => b[1] - a[1]).slice(0, 10);
  for (const [label, count] of sortedLabels) {
    stdout.write(`    ${String(count).padStart(6)}  ${label}\n`);
  }

  /* Timeline: bucket events into 10 equal tick-windows */
  stdout.write(`\n  Timeline (10 buckets):\n`);
  const span = last.tick - first.tick;
  if (span > 0) {
    const buckets = new Array(10).fill(0);
    for (const ev of events) {
      const idx = Math.min(9, Math.floor(((ev.tick - first.tick) / span) * 10));
      buckets[idx]++;
    }
    const maxB = Math.max(...buckets);
    for (let i = 0; i < 10; i++) {
      const bar = '█'.repeat(Math.round((buckets[i] / maxB) * 30)).padEnd(30);
      stdout.write(`    [${String(i * 10).padStart(3)}%] ${bar} ${buckets[i]}\n`);
    }
  }

  stdout.write(`──────────────────────────────────────────────────────────\n\n`);
}

/* ── Regression validator ────────────────────────────────────────────── */
function validate(events) {
  stdout.write(`\n── Regression Validation ──────────────────────────────────\n`);
  let passed = 0;
  let failed = 0;

  for (const seq of EXPECTED_SEQUENCES) {
    const found = events.some(ev => {
      if (ev.src !== seq.src) return false;
      if (ev.dst !== seq.dst) return false;
      if (seq.label !== null) {
        const evLabel  = String(ev.label  ?? '').toLowerCase();
        const seqLabel = String(seq.label ?? '').toLowerCase();
        if (!evLabel.startsWith(seqLabel)) return false;
      }
      return true;
    });

    if (found) {
      stdout.write(`  PASS  ${seq.src} → ${seq.dst}  ${seq.label ?? '*'}  (${seq.desc})\n`);
      passed++;
    } else {
      stdout.write(`  FAIL  ${seq.src} → ${seq.dst}  ${seq.label ?? '*'}  (${seq.desc})\n`);
      failed++;
    }
  }

  stdout.write(`\n  Result: ${passed} passed, ${failed} failed\n`);
  stdout.write(`──────────────────────────────────────────────────────────\n\n`);

  return failed === 0;
}

/* ── Main ────────────────────────────────────────────────────────────── */
(async () => {
  let events;
  try {
    events = await readEvents(filePath);
  } catch (e) {
    stderr.write(`[error] Cannot read trace file '${filePath}': ${e.message}\n`);
    exit(2);
  }

  stdout.write(`Loaded ${events.length} trace events from '${filePath}'\n`);

  if (doSummary) {
    printSummary(events);
  }

  if (doValidate) {
    const ok = validate(events);
    exit(ok ? 0 : 1);
  }

  exit(0);
})();
