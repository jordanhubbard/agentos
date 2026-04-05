#!/usr/bin/env node
/**
 * agentOS Console WebSocket Bridge
 *
 * Bridges console_mux per-PD output rings to browser xterm.js clients.
 * Each client subscribes to one or more slots (PDs) and receives live output.
 * Input typed in the browser terminal is forwarded to the inject endpoint.
 *
 * Protocol (client → server):
 *   {"action":"subscribe","slot":2}           — start receiving output from slot 2
 *   {"action":"unsubscribe","slot":2}         — stop
 *   {"action":"attach","slot":2}              — POST attach to agentOS
 *   {"action":"input","slot":2,"data":"..."}  — inject keystrokes into slot 2
 *   {"action":"list"}                         — get all slots with activity counts
 *
 * Protocol (server → client):
 *   {"slot":2,"line":"[vibe_engine] WASM slot 0 ready\n"}
 *   {"event":"attached","slot":2}
 *   {"event":"slots","data":[{"slot":0,"lines":42},…]}
 *   {"event":"error","msg":"..."}
 *
 * Listens on port 8795.
 * Polls agentOS GET /api/agentos/console/:slot every 50ms per subscribed slot.
 */

import { WebSocketServer } from 'ws';
import http from 'http';
import fs from 'fs';
import { fileURLToPath } from 'url';
import nodePath from 'path';

const __dirname = nodePath.dirname(fileURLToPath(import.meta.url));

const WS_PORT       = 8795;
const POLL_MS       = 50;
const AGENTOS_BASE  = 'http://127.0.0.1:8789';
const AGENTOS_TOKEN = 'agentos-console-20maaghccmbmnby63so';
const MAX_SLOTS     = 16;

const SLOT_NAMES = [
  'monitor', 'init_agent', 'event_bus', 'agentfs',
  'cap_broker', 'cap_audit_log', 'agent_pool/worker', 'vibe_engine',
  'mem_profiler', 'watchdog', 'trace_recorder', 'cap_policy',
  'debug_bridge', 'fault_handler', 'quota_pd', 'misc',
];

// ─── Profiler mock data ──────────────────────────────────────────────────────

function generateProfilerSnapshot() {
  const j = (base, range = 200) =>
    Math.max(0, base + Math.floor((Math.random() - 0.5) * range));

  return {
    ts: Date.now(),
    slots: [
      {
        id: 0, name: 'inference_worker',
        cpu_pct: Math.min(99, Math.max(1, 42 + Math.floor(Math.random() * 10 - 5))),
        mem_kb:  j(8192, 512),
        ticks:   j(12450),
        frames: [
          { fn: 'matmul_f32',   ticks: j(5200), depth: 0 },
          { fn: 'softmax',      ticks: j(2100), depth: 1 },
          { fn: 'embed_lookup', ticks: j(1800), depth: 1 },
          { fn: 'layer_norm',   ticks: j(900),  depth: 2 },
          { fn: 'rms_norm',     ticks: j(620),  depth: 2 },
          { fn: 'rope_enc',     ticks: j(480),  depth: 3 },
        ],
      },
      {
        id: 1, name: 'event_handler',
        cpu_pct: Math.min(99, Math.max(1, 8 + Math.floor(Math.random() * 4 - 2))),
        mem_kb:  j(512, 64),
        ticks:   j(2340, 150),
        frames: [
          { fn: 'dispatch_event', ticks: j(1200, 100), depth: 0 },
          { fn: 'cap_check',      ticks: j(600,  80),  depth: 1 },
          { fn: 'ring_enqueue',   ticks: j(340,  60),  depth: 2 },
        ],
      },
      {
        id: 2, name: 'vibe_validator',
        cpu_pct: Math.min(99, Math.max(1, 15 + Math.floor(Math.random() * 6 - 3))),
        mem_kb:  j(2048, 256),
        ticks:   j(4110, 300),
        frames: [
          { fn: 'wasm_validate', ticks: j(2800, 200), depth: 0 },
          { fn: 'section_parse', ticks: j(1100, 100), depth: 1 },
          { fn: 'type_check',    ticks: j(540,  80),  depth: 2 },
        ],
      },
    ],
  };
}

// ─── State ──────────────────────────────────────────────────────────────────

/** Map<slot, Set<ws>> — clients subscribed to each slot */
const subscribers = new Map();
for (let i = 0; i < MAX_SLOTS; i++) subscribers.set(i, new Set());

/** Map<slot, lastSeenLineCount> — track how many lines we've already sent */
const slotCursor = new Map();
for (let i = 0; i < MAX_SLOTS; i++) slotCursor.set(i, 0);

/** Set<slot> — slots we're actively polling */
const activeSlots = new Set();

// ─── Helpers ────────────────────────────────────────────────────────────────

async function agentosGet(path) {
  const url = `${AGENTOS_BASE}${path}`;
  const res = await fetch(url, {
    headers: { Authorization: `Bearer ${AGENTOS_TOKEN}` },
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
  });
  if (!res.ok) throw new Error(`agentOS POST ${path} → ${res.status}`);
  return res.json();
}

