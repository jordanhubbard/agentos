/**
 * agentOS Console — API integration tests
 *
 * Requires a running server on CONSOLE_PORT (default 8080) and bridge on
 * BRIDGE_PORT (default 8790).  Start with `make console` or `npm start`
 * before running these tests.
 *
 * Run:  node --test console/test/api.test.mjs
 *   or: npm test
 */

import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';

const CONSOLE_PORT = Number(process.env.CONSOLE_PORT || 8080);
const BRIDGE_PORT  = Number(process.env.BRIDGE_PORT  || 8790);
const BASE         = `http://127.0.0.1:${CONSOLE_PORT}`;
const BRIDGE       = `http://127.0.0.1:${BRIDGE_PORT}`;
const WS_BASE      = `ws://127.0.0.1:${CONSOLE_PORT}`;

// ─── helpers ─────────────────────────────────────────────────────────────────

async function GET(url)         { return fetch(url); }
async function POST(url, body)  { return fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body) }); }

async function getJSON(url) {
  const res = await GET(url);
  assert.equal(res.status, 200, `GET ${url} → ${res.status}`);
  return res.json();
}

function wsConnect(path) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`${WS_BASE}${path}`);
    ws.once('open',  () => resolve(ws));
    ws.once('error', reject);
    setTimeout(() => reject(new Error(`WS timeout: ${path}`)), 4000);
  });
}

function wsNextMessage(ws, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('WS message timeout')), timeoutMs);
    ws.once('message', data => { clearTimeout(t); resolve(data.toString()); });
  });
}

// ─── Preflight: verify servers are up ────────────────────────────────────────

before(async () => {
  try {
    await fetch(`${BASE}/health`);
    await fetch(`${BRIDGE}/api/agentos/slots`);
  } catch (e) {
    throw new Error(`Servers not reachable — start with \`npm start\` before running tests. (${e.message})`);
  }
});

// ─── 1. Console server static routes ─────────────────────────────────────────

describe('console server — static & health', () => {
  it('GET / returns HTML dashboard', async () => {
    const res = await GET(`${BASE}/`);
    assert.equal(res.status, 200);
    const ct = res.headers.get('content-type') ?? '';
    assert.ok(ct.includes('text/html'), `content-type: ${ct}`);
    const body = await res.text();
    assert.ok(body.includes('agentOS Console'), 'title missing');
    assert.ok(body.includes('xterm'), 'xterm.js not referenced');
  });

  it('GET /health returns JSON with ok:true and subscriber counts', async () => {
    const data = await getJSON(`${BASE}/health`);
    assert.equal(data.ok, true);
    assert.ok(typeof data.subscriberCounts === 'object', 'subscriberCounts missing');
    assert.ok(Array.isArray(data.activeSlots), 'activeSlots must be array');
  });
});

// ─── 2. Console server proxy routes ──────────────────────────────────────────

