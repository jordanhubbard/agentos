# agentOS Vibe Integration Test

Tests the full vibe-coding loop end-to-end:

```
Agent prompt → bridge generate → C source → compile → WASM → bridge health
```

## What it validates

| Phase | What is tested |
|-------|----------------|
| 1 — Generate | `POST /api/agentos/vibe/generate` accepts a prompt and returns either generated C code (real LLM) or a well-formed error (no backend). |
| 2 — Compile  | `POST /api/agentos/vibe/compile` accepts C source and returns a base64-encoded WASM binary, or skips gracefully if clang/wasm32-wasi is unavailable. |
| 3 — WASM magic | Decodes the base64 WASM and confirms the first four bytes are `\x00asm` (WebAssembly module magic). |
| 4 — Health    | `GET /api/agentos/agents` confirms the bridge is still alive and returning an agents list after the test run. |

The test is designed to **pass in CI without a real LLM or real QEMU**:

- Phase 1 is **SKIP** when no `ANTHROPIC_API_KEY` or `OPENAI_API_KEY` is set.
- Phase 2 is **SKIP** when `clang --target=wasm32-wasi` is not available.
- Phase 3 is **SKIP** when Phase 2 is skipped.
- Phase 4 always runs — it is the minimum liveness check.

A run where all non-skipped phases pass exits with code 0.

## How to run

### Mock mode (CI / no LLM)

```bash
chmod +x tests/vibe_integration_test.sh
./tests/vibe_integration_test.sh
```

The script starts `agentos-console` on port 8790, waits for it to become
ready, runs the four phases, then kills it.  If a bridge is already running
on port 8790, it is reused and not stopped after the test.

Expected output (mock mode, no clang):

```
[INFO] Checking prerequisites...
[INFO] cargo: cargo 1.78.0 (...)
[INFO] curl: curl 8.x (...)
[INFO] clang with wasm32-wasi not found — compile phase will be skipped
[INFO] Checking bridge on port 8790...
[INFO] Starting bridge: cargo run --bin agentos-console ...
[INFO] Bridge PID: 12345
[INFO] Waiting for bridge to become ready (up to 30 s)...
[INFO] Bridge is ready.

--- Phase 1: Generate endpoint (POST /api/agentos/vibe/generate) ---
[SKIP] Phase 1: no LLM backend available (AGENTOS_CODEGEN_BACKEND=mock) — expected in CI

--- Phase 2: Compile endpoint (POST /api/agentos/vibe/compile) ---
[SKIP] Phase 2: clang with wasm32-wasi not found — install llvm to enable compile phase

--- Phase 3: WASM magic byte verification ---
[SKIP] Phase 3: skipped — no WASM output from Phase 2

--- Phase 4: Bridge health check (GET /api/agentos/agents) ---
[INFO] Bridge returned 6 agent(s)
[PASS] Phase 4: bridge is healthy and returned agents list

══════════════════════════════════════════════════
Vibe Integration Test Results
══════════════════════════════════════════════════
  Passed:  1
  Failed:  0
  Skipped: 3
  Total:   1 / 4
══════════════════════════════════════════════════
PASS — all non-skipped tests passed
```

### With a real LLM

Set an API key before running — the bridge auto-detects it:

```bash
ANTHROPIC_API_KEY=sk-ant-... ./tests/vibe_integration_test.sh
# or
OPENAI_API_KEY=sk-... ./tests/vibe_integration_test.sh
```

Phase 1 will now call the LLM and return generated C source.  Phase 2
then compiles it (if clang is available) and Phases 3–4 verify the result.

### With clang + wasm32-wasi installed

On macOS:
```bash
brew install llvm
export PATH="$(brew --prefix llvm)/bin:$PATH"
./tests/vibe_integration_test.sh
```

On Debian/Ubuntu:
```bash
apt install clang wasi-libc
./tests/vibe_integration_test.sh
```

## Environment variables

| Variable | Default | Meaning |
|----------|---------|---------|
| `AGENTOS_BRIDGE_PORT` | `8790` | Bridge port to use |
| `AGENTOS_TIMEOUT` | `120` | Seconds allowed for the compile phase |
| `AGENTOS_CODEGEN_BACKEND` | `mock` | `mock` = skip LLM, `http` = use API key |
| `AGENTOS_DEBUG` | (unset) | Set to any value to print bridge logs to stdout |
| `ANTHROPIC_API_KEY` | (unset) | Enables Anthropic API backend |
| `OPENAI_API_KEY` | (unset) | Enables OpenAI API backend |

## The mock_memfs.c module

`tests/vibe/mock_memfs.c` is the known-good reference service used in Phase 2.
It implements the full `storage.v1` interface:

- `STORAGE_OP_WRITE (0x30)` — write key/value
- `STORAGE_OP_READ (0x31)` — read value by key
- `STORAGE_OP_DELETE (0x32)` — delete entry
- `STORAGE_OP_STAT (0x33)` — stat a single key
- `STORAGE_OP_STAT_SVC (0x20)` — aggregate stats (file count, total bytes)
- `AOS_LABEL_HEALTH (0xFFFF)` — liveness probe

Compile manually to check it builds cleanly:

```bash
clang --target=wasm32-wasi -O2 -nostdlib \
      -Wl,--no-entry -Wl,--export-all \
      -o /tmp/mock_memfs.wasm tests/vibe/mock_memfs.c
```

Verify the magic header:

```bash
od -A n -t x1 -N 4 /tmp/mock_memfs.wasm
# expected:  00 61 73 6d
```

## CI integration

Add to your CI pipeline (GitHub Actions example):

```yaml
- name: Vibe integration test
  run: |
    chmod +x tests/vibe_integration_test.sh
    ./tests/vibe_integration_test.sh
  env:
    AGENTOS_CODEGEN_BACKEND: mock
```

The test exits 0 (pass) in mock mode even without an LLM or QEMU, making it
safe to run on every pull request.  Enable a full LLM run in a separate
nightly job by adding `ANTHROPIC_API_KEY` or `OPENAI_API_KEY` as a CI secret.
