#!/usr/bin/env node
/**
 * agentOS Console WebSocket Bridge
 *
 * Reads the QEMU serial log (/tmp/agentos-serial.log) and routes each line to
 * the appropriate PD slot based on log prefix, then streams live output to
 * browser xterm.js clients via WebSocket.
 *
 * When agentOS exposes a real HTTP API at AGENTOS_BASE the bridge can also
 * poll that; the serial reader is always the primary source.
 *
 * Protocol (client → server):
 *   {"action":"subscribe","slot":2}           — start receiving output from slot 2
 *   {"action":"unsubscribe","slot":2}         — stop
 *   {"action":"attach","slot":2}              — POST attach to agentOS (optional)
 *   {"action":"input","slot":2,"data":"..."}  — inject keystrokes into slot 2
 *   {"action":"list"}                         — get all slots with line counts
 *
 * Protocol (server → client):
 *   {"slot":2,"line":"[vibe_engine] WASM slot 0 ready\n"}
 *   {"event":"attached","slot":2}
 *   {"event":"slots","data":[{"slot":0,"lines":42},…]}
 *   {"event":"error","msg":"..."}
 *
 * Listens on port 8795.
 */

import { WebSocketServer } from 'ws';
import http from 'http';
import net from 'net';
import fs from 'fs';
import { fileURLToPath } from 'url';
import nodePath from 'path';
import { spawn } from 'child_process';

const __dirname = nodePath.dirname(fileURLToPath(import.meta.url));

const WS_PORT       = 8795;
const SERIAL_SOCK   = '/tmp/agentos-serial.sock';
const AGENTOS_BASE  = 'http://127.0.0.1:8789';
const AGENTOS_TOKEN = 'agentos-console-20maaghccmbmnby63so';
const MAX_SLOTS     = 16;
const MAX_HISTORY   = 2000;  // lines kept per slot for late-joining clients

const SLOT_NAMES = [
  'monitor', 'init_agent', 'event_bus', 'agentfs',
  'cap_broker', 'cap_audit_log', 'agent_pool/worker', 'vibe_engine',
  'mem_profiler', 'watchdog', 'trace_recorder', 'cap_policy',
  'debug_bridge', 'fault_handler', 'quota_pd', 'linux_vm',
];

// ─── Mock profiler data ──────────────────────────────────────────────────────

function generateProfilerSnapshot() {
  const j = (base, range = 200) =>
    Math.max(0, base + Math.floor((Math.random() - 0.5) * range));
  return {
    ts: Date.now(),
    slots: [
      { id: 0, name: 'inference_worker',
        cpu_pct: Math.min(99, Math.max(1, 42 + Math.floor(Math.random() * 10 - 5))),
        mem_kb: j(8192, 512), ticks: j(12450),
        frames: [
          { fn: 'matmul_f32',   ticks: j(5200), depth: 0 },
          { fn: 'softmax',      ticks: j(2100), depth: 1 },
          { fn: 'embed_lookup', ticks: j(1800), depth: 1 },
          { fn: 'layer_norm',   ticks: j(900),  depth: 2 },
          { fn: 'rms_norm',     ticks: j(620),  depth: 2 },
          { fn: 'rope_enc',     ticks: j(480),  depth: 3 },
        ],
      },
      { id: 1, name: 'event_handler',
        cpu_pct: Math.min(99, Math.max(1, 8 + Math.floor(Math.random() * 4 - 2))),
        mem_kb: j(512, 64), ticks: j(2340, 150),
        frames: [
          { fn: 'dispatch_event', ticks: j(1200, 100), depth: 0 },
          { fn: 'cap_check',      ticks: j(600,  80),  depth: 1 },
          { fn: 'ring_enqueue',   ticks: j(340,  60),  depth: 2 },
        ],
      },
      { id: 2, name: 'vibe_validator',
        cpu_pct: Math.min(99, Math.max(1, 15 + Math.floor(Math.random() * 6 - 3))),
        mem_kb: j(2048, 256), ticks: j(4110, 300),
        frames: [
          { fn: 'wasm_validate', ticks: j(2800, 200), depth: 0 },
          { fn: 'section_parse', ticks: j(1100, 100), depth: 1 },
          { fn: 'type_check',    ticks: j(540,  80),  depth: 2 },
        ],
      },
    ],
  };
}