describe('console server — proxy routes', () => {
  it('GET /api/agentos/slots returns slot list', async () => {
    const data = await getJSON(`${BASE}/api/agentos/slots`);
    assert.ok(Array.isArray(data.slots), 'slots must be array');
    assert.equal(data.slots.length, 16, 'must have exactly 16 slots');
    const s0 = data.slots[0];
    assert.ok('id' in s0 && 'name' in s0 && 'active' in s0 && 'lines' in s0, 'slot schema');
    assert.equal(s0.id, 0);
    assert.equal(s0.name, 'controller');
  });

  it('GET /api/agentos/agents returns agent list with required types', async () => {
    const data = await getJSON(`${BASE}/api/agentos/agents`);
    assert.ok(Array.isArray(data.agents), 'agents must be array');
    const types = data.agents.map(a => a.type);
    assert.ok(types.includes('linux_vm'),   'must have linux_vm agent');
    assert.ok(types.includes('freebsd_vm'), 'must have freebsd_vm agent');
    assert.ok(types.includes('wasm'),       'must have wasm agents');

    const linuxAgent = data.agents.find(a => a.type === 'linux_vm');
    assert.ok('id' in linuxAgent && 'slot' in linuxAgent && 'status' in linuxAgent, 'linux_vm schema');
    assert.equal(linuxAgent.slot, 14);

    const fbAgent = data.agents.find(a => a.type === 'freebsd_vm');
    assert.ok('freebsdState' in fbAgent, 'freebsd_vm must include freebsdState');
    assert.ok(['idle','preparing','downloading','building','ready','error'].includes(fbAgent.freebsdState.phase),
      `invalid freebsd phase: ${fbAgent.freebsdState.phase}`);

    const wasmAgents = data.agents.filter(a => a.type === 'wasm');
    assert.equal(wasmAgents.length, 4, 'must have exactly 4 wasm agents');
    wasmAgents.forEach((a, i) => {
      assert.equal(a.slot, 9 + i, `wasm agent ${i} must be slot ${9+i}`);
    });
  });

  it('GET /api/agentos/agents/freebsd/status returns state machine', async () => {
    const data = await getJSON(`${BASE}/api/agentos/agents/freebsd/status`);
    assert.ok('phase' in data && 'progress' in data && 'assetsReady' in data, 'freebsd status schema');
    assert.ok(typeof data.progress === 'number' && data.progress >= 0 && data.progress <= 100);
    assert.ok(typeof data.assetsReady === 'boolean');
  });

  it('GET /api/debug returns consoleSamples for slots 0-2', async () => {
    const data = await getJSON(`${BASE}/api/debug`);
    assert.ok('agentosBase' in data, 'agentosBase missing');
    assert.ok('consoleSamples' in data, 'consoleSamples missing');
    assert.ok('slots' in data, 'slots missing');
    const s0 = data.consoleSamples['0'];
    assert.ok(typeof s0 === 'object', 'consoleSamples[0] must be object');
    assert.ok('ok' in s0, 'consoleSamples[0].ok missing');
  });
});

// ─── 3. Bridge server routes (port 8790) ─────────────────────────────────────

describe('bridge server (port 8790) — direct', () => {
  it('GET /api/agentos/slots mirrors console proxy', async () => {
    const [proxy, direct] = await Promise.all([
      getJSON(`${BASE}/api/agentos/slots`),
      getJSON(`${BRIDGE}/api/agentos/slots`),
    ]);
    assert.equal(proxy.slots.length, direct.slots.length);
    // source field on direct bridge response
    assert.ok(['serial_log','live'].includes(direct.source), `unexpected source: ${direct.source}`);
  });

  it('GET /api/agentos/console/:slot returns paginated lines', async () => {
    const data = await getJSON(`${BRIDGE}/api/agentos/console/0?cursor=0`);
    assert.equal(data.slot, 0);
    assert.ok(typeof data.cursor === 'number');
    assert.ok(Array.isArray(data.lines), 'lines must be array');
    // cursor must advance by number of lines returned
    assert.equal(data.cursor, data.lines.length, 'cursor = initial 0 + lines.length');
  });

  it('GET /api/agentos/console/:slot with cursor skips already-seen lines', async () => {
    const first  = await getJSON(`${BRIDGE}/api/agentos/console/0?cursor=0`);
    const second = await getJSON(`${BRIDGE}/api/agentos/console/0?cursor=${first.cursor}`);
    assert.equal(second.lines.length, 0, 'no new lines on re-fetch at same cursor');
    assert.equal(second.cursor, first.cursor);
  });

  it('GET /api/agentos/console/:slot with invalid slot returns 400', async () => {
    const res = await GET(`${BRIDGE}/api/agentos/console/99?cursor=0`);
    assert.equal(res.status, 400);
    const body = await res.json();
    assert.ok('error' in body);
  });

  it('POST /api/agentos/console/attach/:slot returns ok', async () => {
    const res = await POST(`${BRIDGE}/api/agentos/console/attach/0`, {});
    assert.equal(res.status, 200);
    const data = await res.json();
    assert.equal(data.ok, true);
    assert.equal(data.slot, 0);
  });

  it('POST /api/agentos/console/inject/:slot responds (ok or no_socket)', async () => {
    const res = await POST(`${BRIDGE}/api/agentos/console/inject/14`, { data: '' });
    assert.equal(res.status, 200);
    const data = await res.json();
    // Either ok (socket connected) or no_socket (QEMU not running) — both valid
    assert.ok('ok' in data, 'response must have ok field');
    assert.ok('source' in data, 'response must have source field');
    assert.ok(['socket','no_socket'].includes(data.source), `unexpected source: ${data.source}`);
  });

  it('GET /api/agentos/console/status returns active_pd', async () => {
    const data = await getJSON(`${BRIDGE}/api/agentos/console/status`);
    assert.ok('active_pd' in data);
    assert.ok('mode' in data);
  });

  it('GET unknown bridge route returns 404', async () => {
    const res = await GET(`${BRIDGE}/api/agentos/nonexistent`);
    assert.equal(res.status, 404);
  });
});

