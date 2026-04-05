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
import fs from 'fs';
import { fileURLToPath } from 'url';
import nodePath from 'path';

const __dirname = nodePath.dirname(fileURLToPath(import.meta.url));

const WS_PORT       = 8795;
const SERIAL_LOG    = '/tmp/agentos-serial.log';
const SERIAL_POLL_MS = 100;
const AGENTOS_BASE  = 'http://127.0.0.1:8789';
const AGENTOS_TOKEN = 'agentos-console-20maaghccmbmnby63so';
const MAX_SLOTS     = 16;
const MAX_HISTORY   = 2000;  // lines kept per slot for late-joining clients

const SLOT_NAMES = [
  'monitor', 'init_agent', 'event_bus', 'agentfs',
  'cap_broker', 'cap_audit_log', 'agent_pool/worker', 'vibe_engine',
  'mem_profiler', 'watchdog', 'trace_recorder', 'cap_policy',
  'debug_bridge', 'fault_handler', 'quota_pd', 'misc',
];

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
  // Linux VMM and kernel boot → slot 15 (misc)
  { slot: 15, prefixes: ['linux_vmm|', '[    ', 'Starting ', 'Welcome to',
                          'buildroot login', 'Starting syslogd', 'Starting klogd',
                          'Running sysctl', 'Saving random seed', 'OK'] },
];

function lineToSlot(line) {
  for (const { slot, prefixes } of SERIAL_ROUTES) {
    for (const pfx of prefixes) {
      if (line.startsWith(pfx) || line.includes(pfx)) return slot;
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

// Serial file reader state
let serialPos    = 0;
let serialBuffer = '';

// ─── Serial reader ───────────────────────────────────────────────────────────

function parseSerialLog() {
  let stat;
  try { stat = fs.statSync(SERIAL_LOG); } catch { return; }
  if (stat.size <= serialPos) return;

  let fd;
  try {
    fd = fs.openSync(SERIAL_LOG, 'r');
    const chunk = Buffer.alloc(stat.size - serialPos);
    fs.readSync(fd, chunk, 0, chunk.length, serialPos);
    serialPos = stat.size;
    serialBuffer += chunk.toString('utf-8');
  } catch { return; }
  finally { if (fd !== undefined) try { fs.closeSync(fd); } catch {} }

  // Process complete lines only
  const lastNL = serialBuffer.lastIndexOf('\n');
  if (lastNL < 0) return;

  const lines = serialBuffer.slice(0, lastNL).split('\n');
  serialBuffer = serialBuffer.slice(lastNL + 1);

  for (const rawLine of lines) {
    if (!rawLine) continue;
    const slot = lineToSlot(rawLine);
    const hist = slotHistory.get(slot);
    hist.push(rawLine);
    if (hist.length > MAX_HISTORY) hist.shift();
    activeSlots.add(slot);
    broadcast(slot, { slot, line: rawLine });
  }
}

setInterval(parseSerialLog, SERIAL_POLL_MS);

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
      serialPos,
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
      try {
        await agentosPost(`/api/agentos/console/inject/${slot}`, { data: msg.data });
      } catch {
        // inject endpoint not yet implemented — silently ignore
      }
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
  console.log(`[console-ws] serial log:   ${SERIAL_LOG}`);
  console.log(`[console-ws] agentOS API:  ${AGENTOS_BASE} (optional, polled on demand)`);
});

process.on('SIGTERM', () => { server.close(); process.exit(0); });
process.on('SIGINT',  () => { server.close(); process.exit(0); });
