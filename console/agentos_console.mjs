#!/usr/bin/env node
/**
 * agentOS Console Server
 *
 * Serves dashboard.html and bridges browser clients to the agentOS API.
 *
 * HTTP routes:
 *   GET  /              → dashboard.html
 *   GET  /health        → JSON health check
 *   GET  /api/agentos/profiler/snapshot → mock profiler data
 *   GET  /api/agentos/slots             → slot list (proxy or mock)
 *
 * WebSocket routes:
 *   ws://.../           → log subscription bus (subscribe/unsubscribe per slot)
 *   ws://.../terminal   → raw terminal I/O for slot 0
 *   ws://.../terminal/N → raw terminal I/O for slot N
 *
 * Log subscription protocol (client → server):
 *   {"action":"subscribe","slot":2}    — start receiving lines from slot 2
 *   {"action":"unsubscribe","slot":2}  — stop
 *   {"action":"attach","slot":2}       — also POST attach to agentOS
 *   {"action":"list"}                  — get all slots with activity counts
 *
 * Log subscription protocol (server → client):
 *   {"slot":2,"line":"...\n"}
 *   {"event":"attached","slot":2}
 *   {"event":"slots","data":[...]}
 *   {"event":"error","msg":"..."}
 *
 * Terminal protocol:
 *   server → client : raw UTF-8 terminal output (VT100/xterm)
 *   client → server : raw UTF-8 keyboard input
 */

import { WebSocketServer } from 'ws';
import http from 'http';
import https from 'https';
import fs from 'fs';
import net from 'net';
import { execSync } from 'child_process';
import { spawn as spawnProc } from 'child_process';
import { fileURLToPath } from 'url';
import nodePath from 'path';

const __dirname = nodePath.dirname(fileURLToPath(import.meta.url));

const PORT      = Number(process.env.CONSOLE_PORT || 8080);
const POLL_MS   = 50;
// AGENTOS_API_PORT (8789) is reserved for QEMU hostfwd → Linux guest bridge.
// BRIDGE_PORT (8790) is the local stub bridge served by this process.
const BRIDGE_PORT   = Number(process.env.BRIDGE_PORT || 8790);
const AGENTOS_BASE  = `http://${process.env.AGENTOS_HOST || '127.0.0.1'}:${process.env.AGENTOS_API_PORT || BRIDGE_PORT}`;
const AGENTOS_TOKEN = process.env.AGENTOS_TOKEN || 'agentos-console-20maaghccmbmnby63so';
const MAX_SLOTS = 16;

// Slot names match console_mux.c pd_names[] table (slot = pd_id)
const SLOT_NAMES = [
  'controller', 'event_bus', 'init_agent', 'agentfs',
  'vibe_engine', 'worker_0', 'worker_1', 'worker_2',
  'worker_3', 'swap_slot_0', 'swap_slot_1', 'swap_slot_2',
  'swap_slot_3', 'console_mux', 'linux_vmm', 'fault_hndlr',
];

// Map PD name → slot id (reverse of SLOT_NAMES, plus aliases)
const PD_NAME_TO_SLOT = Object.fromEntries(SLOT_NAMES.map((n, i) => [n, i]));
// Aliases seen in raw serial log output
Object.assign(PD_NAME_TO_SLOT, {
  'monitor': 0, 'event_bus': 1, 'eventbus': 1,
  'init_agent': 2, 'initagent': 2,
  'agentfs': 3, 'vibe_engine': 4, 'vibeengine': 4,
  'boot': 0,       // agentos_log_boot: "[boot] protection domain: X starting"
  'worker': 5,     // generic [worker] tag → worker_0 slot
  'swap_slot': 9,  // generic swap_slot tag → swap_slot_0
  'linux_vmm': 14, // linux_vmm already in SLOT_NAMES, alias for pipe-style logs
});

// ─── Serial log parser (fallback when HTTP API is unreachable) ───────────────
//
// Parses /tmp/agentos-serial.log (QEMU -serial file:...) to reconstruct
// per-PD output streams.  console_mux tags each line in broadcast mode:
//   \033[36m[pd_name]\033[0m text\n   (ANSI coloured)
// Some PDs also use microkit_dbg_puts directly:
//   [pd_name] text\n
//
// We strip ANSI, extract the tag, look up the slot id, and store lines.

const SERIAL_LOG  = process.env.SERIAL_LOG  || '/tmp/agentos-serial.log';
const SERIAL_SOCK = process.env.SERIAL_SOCK || '/tmp/agentos-serial.sock';

/** Per-slot line cache populated from the serial log */
const serialCache = {
  /** slot → string[] of complete lines */
  lines:    Array.from({ length: MAX_SLOTS }, () => []),
  /** last byte-offset we read in the log file */
  offset:   0,
  /** whether we have ever successfully read the log */
  everRead: false,
};

