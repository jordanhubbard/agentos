//! Cooperative scheduling with a fixed number of task slots.
//!
//! Each run examines at most N slots and polls each ready task at most once,
//! subject to the caller's poll budget. Futures must return from `poll` without
//! blocking: a poll budget cannot preempt arbitrary user code. Wakeups mark
//! readiness; the embedding IPC/notification loop supplies platform wakeups.
//! Futures and reference-counted wake state use the PD's global allocator.

use alloc::{boxed::Box, sync::Arc, task::Wake};
use core::{
    fmt,
    future::Future,
    pin::Pin,
    sync::atomic::{AtomicBool, Ordering},
    task::{Context, Waker},
};

pub type BoxFuture<'a> = Pin<Box<dyn Future<Output = ()> + 'a>>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
/// Identifies one task incarnation within its originating executor.
pub struct TaskId(u64);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpawnFailure {
    Full,
    IdExhausted,
}

pub struct SpawnError<'a> {
    pub reason: SpawnFailure,
    /// Ownership is returned on rejection; the future has not been polled.
    pub future: BoxFuture<'a>,
}

impl fmt::Debug for SpawnError<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SpawnError")
            .field("reason", &self.reason)
            .finish_non_exhaustive()
    }
}

struct Ready(AtomicBool);
impl Wake for Ready {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }
    fn wake_by_ref(self: &Arc<Self>) {
        self.0.store(true, Ordering::Release);
    }
}

struct Task<'a> {
    id: TaskId,
    future: BoxFuture<'a>,
    ready: Arc<Ready>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RunStats {
    pub polls: usize,
    pub completed: usize,
    pub remaining: usize,
    pub has_ready: bool,
}

pub struct Executor<'a, const N: usize> {
    tasks: [Option<Task<'a>>; N],
    next_id: Option<u64>,
    cursor: usize,
    count: usize,
}

impl<'a, const N: usize> Executor<'a, N> {
    pub fn new() -> Self {
        Self {
            tasks: core::array::from_fn(|_| None),
            next_id: Some(1),
            cursor: 0,
            count: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.count
    }
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }
    pub fn has_ready(&self) -> bool {
        self.tasks
            .iter()
            .flatten()
            .any(|task| task.ready.0.load(Ordering::Acquire))
    }

    pub fn spawn(&mut self, future: BoxFuture<'a>) -> Result<TaskId, SpawnError<'a>> {
        let Some(index) = self.tasks.iter().position(Option::is_none) else {
            return Err(SpawnError {
                reason: SpawnFailure::Full,
                future,
            });
        };
        let Some(id) = self.next_id else {
            return Err(SpawnError {
                reason: SpawnFailure::IdExhausted,
                future,
            });
        };
        // Every incarnation owns a distinct wake state. A retained waker from
        // a cancelled/completed task cannot wake a later occupant of its slot.
        self.tasks[index] = Some(Task {
            id: TaskId(id),
            future,
            ready: Arc::new(Ready(AtomicBool::new(true))),
        });
        self.next_id = id.checked_add(1);
        self.count += 1;
        Ok(TaskId(id))
    }

    pub fn cancel(&mut self, id: TaskId) -> bool {
        let Some(index) = self
            .tasks
            .iter()
            .position(|task| task.as_ref().is_some_and(|task| task.id == id))
        else {
            return false;
        };
        self.tasks[index] = None;
        self.count -= 1;
        true
    }

    pub fn run_ready(&mut self, budget: usize) -> RunStats {
        let mut stats = RunStats::default();
        let mut examined = 0;
        while examined < N && stats.polls < budget {
            let index = self.cursor;
            self.cursor = (self.cursor + 1) % N;
            examined += 1;
            let Some(task) = self.tasks[index].as_mut() else {
                continue;
            };
            if !task.ready.0.swap(false, Ordering::AcqRel) {
                continue;
            }
            let waker = Waker::from(task.ready.clone());
            stats.polls += 1;
            if task
                .future
                .as_mut()
                .poll(&mut Context::from_waker(&waker))
                .is_ready()
            {
                self.tasks[index] = None;
                self.count -= 1;
                stats.completed += 1;
            }
        }
        stats.remaining = self.count;
        stats.has_ready = self.has_ready();
        stats
    }
}

impl<const N: usize> Default for Executor<'_, N> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::{sync::atomic::AtomicUsize, task::Poll};
    use std::sync::Mutex;

