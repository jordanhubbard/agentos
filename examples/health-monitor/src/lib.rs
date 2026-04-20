extern "C" {
    fn aos_log(ptr: *const u8, len: usize);
    fn aos_event_publish(
        topic_ptr: *const u8,
        topic_len: usize,
        payload_ptr: *const u8,
        payload_len: usize,
    );
    fn microkit_mr_set(mr: u32, val: i64);
    fn microkit_mr_get(mr: u32) -> i64;
}

/// Log a string slice via the host `aos_log` import.
fn log_str(s: &str) {
    unsafe { aos_log(s.as_ptr(), s.len()) }
}

/// Called once when the agent is loaded.
/// Returns 0 on success.
#[no_mangle]
pub extern "C" fn init() -> i32 {
    log_str("health-monitor: starting");
    0
}

/// Liveness probe — the host polls this periodically.
/// Returns 0 (healthy).
#[no_mangle]
pub extern "C" fn health_check() -> i32 {
    0
}

/// Microkit protected-procedure-call handler.
///
/// * `label == 1` → HEALTH_QUERY: publish to "agent.health" and return 0.
/// * anything else → return -1 (unhandled).
#[no_mangle]
pub extern "C" fn handle_ppc(
    label: i64,
    _mr1: i64,
    _mr2: i64,
    _mr3: i64,
    _mr4: i64,
) -> i64 {
    const HEALTH_QUERY: i64 = 1;
    if label == HEALTH_QUERY {
        let topic = "agent.health";
        let payload = "ok";
        unsafe {
            aos_event_publish(
                topic.as_ptr(),
                topic.len(),
                payload.as_ptr(),
                payload.len(),
            );
        }
        0
    } else {
        -1
    }
}

/// Notification handler — called when one or more notification channels fire.
#[no_mangle]
pub extern "C" fn notified(_channels: u32) {
    log_str("health-monitor: notification received");
}