function broadcast(slot, msg) {
  const str = JSON.stringify(msg);
  for (const ws of subscribers.get(slot) ?? []) {
    if (ws.readyState === 1 /* OPEN */) ws.send(str);
  }
}

// ─── Per-slot poller ─────────────────────────────────────────────────────────

async function pollSlot(slot) {
  if (!activeSlots.has(slot)) return;
  try {
    const data = await agentosGet(`/api/agentos/console/${slot}`);
    const lines = data.lines ?? [];
    const cursor = slotCursor.get(slot) ?? 0;
    if (lines.length > cursor) {
      const newLines = lines.slice(cursor);
      slotCursor.set(slot, lines.length);
      for (const line of newLines) {
        broadcast(slot, { slot, line });
      }
    }
  } catch (e) {
    // agentOS unreachable — retry next tick
  }
  if (activeSlots.has(slot)) {
    setTimeout(() => pollSlot(slot), POLL_MS);
  }
}

function startPolling(slot) {
  if (!activeSlots.has(slot)) {
    activeSlots.add(slot);
    pollSlot(slot);
    console.log(`[console-ws] started polling slot ${slot} (${SLOT_NAMES[slot] ?? '?'})`);
  }
}

function maybeStopPolling(slot) {
  if ((subscribers.get(slot)?.size ?? 0) === 0) {
    activeSlots.delete(slot);
    console.log(`[console-ws] stopped polling slot ${slot} — no subscribers`);
  }
}

// ─── WebSocket server ────────────────────────────────────────────────────────

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
      activeSlots: [...activeSlots],
      subscriberCounts: Object.fromEntries(
        [...subscribers.entries()].map(([k, v]) => [k, v.size])
      ),
    }));

  // ── Profiler snapshot ─────────────────────────────────────────────
  } else if (pathname === '/api/agentos/profiler/snapshot') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(generateProfilerSnapshot()));

  // ── Slot list (proxy agentOS or fall back to mock) ───────────────────
  } else if (pathname === '/api/agentos/slots') {
    try {
      const data = await agentosGet('/api/agentos/slots');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(data));
    } catch {
      const mock = { slots: SLOT_NAMES.map((name, id) => ({ id, name, active: id < 12 })) };
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(mock));
    }

  } else {
    res.writeHead(404);
    res.end();
  }
});

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
      startPolling(slot);
      ws.send(JSON.stringify({ event: 'subscribed', slot, name: SLOT_NAMES[slot] ?? 'unknown' }));
    }

    else if (action === 'unsubscribe') {
      if (typeof slot === 'number' && slot >= 0 && slot < MAX_SLOTS) {
        subscribers.get(slot).delete(ws);
        maybeStopPolling(slot);
        ws.send(JSON.stringify({ event: 'unsubscribed', slot }));
      }
    }

    else if (action === 'attach') {
      if (typeof slot !== 'number' || slot < 0 || slot >= MAX_SLOTS) {
        ws.send(JSON.stringify({ event: 'error', msg: 'slot required' }));
        return;
      }
      try {
        await agentosPost(`/api/agentos/console/attach/${slot}`);
        ws.send(JSON.stringify({ event: 'attached', slot }));
      } catch (e) {
        ws.send(JSON.stringify({ event: 'error', msg: `attach failed: ${e.message}` }));
      }
    }

    else if (action === 'input') {
      if (typeof slot !== 'number' || slot < 0 || slot >= MAX_SLOTS) return;
      if (typeof msg.data !== 'string') return;
      try {
        await agentosPost(`/api/agentos/console/inject/${slot}`, { data: msg.data });
      } catch {
        // inject endpoint may not be implemented yet — silently ignore
      }
    }

    else if (action === 'list') {
      const data = [];
      for (let s = 0; s < MAX_SLOTS; s++) {
        data.push({
          slot: s,
          name: SLOT_NAMES[s] ?? 'unknown',
          lines: slotCursor.get(s) ?? 0,
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
    // Clean up all subscriptions for this client
    for (let s = 0; s < MAX_SLOTS; s++) {
      if (subscribers.get(s).delete(ws)) {
        maybeStopPolling(s);
      }
    }
    console.log(`[console-ws] client disconnected: ${clientIp}`);
  });

  ws.on('error', (e) => {
    console.error(`[console-ws] client error: ${e.message}`);
  });
});

server.listen(WS_PORT, () => {
  console.log(`[console-ws] agentOS console bridge listening on ws://0.0.0.0:${WS_PORT}`);
  console.log(`[console-ws] dashboard: http://127.0.0.1:${WS_PORT}/`);
  console.log(`[console-ws] health:    http://127.0.0.1:${WS_PORT}/health`);
  console.log(`[console-ws] profiler:  http://127.0.0.1:${WS_PORT}/api/agentos/profiler/snapshot`);
  console.log(`[console-ws] agentOS:   ${AGENTOS_BASE}`);
});

process.on('SIGTERM', () => { server.close(); process.exit(0); });
process.on('SIGINT',  () => { server.close(); process.exit(0); });
