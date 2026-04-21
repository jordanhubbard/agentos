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

#[cfg(test)]
mod tests {
    use super::*;

    // ── SchedulingContract constructors ───────────────────────────────────────

    #[test]
    fn interactive_contract_class_and_priority() {
        let sc = SchedulingContract::interactive();
        assert_eq!(sc.class, SchedulingClass::Interactive);
        assert_eq!(sc.priority, 128);
        assert!(sc.cpu_budget_us.is_none());
        assert!(sc.period_us.is_none());
        assert!(sc.deadline_ns.is_none());
    }

    #[test]
    fn compute_contract_stores_priority() {
        let sc = SchedulingContract::compute(64);
        assert_eq!(sc.class, SchedulingClass::Compute);
        assert_eq!(sc.priority, 64);
        assert!(sc.cpu_budget_us.is_none());
    }

    #[test]
    fn realtime_contract_stores_budget_and_period() {
        let sc = SchedulingContract::realtime(500, 1000);
        assert_eq!(sc.class, SchedulingClass::RealTime);
        assert_eq!(sc.priority, 255);
        assert_eq!(sc.cpu_budget_us, Some(500));
        assert_eq!(sc.period_us, Some(1000));
        assert!(sc.deadline_ns.is_none());
    }

    #[test]
    fn realtime_utilization_within_bounds() {
        // Budget must be <= period for a valid sporadic server
        let sc = SchedulingContract::realtime(200, 1000);
        let budget = sc.cpu_budget_us.unwrap();
        let period = sc.period_us.unwrap();
        assert!(budget <= period, "budget {budget} must not exceed period {period}");
    }

    #[test]
    fn background_contract_lowest_priority() {
        let sc = SchedulingContract::background();
        assert_eq!(sc.class, SchedulingClass::Background);
        assert_eq!(sc.priority, 0);
        assert!(sc.cpu_budget_us.is_none());
        assert!(sc.period_us.is_none());
    }

    // ── SchedulingClass ordering / distinctness ───────────────────────────────

    #[test]
    fn scheduling_classes_are_distinct() {
        assert_ne!(SchedulingClass::RealTime, SchedulingClass::Interactive);
        assert_ne!(SchedulingClass::Interactive, SchedulingClass::Compute);
        assert_ne!(SchedulingClass::Compute, SchedulingClass::Background);
    }
}
