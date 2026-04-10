//! wasmi-based WASM agent runner.
//!
//! Instantiates a compiled `.wasm` module and drives the standard
//! agentOS agent lifecycle:
//!
//!   1. Link host imports ("env" namespace)
//!   2. Call `init()`
//!   3. Optionally call `handle_ppc(i64,i64,i64,i64,i64)`
//!   4. Optionally call `health_check() → i32`
//!   5. Deliver pending notifications via `notified(channel: i32)`

use std::sync::{Arc, Mutex};

use anyhow::{Context, Result, bail};
use wasmi::{Engine, Linker, Module, Store, Caller, TypedFunc, AsContext};
use tracing::{info, warn};

use crate::microkit::MicrokitShim;
use crate::eventbus::SimEventBus;
use crate::caps::SimCapStore;

/// State threaded through all wasmi host calls.
pub struct AgentState {
    pub name:     String,
    pub shim:     MicrokitShim,
    pub eventbus: Arc<Mutex<SimEventBus>>,
    pub caps:     Arc<Mutex<SimCapStore>>,
    /// Linear memory of the WASM instance (set after instantiation)
    pub memory:   Option<wasmi::Memory>,
    /// Simulated boot time counter in µs
    pub boot_us:  u64,
    /// Captured `aos_log` output
    pub log_lines: Vec<String>,
}

impl AgentState {
    pub fn new(
        name: impl Into<String>,
        eventbus: Arc<Mutex<SimEventBus>>,
        caps: Arc<Mutex<SimCapStore>>,
    ) -> Self {
        Self {
            name: name.into(),
            shim: MicrokitShim::new(),
            eventbus,
            caps,
            memory: None,
            boot_us: 0,
            log_lines: Vec::new(),
        }
    }
}

/// A running WASM agent instance.
pub struct AgentRunner {
    store:    Store<AgentState>,
    init_fn:  Option<TypedFunc<(), ()>>,
    ppc_fn:   Option<TypedFunc<(i64, i64, i64, i64, i64), ()>>,
    health_fn:Option<TypedFunc<(), i32>>,
    notif_fn: Option<TypedFunc<i32, ()>>,
}