// Strip all ANSI escape sequences from a string
function stripAnsi(s) {
  return s.replace(/\x1b\[[0-9;]*[mGKHJA-Za-z]/g, '');
}

/**
 * Incrementally read new bytes from the serial log and parse tagged lines.
 * Returns the number of new lines added across all slots.
 */
function parseSerialLog() {
  let fd;
  try {
    fd = fs.openSync(SERIAL_LOG, 'r');
  } catch {
    return 0;  // log doesn't exist yet
  }

  try {
    const stat = fs.fstatSync(fd);
    const fileSize = stat.size;

    // If the file shrank (QEMU restarted), reset
    if (fileSize < serialCache.offset) {
      serialCache.offset = 0;
      for (let i = 0; i < MAX_SLOTS; i++) serialCache.lines[i] = [];
    }

    if (fileSize <= serialCache.offset) return 0;

    const chunkSize = fileSize - serialCache.offset;
    const buf = Buffer.allocUnsafe(chunkSize);
    fs.readSync(fd, buf, 0, chunkSize, serialCache.offset);
    serialCache.offset = fileSize;
    serialCache.everRead = true;

    const newText = buf.toString('utf8');
    const rawLines = newText.split('\n');
    let added = 0;

    // Tagged line patterns (both ANSI and plain):
    //   \033[36m[pd_name]\033[0m text       — console_mux broadcast mode
    //   [pd_name] text                       — microkit_dbg_puts style
    //   pd_name|INFO: text                   — linux_vmm / microkit INFO style
    const BRACKET_RE = /(?:\x1b\[[0-9;]*m)?\[([a-zA-Z0-9_]+)\](?:\x1b\[[0-9;]*m)?\s*(.*)/;
    const PIPE_RE    = /^([a-zA-Z0-9_]+)\|(?:INFO|WARN|ERROR|DEBUG):\s*(.*)/;

    // Prefixes that are seL4 system / bootloader output — never route to linux_vmm
    const SYSPREFIX_RE = /^(LDR\||MON\||MICROKIT\||Bootstrapping|seL4 |ELF |CAPDL )/;
    let seenPdLine = serialCache.offset > chunkSize; // true if we've parsed PD lines in prior chunks

    for (const rawLine of rawLines) {
      const clean = stripAnsi(rawLine).trim();
      if (!clean) continue;

      let pdName = null, text = null;

      const bm = BRACKET_RE.exec(rawLine.trim());
      if (bm) {
        pdName = bm[1].toLowerCase();
        text   = stripAnsi(bm[2]).trimEnd();
        seenPdLine = true;
      } else {
        const pm = PIPE_RE.exec(clean);
        if (pm) {
          pdName = pm[1].toLowerCase();
          text   = pm[2].trimEnd();
          seenPdLine = true;
        }
      }

      if (pdName && text !== null) {
        const normalised = pdName.replace(/[\s-]/g, '_');
        const slot = PD_NAME_TO_SLOT[normalised] ?? PD_NAME_TO_SLOT[pdName] ?? -1;
        if (slot >= 0 && slot < MAX_SLOTS) {
          const line = `[${normalised}] ${text}`;
          serialCache.lines[slot].push(line);
          added++;
          broadcast(slot, { slot, line: line + '\n' });
        }
      } else if (seenPdLine && !SYSPREFIX_RE.test(clean)) {
        // Untagged line after seL4 PD startup = Linux/FreeBSD guest console output → linux_vmm slot 14
        const slot = PD_NAME_TO_SLOT['linux_vmm']; // 14
        serialCache.lines[slot].push(clean);
        added++;
        broadcast(slot, { slot, line: clean + '\n' });
      }
    }

    return added;
  } finally {
    fs.closeSync(fd);
  }
}

/** Poll the serial log every 100 ms to pick up new output */
setInterval(() => parseSerialLog(), 100);

// ─── Bidirectional serial socket (QEMU chardev) ──────────────────────────────
//
// When QEMU is launched with -chardev socket,path=/tmp/agentos-serial.sock,
// we connect here for both reading (output → serialCache) and writing (input
// injection from terminal tiles). Falls back gracefully if socket not present.

let serialSocket = null;
let serialSocketLineBuf = '';
let serialSocketSeenPd = false;

const SYSPREFIX_RE2 = /^(LDR\||MON\||MICROKIT\||Bootstrapping|seL4 |ELF |CAPDL )/;

function handleSerialSocketLine(rawLine) {
  // Write through to log file for the file-based parser (compatibility)
  try { fs.appendFileSync(SERIAL_LOG, rawLine + '\n'); } catch { /* ignore */ }

  const BRACKET_RE = /(?:\x1b\[[0-9;]*m)?\[([a-zA-Z0-9_]+)\](?:\x1b\[[0-9;]*m)?\s*(.*)/;
  const PIPE_RE    = /^([a-zA-Z0-9_]+)\|(?:INFO|WARN|ERROR|DEBUG):\s*(.*)/;

  const clean = stripAnsi(rawLine).trim();
  if (!clean) return;

  let pdName = null, text = null;
  const bm = BRACKET_RE.exec(rawLine.trim());
  if (bm) {
    pdName = bm[1].toLowerCase();
    text   = stripAnsi(bm[2]).trimEnd();
    serialSocketSeenPd = true;
  } else {
    const pm = PIPE_RE.exec(clean);
    if (pm) {
      pdName = pm[1].toLowerCase();
      text   = pm[2].trimEnd();
      serialSocketSeenPd = true;
    }
  }

  if (pdName && text !== null) {
    const normalised = pdName.replace(/[\s-]/g, '_');
    const slot = PD_NAME_TO_SLOT[normalised] ?? PD_NAME_TO_SLOT[pdName] ?? -1;
    if (slot >= 0 && slot < MAX_SLOTS) {
      const line = `[${normalised}] ${text}`;
      if (!serialCache.lines[slot].includes(line)) { // avoid dups with file parser
        serialCache.lines[slot].push(line);
        broadcast(slot, { slot, line: line + '\n' });
      }
    }
  } else if (serialSocketSeenPd && !SYSPREFIX_RE2.test(clean)) {
    const slot = PD_NAME_TO_SLOT['linux_vmm'];
    if (slot >= 0 && !serialCache.lines[slot].includes(clean)) {
      serialCache.lines[slot].push(clean);
      broadcast(slot, { slot, line: clean + '\n' });
    }
  }
}

function connectSerialSocket() {
  if (serialSocket) return;
  const sock = net.createConnection(SERIAL_SOCK);
  sock.on('connect', () => {
    console.log('[console] connected to QEMU serial socket (bidirectional)');
    serialSocket = sock;
    // reset duplicate-avoidance: socket stream is now authoritative
    serialSocketLineBuf = '';
  });
  sock.on('data', chunk => {
    // Push raw bytes immediately to all connected terminal tiles for linux_vmm.
    // This gives full-duplex character-by-character echo without waiting for \n.
    const rawStr = chunk.toString('utf8');
    const lvmSlot = PD_NAME_TO_SLOT['linux_vmm'];
    const lvmClients = terminalClients.get(lvmSlot);
    if (lvmClients?.size > 0) {
      for (const tws of lvmClients) {
        if (tws.readyState === 1) tws.send(rawStr);
      }
    }

    // Line-buffer for cache (other slots + linux_vmm history)
    serialSocketLineBuf += rawStr;
    const parts = serialSocketLineBuf.split('\n');
    serialSocketLineBuf = parts.pop();
    parts.forEach(line => handleSerialSocketLine(line));
  });
  sock.on('error', () => { serialSocket = null; });
  sock.on('close', () => {
    serialSocket = null;
    setTimeout(connectSerialSocket, 1000);
  });
}

// Try immediately and retry every second until QEMU starts
setInterval(connectSerialSocket, 1000);
connectSerialSocket();

// ─── FreeBSD lazy asset loader ───────────────────────────────────────────────
//
// FreeBSD requires two assets that are NOT in the repo:
//   1. EDK2 UEFI firmware  — already present at the Homebrew QEMU path
//   2. FreeBSD arm64 QEMU disk image — downloaded on first use from freebsd.org
//
// Once both are present, `make build GUEST_OS=freebsd` produces freebsd_vmm.elf
// and the user can switch to the FreeBSD system image.

const ROOT_DIR      = nodePath.resolve(__dirname, '..');
const GUEST_IMG_DIR = nodePath.join(ROOT_DIR, 'guest-images');
const EDK2_SRC      = '/opt/homebrew/share/qemu/edk2-aarch64-code.fd';
const EDK2_DST      = nodePath.join(GUEST_IMG_DIR, 'edk2-aarch64-code.fd');
const FREEBSD_VER   = '14.3';
const FREEBSD_IMG   = nodePath.join(GUEST_IMG_DIR, `FreeBSD-${FREEBSD_VER}-RELEASE-arm64-aarch64-ufs.raw`);
const FREEBSD_URL   = `https://download.freebsd.org/releases/VM-IMAGES/${FREEBSD_VER}-RELEASE/aarch64/Latest/FreeBSD-${FREEBSD_VER}-RELEASE-arm64-aarch64-ufs.raw.xz`;

/** Live state of any in-progress FreeBSD asset fetch */
const freebsdState = {
  phase:    'idle',   // idle | preparing | downloading | building | ready | error
  step:     '',
  progress: 0,        // 0–100
  error:    null,
  pid:      null,
};

function freebsdAssetsReady() {
  return fs.existsSync(EDK2_DST) && fs.existsSync(FREEBSD_IMG);
}

async function prepareFreeBSDAssets() {
  if (freebsdState.phase === 'preparing' || freebsdState.phase === 'downloading' || freebsdState.phase === 'building') return;
  freebsdState.phase = 'preparing';
  freebsdState.error = null;
  freebsdState.progress = 0;

  try {
    fs.mkdirSync(GUEST_IMG_DIR, { recursive: true });

    // Step 1: EDK2 firmware — copy from Homebrew QEMU installation
    if (!fs.existsSync(EDK2_DST)) {
      freebsdState.step = 'Copying EDK2 UEFI firmware from Homebrew QEMU…';
      fs.copyFileSync(EDK2_SRC, EDK2_DST);
      console.log(`[freebsd] EDK2 firmware copied → ${EDK2_DST}`);
    }
    freebsdState.progress = 5;

    // Step 2: FreeBSD disk image — download + decompress
    if (!fs.existsSync(FREEBSD_IMG)) {
      freebsdState.phase = 'downloading';
      freebsdState.step  = `Downloading FreeBSD ${FREEBSD_VER} arm64 image…`;
      console.log(`[freebsd] downloading ${FREEBSD_URL}`);
      await downloadXZ(FREEBSD_URL, FREEBSD_IMG);
    }
    freebsdState.progress = 80;

    // Step 3: Build freebsd_vmm.elf
    freebsdState.phase = 'building';
    freebsdState.step  = 'Building freebsd_vmm.elf (make build GUEST_OS=freebsd)…';
    await runMake(['build', 'BOARD=qemu_virt_aarch64', 'GUEST_OS=freebsd']);
    freebsdState.progress = 100;
    freebsdState.phase    = 'ready';
    freebsdState.step     = 'FreeBSD assets ready — restart console to boot FreeBSD';
    console.log('[freebsd] assets ready');
  } catch (e) {
    freebsdState.phase = 'error';
    freebsdState.error = e.message;
    console.error('[freebsd] asset prep failed:', e.message);
  }
}

/** Download a .xz URL and pipe through `xz -d` to produce a raw file */
function downloadXZ(url, dest) {
  return new Promise((resolve, reject) => {
    const tmpXZ = dest + '.xz';
    const out   = fs.createWriteStream(tmpXZ);
    https.get(url, res => {
      if (res.statusCode === 301 || res.statusCode === 302) {
        out.close();
        fs.unlinkSync(tmpXZ);
        return downloadXZ(res.headers.location, dest).then(resolve).catch(reject);
      }
      if (res.statusCode !== 200) {
        out.close();
        return reject(new Error(`HTTP ${res.statusCode} for ${url}`));
      }
      const total = parseInt(res.headers['content-length'] ?? '0', 10);
      let received = 0;
      res.on('data', chunk => {
        received += chunk.length;
        if (total > 0) freebsdState.progress = 5 + Math.floor((received / total) * 70);
      });
      res.pipe(out);
      out.on('finish', () => {
        out.close(() => {
          // Decompress with system xz
          const xzProc = spawnProc('xz', ['-d', '-k', tmpXZ], { stdio: 'inherit' });
          xzProc.on('close', code => {
            try { fs.unlinkSync(tmpXZ); } catch { /* ignore */ }
            if (code === 0) resolve();
            else reject(new Error(`xz decompression exited with code ${code}`));
          });
        });
      });
      out.on('error', reject);
    }).on('error', reject);
  });
}

/** Run a make target in ROOT_DIR */
function runMake(args) {
  return new Promise((resolve, reject) => {
    const p = spawnProc('make', args, { cwd: ROOT_DIR, stdio: 'inherit' });
    freebsdState.pid = p.pid;
    p.on('close', code => {
      freebsdState.pid = null;
      if (code === 0) resolve();
      else reject(new Error(`make ${args.join(' ')} exited with code ${code}`));
    });
  });
}

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

// ─── agentOS API helpers ─────────────────────────────────────────────────────

async function agentosPost(path, body = {}, timeoutMs = 2000) {
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);
  try {
    const res = await fetch(`${AGENTOS_BASE}${path}`, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${AGENTOS_TOKEN}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(body),
      signal: ac.signal,
    });
    if (!res.ok) throw new Error(`agentOS POST ${path} → ${res.status}`);
    return res.json();
  } finally {
    clearTimeout(timer);
  }
}

async function agentosGet(path, timeoutMs = 2000) {
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);
  try {
    const res = await fetch(`${AGENTOS_BASE}${path}`, {
      headers: { Authorization: `Bearer ${AGENTOS_TOKEN}` },
      signal: ac.signal,
    });
    if (!res.ok) throw new Error(`agentOS ${path} → ${res.status}`);
    return res.json();
  } finally {
    clearTimeout(timer);
  }
}

// ─── Log subscription state ──────────────────────────────────────────────────

/** Map<slot, Set<ws>> — clients subscribed to each slot */
const subscribers = new Map();
for (let i = 0; i < MAX_SLOTS; i++) subscribers.set(i, new Set());

/** Map<slot, Set<ws>> — terminal tiles receiving raw byte stream for this slot */
const terminalClients = new Map();
for (let i = 0; i < MAX_SLOTS; i++) terminalClients.set(i, new Set());

/** Map<slot, lastSeenLineCount> */
const slotCursor = new Map();
for (let i = 0; i < MAX_SLOTS; i++) slotCursor.set(i, 0);

/** Set<slot> — slots actively being polled for log subscriptions */
const activeSlots = new Set();

function broadcast(slot, msg) {
  const str = JSON.stringify(msg);
  for (const ws of subscribers.get(slot) ?? []) {
    if (ws.readyState === 1) ws.send(str);
  }
}

async function pollSlot(slot) {
  if (!activeSlots.has(slot)) return;
  try {
    const data = await agentosGet(`/api/agentos/console/${slot}`);
    const lines = data.lines ?? [];
    const cursor = slotCursor.get(slot) ?? 0;
    if (lines.length > cursor) {
      const newLines = lines.slice(cursor);
      slotCursor.set(slot, lines.length);
      for (const line of newLines) broadcast(slot, { slot, line });
    }
  } catch { /* agentOS unreachable — retry next tick */ }
  if (activeSlots.has(slot)) setTimeout(() => pollSlot(slot), POLL_MS);
}

function startPolling(slot) {
  if (!activeSlots.has(slot)) {
    activeSlots.add(slot);
    pollSlot(slot);
    console.log(`[console] started polling slot ${slot} (${SLOT_NAMES[slot] ?? '?'})`);
  }
}

function maybeStopPolling(slot) {
  if ((subscribers.get(slot)?.size ?? 0) === 0) {
    activeSlots.delete(slot);
    console.log(`[console] stopped polling slot ${slot} — no subscribers`);
  }
}

// ─── HTTP server ─────────────────────────────────────────────────────────────

const server = http.createServer(async (req, res) => {
  const pathname = (req.url ?? '/').split('?')[0];

  if (pathname === '/' || pathname === '/dashboard') {
    try {
      const html = fs.readFileSync(nodePath.join(__dirname, 'dashboard.html'), 'utf-8');
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(html);
    } catch {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.end('dashboard.html not found');
    }

  } else if (pathname === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      ok: true,
      activeSlots: [...activeSlots],
      subscriberCounts: Object.fromEntries(
        [...subscribers.entries()].map(([k, v]) => [k, v.size])
      ),
    }));

  } else if (pathname === '/api/agentos/profiler/snapshot') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(generateProfilerSnapshot()));

  } else if (pathname === '/api/debug') {
    // Probe agentOS API directly and return the raw result for browser debugging
    const probes = {};
    for (const slot of [0, 1, 2]) {
      try {
        const d = await agentosGet(`/api/agentos/console/${slot}?cursor=0`);
        probes[slot] = { ok: true, lines: (d.lines || []).length, sample: (d.lines || [])[0] };
      } catch (e) {
        probes[slot] = { ok: false, error: e.message };
      }
    }
    let slotsResult;
    try { slotsResult = await agentosGet('/api/agentos/slots'); }
    catch (e) { slotsResult = { error: e.message }; }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ agentosBase: AGENTOS_BASE, slots: slotsResult, consoleSamples: probes }, null, 2));

  } else if (pathname === '/api/debug/serial') {
    const logFile = process.env.SERIAL_LOG || '/tmp/agentos-serial.log';
    try {
      const tail = execSync(`tail -40 "${logFile}" 2>/dev/null || echo "(no serial log at ${logFile})"`, { encoding: 'utf8' });
      res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end(tail);
    } catch {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('(serial log unavailable)');
    }

  } else if (pathname === '/api/agentos/agents/freebsd/status') {
    try {
      const data = await agentosGet('/api/agentos/agents/freebsd/status');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(data));
    } catch (e) {
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: e.message }));
    }

  } else if (pathname === '/api/agentos/agents') {
    try {
      const data = await agentosGet('/api/agentos/agents');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(data));
    } catch (e) {
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: e.message }));
    }

  } else if (pathname === '/api/agentos/agents/spawn') {
    try {
      const body = await new Promise(resolve => {
        let raw = '';
        req.on('data', d => { raw += d; });
        req.on('end', () => { try { resolve(JSON.parse(raw || '{}')); } catch { resolve({}); } });
      });
      const data = await agentosPost('/api/agentos/agents/spawn', body);
      res.writeHead(202, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(data));
    } catch (e) {
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: e.message }));
    }

  } else if (pathname === '/api/agentos/slots') {
    try {
      const data = await agentosGet('/api/agentos/slots');
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(data));
    } catch {
      // Fallback: infer active slots from serial log cache
      parseSerialLog();
      const slots = SLOT_NAMES.map((name, id) => ({
        id, name,
        active: serialCache.lines[id].length > 0,
        lines: serialCache.lines[id].length,
        source: 'serial',
      }));
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ slots, source: 'serial_log' }));
    }

  } else {
    res.writeHead(404);
    res.end();
  }
});