// ─── 4. Spawn endpoint ───────────────────────────────────────────────────────

describe('spawn endpoint', () => {
  it('POST spawn freebsd_vm queues or confirms ready', async () => {
    const res = await POST(`${BASE}/api/agentos/agents/spawn`, { type: 'freebsd_vm' });
    assert.ok([200, 202].includes(res.status), `unexpected status ${res.status}`);
    const data = await res.json();
    assert.equal(data.ok, true);
    assert.ok(typeof data.message === 'string' && data.message.length > 0);
  });

  it('POST spawn wasm returns queued acknowledgement', async () => {
    const res = await POST(`${BASE}/api/agentos/agents/spawn`, { type: 'wasm', slot: 9, name: 'echo.wasm' });
    assert.equal(res.status, 202);
    const data = await res.json();
    assert.equal(data.ok, true);
    assert.equal(data.queued, true);
  });
});

// ─── 5. WebSocket — terminal tiles ───────────────────────────────────────────

describe('WebSocket — terminal (/terminal/:slot)', () => {
  it('connects to /terminal/0 and receives initial cache flush', async () => {
    const ws = await wsConnect('/terminal/0');
    try {
      // Server must send cached lines immediately on connect
      const msg = await wsNextMessage(ws, 3000);
      assert.ok(typeof msg === 'string' && msg.length > 0, 'expected non-empty initial flush');
      // Controller output should be present
      assert.ok(msg.includes('controller') || msg.includes('[boot]'), `unexpected flush: ${msg.slice(0,80)}`);
    } finally {
      ws.close();
    }
  });

  it('connects to /terminal/14 (linux_vmm) and receives Linux boot lines', async () => {
    const ws = await wsConnect('/terminal/14');
    try {
      const msg = await wsNextMessage(ws, 3000);
      assert.ok(msg.length > 0, 'expected cached linux_vmm output');
    } finally {
      ws.close();
    }
  });

  it('rejects invalid slot /terminal/99 with close code 1008', async () => {
    await new Promise((resolve, reject) => {
      const ws = new WebSocket(`${WS_BASE}/terminal/99`);
      ws.on('close', (code) => {
        assert.equal(code, 1008, `expected 1008, got ${code}`);
        resolve();
      });
      ws.on('error', reject);
      setTimeout(() => reject(new Error('no close event')), 3000);
    });
  });

  it('forwards keystroke to inject endpoint (input round-trip)', async () => {
    // Inject a no-op NUL byte — just verify the path doesn't crash
    const ws = await wsConnect('/terminal/14');
    try {
      await wsNextMessage(ws, 3000); // wait for initial flush
      ws.send('\x00'); // NUL — harmless to Linux TTY
      // Give server 500ms to process, then check inject endpoint still responds
      await new Promise(r => setTimeout(r, 500));
      const res = await POST(`${BRIDGE}/api/agentos/console/inject/14`, { data: '' });
      assert.equal(res.status, 200);
    } finally {
      ws.close();
    }
  });
});