impl AgentRunner {
    /// Compile and instantiate a WASM module from raw bytes.
    pub fn new(
        wasm_bytes: &[u8],
        state: AgentState,
    ) -> Result<Self> {
        let engine = Engine::default();
        let module = Module::new(&engine, wasm_bytes)
            .context("failed to compile WASM module")?;

        let mut store = Store::new(&engine, state);
        let mut linker = Linker::<AgentState>::new(&engine);

        // ── Host imports ("env" namespace) ────────────────────────────

        // aos_log(ptr: i32, len: i32)
        linker.func_wrap("env", "aos_log", |mut caller: Caller<AgentState>, ptr: i32, len: i32| {
            let mem = match caller.data().memory {
                Some(m) => m,
                None => { return; }
            };
            let data = mem.data(caller.as_context());
            let start = ptr as usize;
            let end = start.saturating_add(len as usize).min(data.len());
            if start <= end {
                let s = String::from_utf8_lossy(&data[start..end]).into_owned();
                let name = caller.data().name.clone();
                info!("[{}] {}", name, s.trim_end());
                caller.data_mut().log_lines.push(s);
            }
        })?;

        // aos_time_us() → i64
        linker.func_wrap("env", "aos_time_us", |caller: Caller<AgentState>| -> i64 {
            caller.data().boot_us as i64
        })?;

        // aos_mem_read(addr: i32, buf_ptr: i32, len: i32) → i32
        linker.func_wrap("env", "aos_mem_read", |_caller: Caller<AgentState>, _addr: i32, _buf: i32, _len: i32| -> i32 {
            // Simulation: shared memory not implemented; return 0 bytes read
            0i32
        })?;

        // aos_mem_write(addr: i32, buf_ptr: i32, len: i32) → i32
        linker.func_wrap("env", "aos_mem_write", |_caller: Caller<AgentState>, _addr: i32, _buf: i32, _len: i32| -> i32 {
            0i32
        })?;

        // microkit_mr_set(mr: i32, value: i64)
        linker.func_wrap("env", "microkit_mr_set", |mut caller: Caller<AgentState>, mr: i32, value: i64| {
            caller.data_mut().shim.mr_set(mr as usize, value as u64);
        })?;

        // microkit_mr_get(mr: i32) → i64
        linker.func_wrap("env", "microkit_mr_get", |caller: Caller<AgentState>, mr: i32| -> i64 {
            caller.data().shim.mr_get(mr as usize) as i64
        })?;

        // microkit_ppcall(channel: i32, label: i64, mr_count: i32) → i64
        linker.func_wrap("env", "microkit_ppcall", |mut caller: Caller<AgentState>, channel: i32, label: i64, mr_count: i32| -> i64 {
            use crate::microkit::MsgInfo;
            let info = MsgInfo::new(label as u64, mr_count as u8);
            let reply = caller.data_mut().shim.ppcall(channel as u32, info);
            reply.label as i64
        })?;

        // microkit_notify(channel: i32)
        linker.func_wrap("env", "microkit_notify", |mut caller: Caller<AgentState>, channel: i32| {
            caller.data_mut().shim.notify(channel as u32);
        })?;

        // aos_event_publish(topic_ptr: i32, topic_len: i32, data_ptr: i32, data_len: i32)
        linker.func_wrap("env", "aos_event_publish", |caller: Caller<AgentState>, tp: i32, tl: i32, dp: i32, dl: i32| {
            let mem = match caller.data().memory { Some(m) => m, None => return };
            let data_snapshot = mem.data(caller.as_context()).to_vec();
            let read_str = |ptr: usize, len: usize| -> String {
                let end = ptr.saturating_add(len).min(data_snapshot.len());
                String::from_utf8_lossy(&data_snapshot[ptr.min(data_snapshot.len())..end]).into_owned()
            };
            let topic   = read_str(tp as usize, tl as usize);
            let payload = {
                let s = dp as usize;
                let e = s.saturating_add(dl as usize).min(data_snapshot.len());
                data_snapshot[s.min(data_snapshot.len())..e].to_vec()
            };
            let name = caller.data().name.clone();
            let bus = caller.data().eventbus.clone();
            bus.lock().unwrap().publish(topic, payload, Some(name));
        })?;

        // ── Instantiate ───────────────────────────────────────────────
        let instance = linker.instantiate(&mut store, &module)
            .context("failed to instantiate WASM module")?
            .start(&mut store)
            .context("failed to run WASM start function")?;

        // Grab exported linear memory
        if let Some(mem) = instance.get_memory(&store, "memory") {
            store.data_mut().memory = Some(mem);
        }

        // Resolve exported functions (all optional)
        let init_fn   = instance.get_typed_func::<(), ()>(&store, "init").ok();
        let ppc_fn    = instance.get_typed_func::<(i64,i64,i64,i64,i64), ()>(&store, "handle_ppc").ok();
        let health_fn = instance.get_typed_func::<(), i32>(&store, "health_check").ok();
        let notif_fn  = instance.get_typed_func::<i32, ()>(&store, "notified").ok();

        if init_fn.is_none()   { warn!("WASM module has no exported 'init' function"); }
        if health_fn.is_none() { warn!("WASM module has no exported 'health_check' function"); }

        Ok(Self { store, init_fn, ppc_fn, health_fn, notif_fn })
    }

    /// Call `init()` — the agent's one-time boot hook.
    pub fn init(&mut self) -> Result<()> {
        if let Some(f) = self.init_fn {
            f.call(&mut self.store, ()).context("init() trapped")
        } else {
            Ok(())
        }
    }

    /// Call `handle_ppc(mr0..mr4)` — simulate an incoming IPC call.
    pub fn handle_ppc(&mut self, mr0: i64, mr1: i64, mr2: i64, mr3: i64, mr4: i64) -> Result<()> {
        match self.ppc_fn {
            Some(f) => f.call(&mut self.store, (mr0, mr1, mr2, mr3, mr4))
                         .context("handle_ppc() trapped"),
            None => bail!("WASM module has no 'handle_ppc' export"),
        }
    }

    /// Call `health_check()` — returns 0 for healthy.
    pub fn health_check(&mut self) -> Result<i32> {
        match self.health_fn {
            Some(f) => f.call(&mut self.store, ()).context("health_check() trapped"),
            None    => Ok(0), // no export → assume healthy
        }
    }

    /// Deliver a pending notification via `notified(channel)`.
    pub fn deliver_notification(&mut self, channel: u32) -> Result<()> {
        match self.notif_fn {
            Some(f) => f.call(&mut self.store, channel as i32).context("notified() trapped"),
            None    => Ok(()),
        }
    }

    /// Deliver all pending notifications queued in the shim.
    pub fn drain_notifications(&mut self) -> Result<()> {
        while let Some(ch) = self.store.data_mut().shim.pop_notification() {
            self.deliver_notification(ch)?;
        }
        Ok(())
    }

    /// Borrow the agent state for inspection/mutation.
    pub fn state(&self) -> &AgentState { self.store.data() }
    pub fn state_mut(&mut self) -> &mut AgentState { self.store.data_mut() }
}
