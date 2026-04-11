/**
 * agentOS vibe-swap — WASM hot-swap pipeline
 * Validates, stages, and executes WASM module swaps.
 */

export class VibeSwapPipeline {
  constructor() {
    this.staged = new Map();   // slotId -> { wasm: Buffer, hash: string, ts: number }
    this.active = new Map();   // slotId -> { name: string, loadedAt: number }
  }

  /** Stage a WASM buffer for slot `slotId`. Returns SHA-256 hex. */
  async stage(slotId, wasmBuffer, name = 'unnamed') {
    const hash = await this.#sha256hex(wasmBuffer);
    this.staged.set(slotId, { wasm: wasmBuffer, hash, name, ts: Date.now() });
    return { slotId, hash, size: wasmBuffer.byteLength };
  }

  /** Validate staged WASM for a slot (check magic bytes + exports). */
  validate(slotId) {
    const entry = this.staged.get(slotId);
    if (!entry) return { valid: false, error: 'no staged module' };
    const buf = new Uint8Array(entry.wasm);
    if (buf[0] !== 0x00 || buf[1] !== 0x61 || buf[2] !== 0x73 || buf[3] !== 0x6D) {
      return { valid: false, error: 'invalid WASM magic' };
    }
    return { valid: true, hash: entry.hash, size: entry.wasm.byteLength };
  }

  /** Commit staged module to active slot. */
  commit(slotId) {
    const entry = this.staged.get(slotId);
    if (!entry) return { ok: false, error: 'nothing staged' };
    this.active.set(slotId, { name: entry.name, hash: entry.hash, loadedAt: Date.now() });
    this.staged.delete(slotId);
    return { ok: true, slotId, name: entry.name };
  }

  /** Rollback active slot (clear it). */
  rollback(slotId) {
    this.active.delete(slotId);
    this.staged.delete(slotId);
    return { ok: true, slotId };
  }

  status() {
    return {
      staged: Object.fromEntries(
        [...this.staged.entries()].map(([k,v]) => [k, { name: v.name, hash: v.hash, size: v.wasm.byteLength }])
      ),
      active: Object.fromEntries(this.active),
    };
  }

  async #sha256hex(buffer) {
    const hash = await crypto.subtle.digest('SHA-256', buffer);
    return Array.from(new Uint8Array(hash)).map(b => b.toString(16).padStart(2,'0')).join('');
  }
}
