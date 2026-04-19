# agentOS API Test Suite

Host-side unit tests for every agentOS service API.  Tests run entirely on
the host without real seL4 hardware by using the same MR-register mock that
the existing `tests/test_proc_server.c` test suite uses.

## Output format

Every test file emits [TAP (Test Anything Protocol)](https://testanything.org/)
output so it can be consumed by any TAP harness or piped directly to the
`xtask test-api` runner.

```
TAP version 14
1..6
ok 1 - subscribe valid topic
ok 2 - publish to subscribed topic
ok 3 - unsubscribe removes delivery
not ok 4 - publish to unknown topic returns ERR_NO_TOPIC
  # got 0x00, expected 0x01
ok 5 - subscribe duplicate returns same slot
ok 6 - publish oversized payload returns ERR_TOO_LARGE
```

## Coverage policy

- Every defined opcode in a service's IPC label space must have at least one
  `ok` test (valid-input / expected-output path).
- Every opcode must also have at least one test for invalid or missing input
  that verifies the correct error code is returned.
- New opcodes added to `contracts/<service>/interface.h` require corresponding
  tests before the PR may land.

## Running

```
# All API tests (via xtask):
cargo xtask test-api

# A single file directly:
cc -DAGENTOS_TEST_HOST -I tests/api -o /tmp/t tests/api/test_msgbus.c && /tmp/t

# Verbose TAP with a third-party harness (e.g. prove):
prove -e 'sh -c' tests/api/run_api_tests.sh
```

## Structure

```
tests/api/
  framework.h          -- shared TAP helpers + mock IPC layer
  test_msgbus.c        -- EventBus: SUBSCRIBE, PUBLISH, UNSUBSCRIBE, DRAIN
  test_capstore.c      -- CapabilityBroker: GRANT, REVOKE, CHECK, QUERY, AUDIT
  test_memfs.c         -- MemFS / ObjectStore: OPEN, CLOSE, READ, WRITE, SEEK,
                          STAT, UNLINK, MKDIR
  test_logsvc.c        -- LogSvc: LOG_WRITE, LOG_QUERY, LOG_FLUSH
  test_vibeos.c        -- VibeOS: VOS_CREATE, VOS_DESTROY, VOS_STATUS, VOS_LIST,
                          VOS_ATTACH, VOS_DETACH
```

## Adding a test

1. Open (or create) `tests/api/test_<service>.c`.
2. Add a `static void test_<opcode>_<scenario>(void)` function.
3. Increment the `TAP_PLAN(N)` counter at the top of `main()`.
4. Call your function from `main()`.
5. Run `cargo xtask test-api` and confirm `ALL TESTS PASSED`.
