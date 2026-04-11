//! Multi-agent orchestration for agentos-sim.
//!
//! [`SimOrchestrator`] connects multiple [`AgentRunner`] instances via a
//! shared channel-route table.  When agent A calls
//! `microkit_ppcall(channel=5, …)` and channel 5 is mapped to agent B, the
//! orchestrator:
//!
//! 1. Takes the MR values passed by the caller as the "outgoing" MRs from A.
//! 2. Calls B's `handle_ppc(mr0, mr1, mr2, mr3, mr4)`.
//! 3. Reads B's reply MRs from B's shim.
//! 4. Pre-loads those reply MRs into A's shim via `enqueue_reply` so that
//!    when A's own WASM fires `microkit_ppcall` it gets the correct reply.
//! 5. Returns the reply label to the external caller.
//!
//! Because the orchestrator's [`ppcall`] method is the *external* entry
//! point (not triggered by a live WASM execution), the design avoids any
//! re-entrancy concern: each agent's store is mutated in turn, never
//! concurrently.
//!
//! [`ppcall`]: SimOrchestrator::ppcall

use std::collections::HashMap;

use anyhow::{bail, Result};

use crate::microkit::{MsgInfo, PpcResult, MR_COUNT};
use crate::runner::AgentRunner;
use crate::SimEngine;

// ── Route table ──────────────────────────────────────────────────────────────

/// A single directed route: calls from `from_agent` on `channel` are
/// delivered to `to_agent`.
#[derive(Debug, Clone)]
pub struct ChannelRoute {
    /// Name of the source agent.
    pub from_agent: String,
    /// Channel ID in the source agent's namespace.
    pub channel: u32,
    /// Name of the target agent.
    pub to_agent: String,
}

// ── SimOrchestrator ───────────────────────────────────────────────────────────

/// Orchestrates multiple simulated agents connected by channel routes.
pub struct SimOrchestrator {
    engine:  SimEngine,
    runners: HashMap<String, AgentRunner>,
    routes:  Vec<ChannelRoute>,
}

impl SimOrchestrator {
    /// Create an empty orchestrator with a fresh [`SimEngine`].
    pub fn new() -> Self {
        Self {
            engine:  SimEngine::new(),
            runners: HashMap::new(),
            routes:  Vec::new(),
        }
    }

    /// Compile and register a WASM agent under the given name.
    ///
    /// Default capabilities are granted for the agent.
    pub fn add_agent(&mut self, name: &str, wasm_bytes: &[u8]) -> Result<()> {
        let runner = self.engine.spawn_agent(name, wasm_bytes)?;
        self.runners.insert(name.to_owned(), runner);
        Ok(())
    }

    /// Add a channel route: ppcalls from `from` on `channel` → `to`.
    pub fn route(&mut self, from: &str, channel: u32, to: &str) {
        self.routes.push(ChannelRoute {
            from_agent: from.to_owned(),
            channel,
            to_agent: to.to_owned(),
        });
    }

    /// Call `init()` on every registered agent.
    pub fn init_all(&mut self) -> Result<()> {
        // Collect names first to avoid borrow-checker issues with the map.
        let names: Vec<String> = self.runners.keys().cloned().collect();
        for name in names {
            let runner = self.runners.get_mut(&name).unwrap();
            runner.init()?;
        }
        Ok(())
    }

    /// Deliver a protected procedure call from `from_agent` on `channel`.
    ///
    /// `mr0`–`mr3` are the outgoing message registers set by the caller.
    ///
    /// If a route exists the call is forwarded to the target agent:
    /// * The target's `handle_ppc` is invoked with the supplied MRs.
    /// * The target's reply MRs are pre-loaded into the source agent's shim
    ///   via [`enqueue_reply`] so that the source's in-WASM `microkit_ppcall`
    ///   will receive the correct reply when it fires.
    /// * The reply label is returned to the external caller.
    ///
    /// If no route is found, an error is returned.
    ///
    /// [`enqueue_reply`]: crate::microkit::MicrokitShim::enqueue_reply
    pub fn ppcall(
        &mut self,
        from_agent: &str,
        channel: u32,
        mr0: i64,
        mr1: i64,
        mr2: i64,
        mr3: i64,
    ) -> Result<i64> {
        // Look up the route.
        let to_agent = self
            .routes
            .iter()
            .find(|r| r.from_agent == from_agent && r.channel == channel)
            .map(|r| r.to_agent.clone())
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "no route from agent '{}' on channel {}",
                    from_agent,
                    channel
                )
            })?;

        // Validate both agents exist before mutating anything.
        if !self.runners.contains_key(from_agent) {
            bail!("unknown source agent '{}'", from_agent);
        }
        if !self.runners.contains_key(&to_agent) {
            bail!("unknown target agent '{}'", to_agent);
        }

        // ── Step 1: prime the target agent's incoming MRs ────────────────
        //
        // Microkit's ABI puts MRs in the shared register file *before* the
        // callee is entered.  We mirror that by writing the caller's MRs into
        // the target shim's register file directly, then calling handle_ppc
        // with the same values (handle_ppc takes them as parameters *and*
        // the agent may read them via microkit_mr_get).
        {
            let target = self.runners.get_mut(&to_agent).unwrap();
            let regs = &mut target.state_mut().shim.regs;
            regs[0] = mr0 as u64;
            regs[1] = mr1 as u64;
            regs[2] = mr2 as u64;
            regs[3] = mr3 as u64;
            // Clear higher MRs so there are no stale values.
            for r in &mut regs[4..] { *r = 0; }
        }

        // ── Step 2: call target's handle_ppc ─────────────────────────────
        {
            let target = self.runners.get_mut(&to_agent).unwrap();
            target.handle_ppc(mr0, mr1, mr2, mr3, 0)?;
        }

        // ── Step 3: harvest target's reply MRs ───────────────────────────
        let reply_regs: [u64; MR_COUNT] = {
            let target = self.runners.get(&to_agent).unwrap();
            target.state().shim.regs
        };
        let reply_label = reply_regs[0]; // convention: label in MR0

        // ── Step 4: pre-load reply into source agent's shim ──────────────
        //
        // When source's WASM calls microkit_ppcall, the shim will check the
        // reply_queues first (before the handler map), so the pre-loaded
        // PpcResult will be returned without actually invoking any further
        // cross-agent routing.
        {
            let result = PpcResult {
                info: MsgInfo::new(reply_label, MR_COUNT as u8),
                regs: reply_regs,
            };
            let src = self.runners.get_mut(from_agent).unwrap();
            src.state_mut().shim.enqueue_reply(channel, result);
        }

        Ok(reply_label as i64)
    }

    /// Inject a notification into `agent`'s shim and immediately drain it.
    ///
    /// This delivers `notified(channel)` to the agent's WASM.
    pub fn notify(&mut self, agent: &str, channel: u32) -> Result<()> {
        let runner = self
            .runners
            .get_mut(agent)
            .ok_or_else(|| anyhow::anyhow!("unknown agent '{}'", agent))?;

        runner.state_mut().shim.inject_notification(channel);
        runner.drain_notifications()
    }

    /// Immutable access to a runner by name (for assertions in tests).
    pub fn agent(&self, name: &str) -> Option<&AgentRunner> {
        self.runners.get(name)
    }
}

impl Default for SimOrchestrator {
    fn default() -> Self { Self::new() }
}
