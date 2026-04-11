//! Integration tests for agentos-sim.
//!
//! Uses an embedded minimal WASM binary (237 bytes, hand-assembled) that
//! exercises all host-import paths:
//!   - aos_log(ptr: i32, len: i32)
//!   - aos_event_publish(topic_ptr, topic_len, data_ptr, data_len: i32)
//!   - microkit_ppcall(channel: i32, label: i64, mr_count: i32) -> i64
//!
//! The module exports: memory, init(), handle_ppc(i64×5), health_check()->i32.
//!
//! WASM linear memory layout initialised by the data section:
//!   [0..4]   = "init"   — logged by init() via aos_log
//!   [16..21] = "hello"  — event topic published by init()
//!   [32..37] = "world"  — event payload published by init()
//!
//! Function behaviour:
//!   init():
//!     aos_log(0, 4)                       → appends "init" to state.log_lines
//!     aos_event_publish(16, 5, 32, 5)     → publishes topic="hello" payload=b"world"
//!
//!   handle_ppc(mr0, mr1, mr2, mr3, mr4):
//!     microkit_ppcall(0, mr0, 1)          → recorded in state.shim.call_log
//!
//!   health_check() -> i32:
//!     returns 0 (healthy)

use agentos_sim::SimEngine;

/// Minimal hand-assembled WASM module (237 bytes).
///
/// Section order (ascending IDs, as required by the spec):
///   1=type, 2=import, 3=function, 5=memory, 7=export, 10=code, 11=data
const HELLO_AGENT_WASM: &[u8] = &[
    // ── Magic + version ────────────────────────────────────────────────────
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    // ── Section 1: Type ────────────────────────────────────────────────────
    // 6 function types
    0x01, 0x23, 0x06,
      0x60, 0x00, 0x00,                                      // type 0: () -> ()
      0x60, 0x02, 0x7f, 0x7f, 0x00,                         // type 1: (i32,i32) -> ()
      0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x00,             // type 2: (i32,i32,i32,i32) -> ()
      0x60, 0x03, 0x7f, 0x7e, 0x7f, 0x01, 0x7e,             // type 3: (i32,i64,i32) -> i64
      0x60, 0x05, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x00,       // type 4: (i64×5) -> ()
      0x60, 0x00, 0x01, 0x7f,                                // type 5: () -> i32
    // ── Section 2: Import ──────────────────────────────────────────────────
    // 3 host functions
    0x02, 0x3d, 0x03,
      // env.aos_log : type 1
      0x03, 0x65, 0x6e, 0x76,
      0x07, 0x61, 0x6f, 0x73, 0x5f, 0x6c, 0x6f, 0x67,
      0x00, 0x01,
      // env.aos_event_publish : type 2
      0x03, 0x65, 0x6e, 0x76,
      0x11, 0x61, 0x6f, 0x73, 0x5f, 0x65, 0x76, 0x65, 0x6e, 0x74, 0x5f, 0x70, 0x75, 0x62,
            0x6c, 0x69, 0x73, 0x68,
      0x00, 0x02,
      // env.microkit_ppcall : type 3
      0x03, 0x65, 0x6e, 0x76,
      0x0f, 0x6d, 0x69, 0x63, 0x72, 0x6f, 0x6b, 0x69, 0x74, 0x5f, 0x70, 0x70, 0x63, 0x61, 0x6c, 0x6c,
      0x00, 0x03,
    // ── Section 3: Function ────────────────────────────────────────────────
    // 3 local funcs (type 0, 4, 5) → func indices 3, 4, 5
    0x03, 0x04, 0x03, 0x00, 0x04, 0x05,
    // ── Section 5: Memory ──────────────────────────────────────────────────
    0x05, 0x03, 0x01, 0x00, 0x01,
    // ── Section 7: Export ──────────────────────────────────────────────────
    0x07, 0x2d, 0x04,
      0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00,              // "memory"       mem[0]
      0x04, 0x69, 0x6e, 0x69, 0x74, 0x00, 0x03,                          // "init"         func[3]
      0x0a, 0x68, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x5f, 0x70, 0x70, 0x63, 0x00, 0x04,       // "handle_ppc"   func[4]
      0x0c, 0x68, 0x65, 0x61, 0x6c, 0x74, 0x68, 0x5f, 0x63, 0x68, 0x65, 0x63, 0x6b, 0x00, 0x05, // "health_check" func[5]
    // ── Section 10: Code ───────────────────────────────────────────────────
    0x0a, 0x25, 0x03,
      // body func[3] = init()
      0x12, 0x00,
        0x41, 0x00, 0x41, 0x04, 0x10, 0x00,                          // aos_log(0, 4)
        0x41, 0x10, 0x41, 0x05, 0x41, 0x20, 0x41, 0x05, 0x10, 0x01,  // aos_event_publish(16,5,32,5)
      0x0b,
      // body func[4] = handle_ppc(i64×5)
      0x0b, 0x00,
        0x41, 0x00,             // i32.const 0   (channel)
        0x20, 0x00,             // local.get 0   (mr0)
        0x41, 0x01,             // i32.const 1   (mr_count)
        0x10, 0x02,             // call microkit_ppcall
        0x1a,                   // drop i64 result
      0x0b,
      // body func[5] = health_check() -> i32
      0x04, 0x00, 0x41, 0x00, 0x0b,
    // ── Section 11: Data ───────────────────────────────────────────────────
    0x0b, 0x1e, 0x03,
      0x00, 0x41, 0x00, 0x0b, 0x04, 0x69, 0x6e, 0x69, 0x74,        // offset 0:  "init"
      0x00, 0x41, 0x10, 0x0b, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,  // offset 16: "hello"
      0x00, 0x41, 0x20, 0x0b, 0x05, 0x77, 0x6f, 0x72, 0x6c, 0x64,  // offset 32: "world"
];

