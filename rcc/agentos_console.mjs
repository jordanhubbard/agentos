#!/usr/bin/env node
/**
 * agentOS Console WebSocket Bridge
 *
 * Bridges console_mux per-PD output rings to browser WebSocket clients.
 * Each client subscribes to a slot (PD) and receives live log lines.
 *
 * Protocol (client → server):
 *   {"action":"subscribe","slot":2}    — start receiving lines from slot 2
 *   {"action":"unsubscribe","slot":2}  — stop
 *   {"action":"attach","slot":2}       — also POST attach to RCC
 *   {"action":"list"}                  — get all slots with activity counts
 *
 * Protocol (server → client):
 *   {"slot":2,"line":"[vibe_engine] WASM slot 0 ready\n"}
 *   {"event":"attached","slot":2}
 *   {"event":"slots","data":[{"slot":0,"lines":42},…]}
 *   {"event":"error","msg":"..."}
 *
 * Listens on WS port 8795.
 * Polls RCC GET /api/agentos/console/:slot every 100ms per subscribed slot.
 */

import { WebSocketServer } from 'ws';
import http from 'http';

const WS_PORT    = 8795;
const POLL_MS    = 100;
const RCC_BASE   = 'http://127.0.0.1:8789';
const RCC_TOKEN  = 'rcc-agent-rocky-20maaghccmbmnby63so';
const MAX_SLOTS  = 16;

const SLOT_NAMES = [
  'monitor', 'init_agent', 'event_bus', 'agentfs',
  'cap_broker', 'cap_audit_log', 'agent_pool/worker', 'vibe_engine',
  'mem_profiler', 'watchdog', 'trace_recorder', 'cap_policy',
  'debug_bridge', 'fault_handler', 'quota_pd', 'misc',
];

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

async function rccGet(path) {
  const url = `${RCC_BASE}${path}`;
  const res = await fetch(url, {
    headers: { Authorization: `Bearer ${RCC_TOKEN}` },
  });
  if (!res.ok) throw new Error(`RCC ${path} → ${res.status}`);
  return res.json();
}

async function rccPost(path, body = {}) {
  const url = `${RCC_BASE}${path}`;
  const res = await fetch(url, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${RCC_TOKEN}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`RCC POST ${path} → ${res.status}`);
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
    const data = await rccGet(`/api/agentos/console/${slot}`);
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
    // RCC unreachable — retry next tick
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

const server = http.createServer((req, res) => {
  // Simple health check at /health
  if (req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      ok: true,
      activeSlots: [...activeSlots],
      subscriberCounts: Object.fromEntries(
        [...subscribers.entries()].map(([k, v]) => [k, v.size])
      ),
    }));
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
        await rccPost(`/api/agentos/console/attach/${slot}`);
        ws.send(JSON.stringify({ event: 'attached', slot }));
      } catch (e) {
        ws.send(JSON.stringify({ event: 'error', msg: `attach failed: ${e.message}` }));
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
  console.log(`[console-ws] health: http://127.0.0.1:${WS_PORT}/health`);
  console.log(`[console-ws] RCC: ${RCC_BASE}`);
});

process.on('SIGTERM', () => { server.close(); process.exit(0); });
process.on('SIGINT',  () => { server.close(); process.exit(0); });