// ─── Serial → slot routing ───────────────────────────────────────────────────
// Order matters: first match wins.
const SERIAL_ROUTES = [
  { slot: 7,  prefixes: ['[vibe_engine]', '[vibe_swap]'] },
  { slot: 3,  prefixes: ['[agentfs]'] },
  { slot: 2,  prefixes: ['[event_bus]'] },
  { slot: 1,  prefixes: ['[init_agent]'] },
  { slot: 6,  prefixes: ['[worker]', '[pool]', '[agent_pool]'] },
  { slot: 4,  prefixes: ['[cap_broker]'] },
  { slot: 5,  prefixes: ['[cap_audit_log]'] },
  { slot: 9,  prefixes: ['[watchdog]'] },
  { slot: 10, prefixes: ['[trace_recorder]'] },
  { slot: 11, prefixes: ['[cap_policy]'] },
  { slot: 12, prefixes: ['[debug_bridge]'] },
  { slot: 13, prefixes: ['[fault_handler]'] },
  { slot: 14, prefixes: ['[quota_pd]'] },
  { slot: 8,  prefixes: ['[mem_profiler]'] },
  // seL4 monitor / controller / boot messages → slot 0
  { slot: 0,  prefixes: ['MON|', 'LDR|', '[boot]', '[controller]',
                          '[boot_integrity]', '[time_partition]', '[core_affinity]',
                          '[power_mgr]', '[perf_counters]', '[console_mux]',
                          'Bootstrapping kernel', 'INFO  [sel4_', 'MICROKIT|'] },
  // Linux VMM and kernel boot → slot 15 (linux_vm)
  // Use line.startsWith() only — no broad substring matches like 'OK' or 'Starting '
  { slot: 15, prefixes: ['linux_vmm|', '[    ', 'Welcome to Buildroot',
                          'buildroot login', 'Starting syslogd', 'Starting klogd',
                          'Starting network', 'Running sysctl', 'Saving random seed',
                          'Booting all finished'] },
];

function lineToSlot(line) {
  for (const { slot, prefixes } of SERIAL_ROUTES) {
    for (const pfx of prefixes) {
      if (line.startsWith(pfx)) return slot;
    }
  }
  return 0; // default to monitor
}

// ─── State ──────────────────────────────────────────────────────────────────

/** Map<slot, Set<ws>> — clients subscribed to each slot */
const subscribers = new Map();
/** Map<slot, string[]> — per-slot line history for late-joining clients */
const slotHistory  = new Map();
/** Set<slot> — slots that have received at least one line */
const activeSlots  = new Set();

for (let i = 0; i < MAX_SLOTS; i++) {
  subscribers.set(i, new Set());
  slotHistory.set(i, []);
}

// ─── Bidirectional serial socket ─────────────────────────────────────────────
// QEMU is launched with -serial unix:/tmp/agentos-serial.sock,server,nowait
// (QEMU creates the socket; we connect as client).  All serial output is parsed
// and routed to slots; all keyboard input is written back to the socket.

let serialSocket  = null;   // active net.Socket, or null
let serialBuffer  = '';     // incomplete line accumulator
let lastFlushBuf  = '';
let lastFlushTs   = 0;

function onSerialData(chunk) {
  serialBuffer += chunk.toString('utf-8');

  // Emit complete lines
  const lastNL = serialBuffer.lastIndexOf('\n');
  if (lastNL >= 0) {
    const lines = serialBuffer.slice(0, lastNL).split('\n');
    serialBuffer = serialBuffer.slice(lastNL + 1);
    for (const rawLine of lines) {
      const line = rawLine.replace(/\r/g, '');
      if (!line) continue;
      emitSerialLine(line);
    }
    lastFlushBuf = '';
    lastFlushTs  = Date.now();
  }
}

