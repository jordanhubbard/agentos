import { test } from 'node:test';
import assert from 'node:assert/strict';
import { VibeSwapPipeline } from '../src/index.mjs';

/* Minimal valid WASM module (magic + version only, no sections) */
const VALID_WASM = Buffer.from([0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00]);
const INVALID_WASM = Buffer.from([0xDE, 0xAD, 0xBE, 0xEF]);

test('stage returns hash and size', async () => {
  const p = new VibeSwapPipeline();
  const result = await p.stage(0, VALID_WASM, 'test-module');
  assert.equal(result.slotId, 0);
  assert.equal(result.size, VALID_WASM.byteLength);
  assert.match(result.hash, /^[0-9a-f]{64}$/);
});

test('validate passes for valid WASM magic', async () => {
  const p = new VibeSwapPipeline();
  await p.stage(1, VALID_WASM, 'valid');
  const v = p.validate(1);
  assert.equal(v.valid, true);
  assert.equal(v.size, VALID_WASM.byteLength);
});

test('validate rejects invalid WASM magic', async () => {
  const p = new VibeSwapPipeline();
  await p.stage(2, INVALID_WASM, 'bad');
  const v = p.validate(2);
  assert.equal(v.valid, false);
  assert.ok(v.error);
});

test('validate returns error when nothing staged', () => {
  const p = new VibeSwapPipeline();
  const v = p.validate(99);
  assert.equal(v.valid, false);
  assert.equal(v.error, 'no staged module');
});

test('commit moves staged to active', async () => {
  const p = new VibeSwapPipeline();
  await p.stage(3, VALID_WASM, 'my-agent');
  const c = p.commit(3);
  assert.equal(c.ok, true);
  assert.equal(c.name, 'my-agent');
  /* staged slot should be gone */
  assert.equal(p.staged.has(3), false);
  /* active slot should exist */
  assert.equal(p.active.has(3), true);
  assert.equal(p.active.get(3).name, 'my-agent');
});

test('commit fails when nothing staged', () => {
  const p = new VibeSwapPipeline();
  const c = p.commit(42);
  assert.equal(c.ok, false);
  assert.ok(c.error);
});

test('rollback clears both staged and active', async () => {
  const p = new VibeSwapPipeline();
  await p.stage(4, VALID_WASM, 'agent-x');
  p.commit(4);
  const r = p.rollback(4);
  assert.equal(r.ok, true);
  assert.equal(p.active.has(4), false);
  assert.equal(p.staged.has(4), false);
});

test('rollback on empty slot is a no-op', () => {
  const p = new VibeSwapPipeline();
  const r = p.rollback(77);
  assert.equal(r.ok, true);
});

test('status reflects staged and active maps', async () => {
  const p = new VibeSwapPipeline();
  await p.stage(5, VALID_WASM, 'staged-mod');
  await p.stage(6, VALID_WASM, 'to-commit');
  p.commit(6);

  const s = p.status();
  assert.ok(s.staged['5']);
  assert.equal(s.staged['5'].name, 'staged-mod');
  assert.ok(s.active['6']);
  assert.equal(s.active['6'].name, 'to-commit');
  assert.equal(s.staged['6'], undefined);
});

test('identical WASM buffers produce the same hash', async () => {
  const p = new VibeSwapPipeline();
  const r1 = await p.stage(10, VALID_WASM, 'a');
  const r2 = await p.stage(11, VALID_WASM, 'b');
  assert.equal(r1.hash, r2.hash);
});