// ─── WebSocket servers ────────────────────────────────────────────────────────

const logWss      = new WebSocketServer({ noServer: true });
const terminalWss = new WebSocketServer({ noServer: true });

server.on('upgrade', (req, socket, head) => {
  const pathname = (req.url || '/').split('?')[0];
  if (pathname.startsWith('/terminal')) {
    terminalWss.handleUpgrade(req, socket, head, (ws) => {
      terminalWss.emit('connection', ws, req);
    });
  } else {
    logWss.handleUpgrade(req, socket, head, (ws) => {
      logWss.emit('connection', ws, req);
    });
  }
});

// ─── Log subscription WebSocket handler ──────────────────────────────────────

logWss.on('connection', (ws, req) => {
  const clientIp = req.socket.remoteAddress;
  console.log(`[console] log client connected: ${clientIp}`);

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

    } else if (action === 'unsubscribe') {
      if (typeof slot === 'number' && slot >= 0 && slot < MAX_SLOTS) {
        subscribers.get(slot).delete(ws);
        maybeStopPolling(slot);
        ws.send(JSON.stringify({ event: 'unsubscribed', slot }));
      }

    } else if (action === 'attach') {
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

    } else if (action === 'list') {
      const data = [];
      for (let s = 0; s < MAX_SLOTS; s++) {
        data.push({
          slot: s, name: SLOT_NAMES[s] ?? 'unknown',
          lines: slotCursor.get(s) ?? 0,
          active: activeSlots.has(s),
          subscribers: subscribers.get(s)?.size ?? 0,
        });
      }
      ws.send(JSON.stringify({ event: 'slots', data }));

    } else {
      ws.send(JSON.stringify({ event: 'error', msg: `unknown action: ${action}` }));
    }
  });

  ws.on('close', () => {
    for (let s = 0; s < MAX_SLOTS; s++) {
      if (subscribers.get(s).delete(ws)) maybeStopPolling(s);
    }
    console.log(`[console] log client disconnected: ${clientIp}`);
  });

  ws.on('error', (e) => console.error(`[console] log client error: ${e.message}`));
});