function emitSerialLine(line) {
  const slot = lineToSlot(line);
  const hist  = slotHistory.get(slot);
  hist.push(line);
  if (hist.length > MAX_HISTORY) hist.shift();
  activeSlots.add(slot);
  broadcast(slot, { slot, line });
}

// Flush partial lines (e.g. "buildroot login: " — no trailing \n).
setInterval(() => {
  if (!serialBuffer) return;
  const now = Date.now();
  if (serialBuffer === lastFlushBuf) {
    if (now - lastFlushTs > 1000) {
      const partial = serialBuffer.replace(/\r/g, '').trimEnd();
      if (partial) emitSerialLine(partial);
      serialBuffer = '';
      lastFlushBuf = '';
    }
  } else {
    lastFlushBuf = serialBuffer;
    lastFlushTs  = now;
  }
}, 300);

function serialWrite(data) {
  if (serialSocket && !serialSocket.destroyed) {
    serialSocket.write(data);
  }
}

function connectSerial() {
  const sock = net.createConnection(SERIAL_SOCK);

  sock.on('connect', () => {
    console.log('[console-ws] serial socket connected');
    serialSocket = sock;
  });

  sock.on('data', onSerialData);

  sock.on('close', () => {
    serialSocket = null;
    console.log('[console-ws] serial socket closed — retry in 1 s');
    setTimeout(connectSerial, 1000);
  });

  sock.on('error', () => { /* 'close' fires next, handles retry */ });
}

connectSerial();

// ─── Optional agentOS HTTP API polling (for when the API is implemented) ─────

async function agentosGet(path) {
  const url = `${AGENTOS_BASE}${path}`;
  const res = await fetch(url, {
    headers: { Authorization: `Bearer ${AGENTOS_TOKEN}` },
    signal: AbortSignal.timeout(3000),
  });
  if (!res.ok) throw new Error(`agentOS ${path} → ${res.status}`);
  return res.json();
}

async function agentosPost(path, body = {}) {
  const url = `${AGENTOS_BASE}${path}`;
  const res = await fetch(url, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${AGENTOS_TOKEN}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(3000),
  });
  if (!res.ok) throw new Error(`agentOS POST ${path} → ${res.status}`);
  return res.json();
}

// ─── Guest OS fetch ───────────────────────────────────────────────────────────
// Track in-progress fetch jobs: os → { proc, log[] }
const fetchJobs = new Map();
const REPO_ROOT = nodePath.join(__dirname, '..');

const FETCH_SCRIPTS = {
  freebsd: nodePath.join(REPO_ROOT, 'scripts', 'fetch-freebsd-guest.sh'),
  ubuntu:  nodePath.join(REPO_ROOT, 'scripts', 'fetch-ubuntu-guest.sh'),
};

function startFetch(os) {
  if (fetchJobs.has(os)) return { status: 'already-running' };
  const script = FETCH_SCRIPTS[os];
  if (!script) return { status: 'error', msg: `Unknown OS: ${os}` };

  const log = [];
  const proc = spawn('bash', [script], { cwd: REPO_ROOT });
  fetchJobs.set(os, { proc, log, done: false, exitCode: null });

  const onLine = (line) => {
    log.push(line);
    // Broadcast as a system event to all connected clients
    broadcastAll({ event: 'fetch-progress', os, line });
  };

  proc.stdout.on('data', d => d.toString().split('\n').filter(Boolean).forEach(onLine));
  proc.stderr.on('data', d => d.toString().split('\n').filter(Boolean).forEach(onLine));
  proc.on('close', (code) => {
    const job = fetchJobs.get(os);
    if (job) { job.done = true; job.exitCode = code; }
    broadcastAll({ event: 'fetch-done', os, exitCode: code });
    console.log(`[console-ws] fetch ${os} exited ${code}`);
  });

  return { status: 'started' };
}

function broadcastAll(msg) {
  const str = JSON.stringify(msg);
  for (const [, subs] of subscribers) {
    for (const ws of subs) {
      if (ws.readyState === 1) ws.send(str);
    }
  }
}

// ─── Broadcast ───────────────────────────────────────────────────────────────