// ─── 6. WebSocket — log-bus ──────────────────────────────────────────────────

describe('WebSocket — log-bus (/)', () => {
  it('connects and responds to list action', async () => {
    const ws = await wsConnect('/');
    try {
      ws.send(JSON.stringify({ action: 'list' }));
      const raw  = await wsNextMessage(ws, 3000);
      const msg  = JSON.parse(raw);
      assert.equal(msg.event, 'slots');
      assert.ok(Array.isArray(msg.data));
      assert.equal(msg.data.length, 16);
    } finally {
      ws.close();
    }
  });

  it('subscribe to slot 0 and receives attached event', async () => {
    const ws = await wsConnect('/');
    try {
      ws.send(JSON.stringify({ action: 'subscribe', slot: 0 }));
      const raw = await wsNextMessage(ws, 3000);
      const msg = JSON.parse(raw);
      assert.equal(msg.event, 'subscribed');
      assert.equal(msg.slot, 0);
    } finally {
      ws.close();
    }
  });

  it('unsubscribe from slot 0 receives unsubscribed event', async () => {
    const ws = await wsConnect('/');
    try {
      ws.send(JSON.stringify({ action: 'subscribe',   slot: 0 }));
      await wsNextMessage(ws, 2000);
      ws.send(JSON.stringify({ action: 'unsubscribe', slot: 0 }));
      const raw = await wsNextMessage(ws, 2000);
      const msg = JSON.parse(raw);
      assert.equal(msg.event, 'unsubscribed');
      assert.equal(msg.slot, 0);
    } finally {
      ws.close();
    }
  });

  it('unknown action receives error event', async () => {
    const ws = await wsConnect('/');
    try {
      ws.send(JSON.stringify({ action: 'bogus_action' }));
      const raw = await wsNextMessage(ws, 2000);
      const msg = JSON.parse(raw);
      assert.equal(msg.event, 'error');
      assert.ok(msg.msg.includes('bogus_action'));
    } finally {
      ws.close();
    }
  });
});

// ─── 7. Agent data shape contracts ───────────────────────────────────────────

describe('agent data shape contracts', () => {
  let agents;
  before(async () => {
    const data = await getJSON(`${BASE}/api/agentos/agents`);
    agents = data.agents;
  });

  it('every agent has required fields', () => {
    const required = ['id','type','slot','name','status','lines'];
    agents.forEach(a => {
      required.forEach(f => assert.ok(f in a, `agent ${a.id ?? '?'} missing field '${f}'`));
    });
  });

  it('status values are from the known enum', () => {
    const valid = new Set(['running','booting','stopped','ready','not_prepared',
                           'preparing','downloading','building','error',
                           'failed','idle','empty']);
    agents.forEach(a => {
      assert.ok(valid.has(a.status), `agent ${a.name} has unknown status '${a.status}'`);
    });
  });

  it('linux_vm agent slot is 14', () => {
    const lvm = agents.find(a => a.type === 'linux_vm');
    assert.ok(lvm, 'linux_vm not found');
    assert.equal(lvm.slot, 14);
  });

  it('wasm agent slots are contiguous 9–12', () => {
    const wasm = agents.filter(a => a.type === 'wasm').map(a => a.slot).sort((a,b)=>a-b);
    assert.deepEqual(wasm, [9, 10, 11, 12]);
  });

  it('slot list slot names match SLOT_NAMES canonical order', async () => {
    const data = await getJSON(`${BASE}/api/agentos/slots`);
    const EXPECTED = [
      'controller','event_bus','init_agent','agentfs',
      'vibe_engine','worker_0','worker_1','worker_2',
      'worker_3','swap_slot_0','swap_slot_1','swap_slot_2',
      'swap_slot_3','console_mux','linux_vmm','fault_hndlr',
    ];
    data.slots.forEach((s, i) => {
      assert.equal(s.name, EXPECTED[i], `slot ${i} name mismatch`);
    });
  });
});