// ── Smoke test: module loads and exports are present ─────────────────────────

#[test]
fn test_module_loads() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    engine.spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("WASM module should load without error");
}

// ── init() ───────────────────────────────────────────────────────────────────

#[test]
fn test_init_logs_message() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    let mut runner = engine
        .spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("spawn");

    runner.init().expect("init() should not trap");

    let log = &runner.state().log_lines;
    assert!(!log.is_empty(), "init() must produce at least one log line");
    assert!(
        log.iter().any(|l| l.contains("init")),
        "expected 'init' in log lines, got: {:?}",
        log
    );
}

#[test]
fn test_init_publishes_event() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    let mut runner = engine
        .spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("spawn");

    runner.init().expect("init() should not trap");

    let bus = engine.eventbus_arc();
    let bus = bus.lock().unwrap();

    assert!(
        bus.total_events() >= 1,
        "init() must publish at least one event, got {}",
        bus.total_events()
    );

    let events = bus.events_for("hello");
    assert!(
        !events.is_empty(),
        "expected an event on topic 'hello', history topics: {:?}",
        bus.history.iter().map(|e| &e.topic).collect::<Vec<_>>()
    );

    let first = &events[0];
    assert_eq!(
        first.payload, b"world",
        "event payload should be b\"world\", got {:?}", first.payload
    );
    assert_eq!(
        first.source.as_deref(),
        Some("hello-agent"),
        "event source should be 'hello-agent'"
    );
}

// ── handle_ppc() ─────────────────────────────────────────────────────────────

#[test]
fn test_handle_ppc_captured() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    let mut runner = engine
        .spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("spawn");

    // mr0 = 0xA0 = 160; the WASM passes this as the ppcall label
    runner.handle_ppc(0xA0, 1, 2, 3, 4).expect("handle_ppc() should not trap");

    let call_log = &runner.state().shim.call_log;
    assert_eq!(call_log.len(), 1, "exactly one ppcall should be recorded");

    let captured = &call_log[0];
    assert_eq!(captured.channel, 0, "ppcall channel should be 0");
    assert_eq!(
        captured.info.label, 0xA0,
        "ppcall label should equal mr0 (0xA0), got {:#x}",
        captured.info.label
    );
    assert_eq!(captured.info.mr_count, 1, "mr_count should be 1");
}