function broadcast(slot, msg) {
  const str = JSON.stringify(msg);
  for (const ws of subscribers.get(slot) ?? []) {
    if (ws.readyState === 1 /* OPEN */) ws.send(str);
  }
}

// ─── HTTP server ──────────────────────────────────────────────────────────────

const server = http.createServer(async (req, res) => {
  const pathname = (req.url ?? '/').split('?')[0];

  // ── Console HTML ──────────────────────────────────────────────────
  if (pathname === '/' || pathname === '/dashboard') {
    try {
      const html = fs.readFileSync(nodePath.join(__dirname, 'index.html'), 'utf-8');
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(html);
    } catch {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.end('index.html not found');
    }

  // ── Health ────────────────────────────────────────────────────────
  } else if (pathname === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      ok: true,
      serialConnected: serialSocket !== null && !serialSocket.destroyed,
      activeSlots: [...activeSlots],
      subscriberCounts: Object.fromEntries(
        [...subscribers.entries()].map(([k, v]) => [k, v.size])
      ),
    }));

  // ── Slot list ─────────────────────────────────────────────────────
  } else if (pathname === '/api/agentos/slots') {
    // Try live agentOS API first; fall back to mock based on active serial slots
    try {
      const data = await agentosGet('/api/agentos/slots');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(data));
    } catch {
      const slots = SLOT_NAMES.map((name, id) => ({
        id,
        name,
        active: activeSlots.has(id) || id < 12,
      }));
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ slots }));
    }

  // ── Console line history ──────────────────────────────────────────
  } else if (/^\/api\/agentos\/console\/\d+$/.test(pathname)) {
    const slot = parseInt(pathname.split('/').pop(), 10);
    if (slot < 0 || slot >= MAX_SLOTS) { res.writeHead(400); res.end(); return; }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ lines: slotHistory.get(slot) ?? [] }));

  } else if (pathname === '/api/agentos/profiler/snapshot') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(generateProfilerSnapshot()));

  } else if (pathname === '/api/vm/images') {
    const guestDir = nodePath.join(__dirname, '..', 'guest-images');
    let available = [];
    try {
      available = fs.readdirSync(guestDir)
        .filter(f => /\.(img|qcow2|raw|fd)$/.test(f))
        .map(f => ({ name: f, size: fs.statSync(nodePath.join(guestDir, f)).size }));
    } catch {}
    const vmRunning = (slotHistory.get(15)?.length ?? 0) > 0;
    res.writeHead(200, { 'Content-Type': 'application/json' });
    // Annotate available images with known OS names and fetch status
    const KNOWN = {
      freebsd: { label: 'FreeBSD 14.4 AArch64', img: 'freebsd-14.4-aarch64.img' },
      ubuntu:  { label: 'Ubuntu 24.04 LTS AArch64', img: 'ubuntu-24.04-aarch64.img' },
    };
    const guests = Object.entries(KNOWN).map(([os, { label, img }]) => {
      const imgPath = nodePath.join(guestDir, img);
      const present = fs.existsSync(imgPath);
      const job = fetchJobs.get(os);
      return {
        os, label, img, present,
        fetching: job ? !job.done : false,
        fetchLog: job?.log ?? [],
      };
    });
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ running: vmRunning ? [{ name: 'Linux VM', slot: 15 }] : [], guests }));

  // ── Guest OS fetch (POST) ────────────────────────────────────────────
  } else if (pathname === '/api/vm/fetch' && req.method === 'POST') {
    let body = '';
    req.on('data', d => { body += d; });
    req.on('end', () => {
      let os;
      try { os = JSON.parse(body).os; } catch {}
      if (!os || !FETCH_SCRIPTS[os]) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: `Unknown OS: ${os}. Use 'freebsd' or 'ubuntu'.` }));
        return;
      }
      const result = startFetch(os);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(result));
    });

  // ── Fetch status ──────────────────────────────────────────────────────
  } else if (pathname.startsWith('/api/vm/fetch/')) {
    const os = pathname.split('/').pop();
    const job = fetchJobs.get(os);
    if (!job) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ os, done: job.done, exitCode: job.exitCode, log: job.log }));

  } else {
    res.writeHead(404);
    res.end();
  }
});