// ─── Terminal WebSocket handler ───────────────────────────────────────────────

terminalWss.on('connection', async (ws, req) => {
  const clientIp = req.socket.remoteAddress;
  const pathname = (req.url || '/').split('?')[0];
  const slotStr  = pathname.split('/').filter(Boolean)[1] ?? '0';
  const slot     = parseInt(slotStr, 10);

  if (isNaN(slot) || slot < 0 || slot >= MAX_SLOTS) {
    ws.close(1008, 'invalid slot');
    return;
  }

  console.log(`[console] terminal client connected: ${clientIp} → slot ${slot}`);

  let cursor = 0;
  let alive  = true;
  let usingSerialFallback = false;

  // Register for raw byte streaming (enables full-duplex echo)
  terminalClients.get(slot)?.add(ws);

  // Attach to the slot (non-fatal if agentOS not yet ready)
  try {
    await agentosPost(`/api/agentos/console/attach/${slot}`);
  } catch { /* will catch output on first poll */ }

  // Flush serial cache already accumulated for this slot
  const cachedLines = serialCache.lines[slot] ?? [];
  if (cachedLines.length > 0 && ws.readyState === 1) {
    ws.send(cachedLines.join('\r\n') + '\r\n');
  }
  let serialCursor = cachedLines.length;

  // Slots receiving raw bytes via terminalClients don't need the line poller —
  // output arrives character-by-character through the socket data handler.
  // For other slots (seL4 PDs), keep the line-buffered polling approach.
  const isRawStreamed = () => (terminalClients.get(slot)?.has(ws) && serialSocket !== null);

  // Poll for raw terminal output and forward to the browser
  const poller = setInterval(async () => {
    if (!alive) { clearInterval(poller); return; }
    // Skip line-based polling while raw bytes are flowing from the socket
    if (isRawStreamed()) return;

    // Try the live agentOS HTTP API first
    let apiOk = false;
    try {
      const data  = await agentosGet(`/api/agentos/console/${slot}?cursor=${cursor}`);
      const lines = data.lines ?? [];
      if (lines.length > 0) {
        cursor += lines.length;
        const text = lines.join('\r\n') + '\r\n';
        if (ws.readyState === 1) ws.send(text);
      }
      apiOk = true;
      if (usingSerialFallback) {
        usingSerialFallback = false;
        console.log(`[console] slot ${slot}: switched back to live API`);
      }
    } catch { /* agentOS unreachable — fall through to serial cache */ }

    if (!apiOk) {
      // Serial log fallback: forward any new lines that arrived since last check
      if (!usingSerialFallback) {
        usingSerialFallback = true;
        console.log(`[console] slot ${slot}: using serial log fallback`);
      }
      const slotLines = serialCache.lines[slot] ?? [];
      if (slotLines.length > serialCursor) {
        const newLines = slotLines.slice(serialCursor);
        serialCursor = slotLines.length;
        if (ws.readyState === 1) ws.send(newLines.join('\r\n') + '\r\n');
      }
    }
  }, POLL_MS);

  // Forward keyboard input from the browser to the inject endpoint
  ws.on('message', async (raw) => {
    const data = raw.toString('utf8');
    try {
      await agentosPost(`/api/agentos/console/inject/${slot}`, { data });
    } catch { /* inject may not be implemented yet */ }
  });

  ws.on('close', () => {
    alive = false;
    clearInterval(poller);
    terminalClients.get(slot)?.delete(ws);
    console.log(`[console] terminal client disconnected: ${clientIp}`);
  });

  ws.on('error', (e) => {
    alive = false;
    clearInterval(poller);
    terminalClients.get(slot)?.delete(ws);
    console.error(`[console] terminal client error: ${e.message}`);
  });
});