#[test]
fn test_handle_ppc_multiple_calls_accumulate() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    let mut runner = engine
        .spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("spawn");

    runner.handle_ppc(1, 0, 0, 0, 0).expect("first call");
    runner.handle_ppc(2, 0, 0, 0, 0).expect("second call");
    runner.handle_ppc(3, 0, 0, 0, 0).expect("third call");

    let call_log = &runner.state().shim.call_log;
    assert_eq!(call_log.len(), 3, "three ppcalls should accumulate in call_log");
    assert_eq!(call_log[0].info.label, 1);
    assert_eq!(call_log[1].info.label, 2);
    assert_eq!(call_log[2].info.label, 3);
}

// ── health_check() ───────────────────────────────────────────────────────────

#[test]
fn test_health_check_returns_zero() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    let mut runner = engine
        .spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("spawn");

    let result = runner.health_check().expect("health_check() should not trap");
    assert_eq!(result, 0, "health_check() should return 0 (healthy)");
}

// ── Full lifecycle ────────────────────────────────────────────────────────────

#[test]
fn test_full_lifecycle() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    let mut runner = engine
        .spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("spawn");

    // 1. Boot
    runner.init().expect("init");

    // 2. Simulate an IPC call with the task-spec mr values
    runner.handle_ppc(0xA0, 1, 2, 3, 4).expect("handle_ppc");

    // 3. Health probe
    let health = runner.health_check().expect("health_check");

    // ── Assertions ─────────────────────────────────────────────────────────

    // state.log_lines: init() logged "init"
    let log = &runner.state().log_lines;
    assert!(
        log.iter().any(|l| l.contains("init")),
        "log_lines should contain 'init', got: {:?}", log
    );

    // state.shim.call_log: handle_ppc invoked microkit_ppcall once with label=0xA0
    let call_log = &runner.state().shim.call_log;
    assert_eq!(call_log.len(), 1, "one ppcall expected");
    assert_eq!(call_log[0].info.label, 0xA0);

    // state.shim.notify_log: no notifications fired in this scenario
    let notify_log = &runner.state().shim.notify_log;
    assert!(
        notify_log.is_empty(),
        "no notifications should have been sent, got: {:?}", notify_log
    );

    // eventbus.history: init() published one event on topic "hello"
    {
        let bus = engine.eventbus_arc();
        let bus = bus.lock().unwrap();
        let events = bus.events_for("hello");
        assert_eq!(events.len(), 1, "exactly one 'hello' event expected");
        assert_eq!(events[0].payload, b"world");
    }

    // health_check returns 0
    assert_eq!(health, 0);
}

// ── Multiple independent agents share the same eventbus ──────────────────────

#[test]
fn test_two_agents_share_eventbus() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("agent-a");
    engine.caps_mut().grant_defaults("agent-b");

    let mut runner_a = engine
        .spawn_agent("agent-a", HELLO_AGENT_WASM)
        .expect("spawn agent-a");
    let mut runner_b = engine
        .spawn_agent("agent-b", HELLO_AGENT_WASM)
        .expect("spawn agent-b");

    runner_a.init().expect("agent-a init");
    runner_b.init().expect("agent-b init");

    let bus = engine.eventbus_arc();
    let bus = bus.lock().unwrap();

    // Both agents publish to "hello" in init(), so 2 events total
    assert_eq!(
        bus.total_events(), 2,
        "each agent publishes one event in init(); expected 2, got {}",
        bus.total_events()
    );

    let hello_events = bus.events_for("hello");
    assert_eq!(hello_events.len(), 2);

    let sources: Vec<_> = hello_events.iter()
        .filter_map(|e| e.source.as_deref())
        .collect();
    assert!(sources.contains(&"agent-a"), "agent-a should appear as source");
    assert!(sources.contains(&"agent-b"), "agent-b should appear as source");
}

// ── Notification delivery ─────────────────────────────────────────────────────

#[test]
fn test_inject_and_drain_notification() {
    let engine = SimEngine::new();
    engine.caps_mut().grant_defaults("hello-agent");
    let mut runner = engine
        .spawn_agent("hello-agent", HELLO_AGENT_WASM)
        .expect("spawn");

    // hello-agent does not export `notified`, so drain_notifications must
    // silently succeed (no-op) rather than erroring.
    runner.state_mut().shim.inject_notification(7);
    runner.drain_notifications()
        .expect("drain should not error even when 'notified' export is absent");
}