// ─── WebSocket server ────────────────────────────────────────────────────────

const wss = new WebSocketServer({ server });

wss.on('connection', (ws, req) => {
  const clientIp = req.socket.remoteAddress;
  console.log(`[console-ws] client connected: ${clientIp}`);

  ws.on('message', async (raw) => {
    let msg;
    try { msg = JSON.parse(raw.toString()); }
    catch { ws.send(JSON.stringify({ event: 'error', msg: 'invalid JSON' })); return; }

    const { action, slot } = msg;

    if (action === 'subscribe') {
      if (typeof slot !== 'number' || slot < 0 || slot >= MAX_SLOTS) {
        ws.send(JSON.stringify({ event: 'error', msg: `slot must be 0-${MAX_SLOTS - 1}` }));
        return;
      }
      subscribers.get(slot).add(ws);
      ws.send(JSON.stringify({ event: 'subscribed', slot, name: SLOT_NAMES[slot] ?? 'unknown' }));

      // Replay history for late-joining clients
      const hist = slotHistory.get(slot);
      if (hist.length > 0) {
        for (const line of hist) {
          ws.send(JSON.stringify({ slot, line }));
        }
      }
    }

    else if (action === 'unsubscribe') {
      if (typeof slot === 'number' && slot >= 0 && slot < MAX_SLOTS) {
        subscribers.get(slot).delete(ws);
        ws.send(JSON.stringify({ event: 'unsubscribed', slot }));
      }
    }

    else if (action === 'attach') {
      if (typeof slot !== 'number' || slot < 0 || slot >= MAX_SLOTS) {
        ws.send(JSON.stringify({ event: 'error', msg: 'slot required' }));
        return;
      }
      // Try agentOS inject endpoint; ignore if not implemented
      try {
        await agentosPost(`/api/agentos/console/attach/${slot}`);
        ws.send(JSON.stringify({ event: 'attached', slot }));
      } catch {
        ws.send(JSON.stringify({ event: 'attached', slot })); // still confirm
      }
    }

    else if (action === 'input') {
      if (typeof slot !== 'number' || slot < 0 || slot >= MAX_SLOTS) return;
      if (typeof msg.data !== 'string') return;
      // Write directly to QEMU serial socket — goes to whichever PD/VM has the
      // active console (typically the Linux VM at the buildroot login prompt).
      serialWrite(msg.data);
    }

    else if (action === 'list') {
      const data = [];
      for (let s = 0; s < MAX_SLOTS; s++) {
        data.push({
          slot: s,
          name: SLOT_NAMES[s] ?? 'unknown',
          lines: slotHistory.get(s)?.length ?? 0,
          active: activeSlots.has(s),
          subscribers: subscribers.get(s)?.size ?? 0,
        });
      }
      ws.send(JSON.stringify({ event: 'slots', data }));
    }

    else {
      ws.send(JSON.stringify({ event: 'error', msg: `unknown action: ${action}` }));
    }
  });

  ws.on('close', () => {
    for (let s = 0; s < MAX_SLOTS; s++) {
      subscribers.get(s).delete(ws);
    }
    console.log(`[console-ws] client disconnected: ${clientIp}`);
  });

  ws.on('error', (e) => {
    console.error(`[console-ws] client error: ${e.message}`);
  });
});

server.listen(WS_PORT, () => {
  console.log(`[console-ws] agentOS console bridge listening on ws://0.0.0.0:${WS_PORT}`);
  console.log(`[console-ws] dashboard:    http://127.0.0.1:${WS_PORT}/`);
  console.log(`[console-ws] health:       http://127.0.0.1:${WS_PORT}/health`);
  console.log(`[console-ws] serial sock:  ${SERIAL_SOCK} (bidirectional, retry on disconnect)`);
  console.log(`[console-ws] agentOS API:  ${AGENTOS_BASE} (optional, polled on demand)`);
});

process.on('SIGTERM', () => { server.close(); process.exit(0); });
process.on('SIGINT',  () => { server.close(); process.exit(0); });
