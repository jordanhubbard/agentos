//! Scheduling contracts for agents
//!
//! Agents declare their scheduling requirements at spawn time.
//! agentOS allocates CPU budgets accordingly.

/// A scheduling contract
#[derive(Debug, Clone)]
pub struct SchedulingContract {
    pub class: SchedulingClass,
    pub priority: u8,
    pub cpu_budget_us: Option<u64>,   // microseconds per period
    pub period_us: Option<u64>,       // period length  
    pub deadline_ns: Option<u64>,     // absolute deadline (for EDF)
}

impl SchedulingContract {
    pub fn interactive() -> Self {
        Self {
            class: SchedulingClass::Interactive,
            priority: 128,
            cpu_budget_us: None,
            period_us: None,
            deadline_ns: None,
        }
    }
    
    pub fn compute(priority: u8) -> Self {
        Self {
            class: SchedulingClass::Compute,
            priority,
            cpu_budget_us: None,
            period_us: None,
            deadline_ns: None,
        }
    }
    
    pub fn realtime(budget_us: u64, period_us: u64) -> Self {
        Self {
            class: SchedulingClass::RealTime,
            priority: 255,
            cpu_budget_us: Some(budget_us),
            period_us: Some(period_us),
            deadline_ns: None,
        }
    }
    
    pub fn background() -> Self {
        Self {
            class: SchedulingClass::Background,
            priority: 0,
            cpu_budget_us: None,
            period_us: None,
            deadline_ns: None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SchedulingClass {
    /// Hard real-time (seL4 MCS sporadic server)
    RealTime,
    /// Low-latency interactive (event-driven)
    Interactive,
    /// Long-running compute (inference, batch)
    Compute,
    /// Background maintenance
    Background,
}