    struct Counting {
        polls: Arc<AtomicUsize>,
        drops: Arc<AtomicUsize>,
        self_wake: bool,
        finish: usize,
        saved: Arc<Mutex<Option<Waker>>>,
    }
    impl Future for Counting {
        type Output = ();
        fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
            let count = self.polls.fetch_add(1, Ordering::Relaxed) + 1;
            *self.saved.lock().unwrap() = Some(cx.waker().clone());
            if count == self.finish {
                return Poll::Ready(());
            }
            if self.self_wake {
                cx.waker().wake_by_ref();
            }
            Poll::Pending
        }
    }
    impl Drop for Counting {
        fn drop(&mut self) {
            self.drops.fetch_add(1, Ordering::Relaxed);
        }
    }
    fn counter(self_wake: bool, finish: usize) -> Counting {
        Counting {
            polls: Arc::new(AtomicUsize::new(0)),
            drops: Arc::new(AtomicUsize::new(0)),
            self_wake,
            finish,
            saved: Arc::new(Mutex::new(None)),
        }
    }

    #[test]
    fn budget_and_round_robin_prevent_self_wake_monopoly() {
        let mut executor = Executor::<2>::new();
        let a = counter(true, 3);
        let a_polls = a.polls.clone();
        let a_drops = a.drops.clone();
        let b = counter(true, 3);
        let b_polls = b.polls.clone();
        executor.spawn(Box::pin(a)).unwrap();
        executor.spawn(Box::pin(b)).unwrap();
        assert_eq!(executor.run_ready(0).polls, 0);
        assert_eq!(executor.run_ready(1).polls, 1);
        assert_eq!(executor.run_ready(1).polls, 1);
        assert_eq!(a_polls.load(Ordering::Relaxed), 1);
        assert_eq!(b_polls.load(Ordering::Relaxed), 1);
        assert_eq!(executor.run_ready(usize::MAX).polls, 2);
        let final_run = executor.run_ready(usize::MAX);
        assert_eq!(final_run.completed, 2);
        assert_eq!(final_run.remaining, 0);
        assert!(!final_run.has_ready);
        assert_eq!(a_drops.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn external_wakes_coalesce_and_stale_wakes_do_not_reach_replacement() {
        let mut executor = Executor::<1>::new();
        let old = counter(false, usize::MAX);
        let saved = old.saved.clone();
        let drops = old.drops.clone();
        let old_id = executor.spawn(Box::pin(old)).unwrap();
        assert_eq!(executor.run_ready(1).polls, 1);
        assert_eq!(executor.run_ready(1).polls, 0);
        let stale = saved.lock().unwrap().take().unwrap();
        stale.wake_by_ref();
        stale.wake_by_ref();
        assert_eq!(executor.run_ready(10).polls, 1);
        assert!(executor.cancel(old_id));
        assert_eq!(drops.load(Ordering::Relaxed), 1);
        let replacement = counter(false, usize::MAX);
        let replacement_polls = replacement.polls.clone();
        executor.spawn(Box::pin(replacement)).unwrap();
        assert_eq!(executor.run_ready(1).polls, 1);
        stale.wake_by_ref();
        assert_eq!(executor.run_ready(10).polls, 0);
        assert!(!executor.cancel(old_id));
        assert_eq!(replacement_polls.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn full_and_exhausted_executors_return_unpolled_future_ownership() {
        let mut executor = Executor::<1>::new();
        executor.next_id = Some(u64::MAX);
        let last = executor.spawn(Box::pin(core::future::pending())).unwrap();
        let future = counter(false, 1);
        let polls = future.polls.clone();
        let drops = future.drops.clone();
        let rejected = executor.spawn(Box::pin(future)).unwrap_err();
        assert_eq!(rejected.reason, SpawnFailure::Full);
        assert_eq!(polls.load(Ordering::Relaxed), 0);
        assert_eq!(drops.load(Ordering::Relaxed), 0);
        assert!(executor.cancel(last));
        let rejected = executor.spawn(rejected.future).unwrap_err();
        assert_eq!(rejected.reason, SpawnFailure::IdExhausted);
        assert_eq!(drops.load(Ordering::Relaxed), 0);
        drop(rejected);
        assert_eq!(drops.load(Ordering::Relaxed), 1);
        assert!(executor.is_empty());
    }

    #[test]
    fn zero_capacity_is_valid_and_never_polls() {
        let mut executor = Executor::<0>::new();
        assert_eq!(
            executor
                .spawn(Box::pin(core::future::ready(())))
                .unwrap_err()
                .reason,
            SpawnFailure::Full
        );
        assert_eq!(executor.run_ready(usize::MAX), RunStats::default());
    }
}