// ─── agentOS HTTP Bridge server (port 8790) ──────────────────────────────────
//
// Local stub bridge API backed by the serial log cache.  Runs on BRIDGE_PORT
// (default 8790) so it does NOT conflict with QEMU's hostfwd on 8789.
// Port 8789 is reserved for the future real Linux-guest HTTP bridge; when that
// is ready, set AGENTOS_HOST + AGENTOS_API_PORT=8789 to point at it instead.

const bridgeServer = http.createServer(async (req, res) => {
  const url    = new URL(req.url ?? '/', `http://localhost`);
  const path   = url.pathname;
  const method = req.method ?? 'GET';

  res.setHeader('Content-Type', 'application/json');

  // Parse body for POST requests
  let body = {};
  if (method === 'POST') {
    body = await new Promise(resolve => {
      let raw = '';
      req.on('data', d => { raw += d; });
      req.on('end', () => { try { resolve(JSON.parse(raw || '{}')); } catch { resolve({}); } });
    });
  }

  // GET /api/agentos/agents
  if (method === 'GET' && path === '/api/agentos/agents') {
    parseSerialLog();
    const vmLinux = serialCache.lines[14];
    const linuxBooted = vmLinux.some(l => /login:|Welcome to|buildroot/i.test(l));
    const fbReady = freebsdAssetsReady();
    const agents = [
      {
        id: 'linux_vm_0', type: 'linux_vm', slot: 14, name: 'Linux VM (Buildroot)',
        status: linuxBooted ? 'running' : (vmLinux.length > 0 ? 'booting' : 'stopped'),
        lines: vmLinux.length,
        note: linuxBooted ? 'Buildroot booted — log in as root (no password)' : null,
      },
      {
        id: 'freebsd_vm_0', type: 'freebsd_vm', slot: -1, name: 'FreeBSD VM',
        status: fbReady ? 'ready' : freebsdState.phase === 'idle' ? 'not_prepared' : freebsdState.phase,
        lines: 0,
        note: fbReady ? 'Assets ready — boot FreeBSD from the spawn dialog'
            : freebsdState.phase === 'error' ? `Error: ${freebsdState.error}`
            : freebsdState.phase !== 'idle' ? `${freebsdState.step} (${freebsdState.progress}%)`
            : 'EDK2 on-machine · FreeBSD image download required (~400 MB)',
        freebsdState: { ...freebsdState },
      },
      ...([0,1,2,3].map(i => {
        const slot = 9 + i;
        const lines = serialCache.lines[slot];
        const failed = lines.some(l => /FAILED|ERROR/i.test(l));
        const running = lines.some(l => /ALIVE|ready/i.test(l));
        return {
          id: `wasm_${i}`, type: 'wasm', slot, name: `WASM Agent (swap_slot_${i})`,
          status: failed ? 'failed' : running ? 'running' : (lines.length ? 'idle' : 'empty'),
          lines: lines.length,
          note: failed ? 'WASM runtime init failed — load a valid .wasm module' : null,
        };
      })),
    ];
    res.writeHead(200);
    res.end(JSON.stringify({ agents }));
    return;
  }

  // GET /api/agentos/agents/freebsd/status
  if (method === 'GET' && path === '/api/agentos/agents/freebsd/status') {
    res.writeHead(200);
    res.end(JSON.stringify({ ...freebsdState, assetsReady: freebsdAssetsReady() }));
    return;
  }

  // POST /api/agentos/agents/spawn
  if (method === 'POST' && path === '/api/agentos/agents/spawn') {
    const { type, slot, name } = body;
    if (type === 'freebsd_vm') {
      if (freebsdAssetsReady()) {
        res.writeHead(200);
        res.end(JSON.stringify({ ok: true, message: 'FreeBSD assets ready. Restart console with GUEST_OS=freebsd to boot.' }));
      } else {
        // Kick off async preparation (non-blocking)
        prepareFreeBSDAssets().catch(() => {});
        res.writeHead(202);
        res.end(JSON.stringify({ ok: true, queued: true, message: 'FreeBSD asset preparation started. Poll /api/agentos/agents/freebsd/status for progress.' }));
      }
      return;
    }
    // WASM / other types
    res.writeHead(202);
    res.end(JSON.stringify({
      ok: true, queued: true,
      message: `Spawn request for ${type ?? 'agent'} queued (bridge stub — seL4 IPC not yet wired)`,
      slot: slot ?? null, name: name ?? null,
    }));
    return;
  }

  // GET /api/agentos/slots
  if (method === 'GET' && path === '/api/agentos/slots') {
    parseSerialLog();
    const slots = SLOT_NAMES.map((name, id) => ({
      id, name,
      active: serialCache.lines[id].length > 0,
      lines:  serialCache.lines[id].length,
      source: 'serial',
    }));
    res.writeHead(200);
    res.end(JSON.stringify({ slots, source: 'serial_log' }));
    return;
  }

  // GET /api/agentos/console/status
  if (method === 'GET' && path === '/api/agentos/console/status') {
    res.writeHead(200);
    res.end(JSON.stringify({
      active_pd: 0xFFFFFFFF, mode: 1 /* broadcast */, session_count: 9,
      source: 'serial_log',
    }));
    return;
  }

  // GET /api/agentos/console/:slot[?cursor=N]
  const consoleReadMatch = path.match(/^\/api\/agentos\/console\/(\d+)$/);
  if (method === 'GET' && consoleReadMatch) {
    const slot   = parseInt(consoleReadMatch[1], 10);
    const cursor = parseInt(url.searchParams.get('cursor') ?? '0', 10) || 0;
    if (slot < 0 || slot >= MAX_SLOTS) {
      res.writeHead(400); res.end(JSON.stringify({ error: 'invalid slot' })); return;
    }
    const lines = (serialCache.lines[slot] ?? []).slice(cursor);
    res.writeHead(200);
    res.end(JSON.stringify({ slot, cursor: cursor + lines.length, lines, source: 'serial_log' }));
    return;
  }

  // POST /api/agentos/console/attach/:slot
  const attachMatch = path.match(/^\/api\/agentos\/console\/attach\/(\d+)$/);
  if (method === 'POST' && attachMatch) {
    const slot = parseInt(attachMatch[1], 10);
    res.writeHead(200);
    res.end(JSON.stringify({ slot, ok: true, source: 'serial_log' }));
    return;
  }

  // POST /api/agentos/console/inject/:slot — forwards keystrokes to QEMU serial socket
  const injectMatch = path.match(/^\/api\/agentos\/console\/inject\/(\d+)$/);
  if (method === 'POST' && injectMatch) {
    const data = body.data ?? '';
    if (serialSocket && data.length > 0) {
      serialSocket.write(Buffer.from(data, 'utf8'));
      res.writeHead(200);
      res.end(JSON.stringify({ ok: true, bytes: data.length, source: 'socket' }));
    } else {
      res.writeHead(200);
      res.end(JSON.stringify({ ok: false, bytes: 0, source: 'no_socket', error: serialSocket ? 'empty input' : 'serial socket not connected' }));
    }
    return;
  }

  res.writeHead(404);
  res.end(JSON.stringify({ error: 'not found' }));
});

// ─── Start ────────────────────────────────────────────────────────────────────

server.listen(PORT, () => {
  console.log(`[console] agentOS console listening on http://0.0.0.0:${PORT}`);
  console.log(`[console] dashboard: http://127.0.0.1:${PORT}/`);
  console.log(`[console] bridge:    http://127.0.0.1:${BRIDGE_PORT}/`);
});

bridgeServer.listen(BRIDGE_PORT, '127.0.0.1', () => {
  console.log(`[console] agentOS bridge API on http://127.0.0.1:${BRIDGE_PORT}`);
});

process.on('SIGTERM', () => { server.close(); bridgeServer.close(); process.exit(0); });
process.on('SIGINT',  () => { server.close(); bridgeServer.close(); process.exit(0); });
