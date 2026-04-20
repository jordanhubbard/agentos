use std::string::String;

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

/// Cumulative count of events processed by this agent.
static mut EVENT_COUNT: u32 = 0;

/// Log a string slice via the host `aos_log` import.
fn log_str(s: &str) {
    unsafe { aos_log(s.as_ptr(), s.len()) }
}

/// Format a u32 as decimal and append to `buf`.  No heap allocation.
fn push_u32(buf: &mut String, mut n: u32) {
    if n == 0 {
        buf.push('0');
        return;
    }
    // Collect digits in reverse order.
    let mut digits = [0u8; 10];
    let mut len = 0usize;
    while n > 0 {
        digits[len] = (n % 10) as u8;
        n /= 10;
        len += 1;
    }
    for i in (0..len).rev() {
        buf.push((b'0' + digits[i]) as char);
    }
}

/// Called once when the agent is loaded.
/// Returns 0 on success.
#[no_mangle]
pub extern "C" fn init() -> i32 {
    log_str("log-aggregator: ready");
    log_str("log-aggregator: subscribing to agent.*");
    0
}

/// Liveness probe.  Returns 0 (healthy).
#[no_mangle]
pub extern "C" fn health_check() -> i32 {
    0
}

/// Microkit protected-procedure-call handler.
///
/// * `label == 2` → QUERY_COUNT: place EVENT_COUNT into MR0 and return 0.
/// * anything else → return -1 (unhandled).
#[no_mangle]
pub extern "C" fn handle_ppc(
    label: i64,
    _mr1: i64,
    _mr2: i64,
    _mr3: i64,
    _mr4: i64,
) -> i64 {
    const QUERY_COUNT: i64 = 2;
    if label == QUERY_COUNT {
        unsafe { microkit_mr_set(0, EVENT_COUNT as i64) };
        0
    } else {
        -1
    }
}

/// Notification handler — increments the counter and logs the new total.
#[no_mangle]
pub extern "C" fn notified(_channels: u32) {
    let total = unsafe {
        EVENT_COUNT += 1;
        EVENT_COUNT
    };
    let mut msg = String::from("log-aggregator: event received, total=");
    push_u32(&mut msg, total);
    log_str(&msg);
}

