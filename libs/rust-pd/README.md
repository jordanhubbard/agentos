# Rust PD runtime

This crate is a runtime under development. Its existing `export_pd!` macro
exports Microkit callbacks. The current agentOS root task starts services via
`pd_entry.c` and `pd_main(endpoint, nameserver)` and does not link libmicrokit.
The callback template alone therefore does not produce a bootable agentOS PD.
For the native entry path, use `tests/native-rust/probe` as the minimal example:
it exports `pd_main`, links the agentOS entry and IPC-buffer initialization,
and serves synchronous requests through `agentos_pd::runtime`.

Message-register access uses `runtime/ipc.c`, a linkable C bridge around the
SDK's static-inline functions. Rust checks indices before entering FFI. The
bridge also rejects invalid indices and asserts the supported 64-bit ABI and
120-word buffer against the actual SDK headers. The runtime must initialize
the current thread's IPC-buffer pointer before any access.

`make test-rust-pd-abi` runs host behavior tests and compiles this bridge against
the AArch64 and x86_64 SDKs. It proves neither target IPC delivery nor PD boot.
`make test-native-rust` builds and boots the native AArch64 example, then a
separate C PD checks its root-minted caller badge, all 120 message words,
version/length/opcode errors and valid calls after errors. It also validates
seeded `alloc::Vec` contents, page alignment, exhaustion and complete reuse
of the probe's private 64 KiB heap. Install Rust's
`aarch64-unknown-none` standard-library target before running the target build.
The full runtime task, `task_3d190486ab18c12663a2d724bb602778`, also requires
virtualizer network bindings and integrated on-target validation in an agentOS
native PD. The migrated requirement to port an external RCC service is obsolete.

`heap::BoundedHeap<N>` supplies a fixed-capacity global allocator with `N`
64-byte blocks. Declare it at a stable static address using `#[global_allocator]`.
Metadata is separate from payloads, freed adjacent spans can be reused, and
unsupported sizes or unavailable aligned spans return null. Search work and
memory are bounded; contention wait on the metadata spin lock is not a
real-time guarantee. The current PD runtime is single-threaded and must not
reenter allocation from an interrupt. No page allocation, IPC or device access
occurs inside the allocator.

Bare-metal AArch64 Rust compilation uses baseline instructions rather than
assuming Cortex-A55 features. The target proof runs on the harness's Cortex-A57,
including the allocator's atomic locking operations.

`executor::Executor<N>` owns at most `N` boxed futures. Each `run_ready(budget)`
examines at most `N` slots and polls each ready task at most once, up to the
budget. A rotating cursor prevents a repeatedly waking task from monopolizing
small budgets. Capacity rejection returns the unpolled future to its owner.
Task IDs are scoped to their originating executor and never wrap into reused
IDs. Cancellation drops the future; retained old wakers cannot mark a later
slot occupant ready.

This is cooperative scheduling: futures must return promptly from `poll`.
The poll budget cannot preempt a blocking or malicious future, and wakers only
mark readiness. The embedding IPC/notification loop must provide platform
wakeups and drive the executor. Task/waker allocations use the PD's global heap.
The runtime crate emits an `rlib`; the final PD crate owns its allocator and
emits the static library linked with the C entry and IPC bridge.

The target proof runs real `async` functions that yield twice, checks admission
and poll budgets, rejects stale cancellation, and then exhausts/reuses the
whole heap to verify executor-owned allocations were released.

`network::Client` copies packets through the existing sDDF free/active queues
in one isolated client page. It checks snapshotted descriptor offsets, lengths
and queue occupancy before accessing payloads. Full queues return `WouldBlock`;
a short receive buffer preserves the pending packet. `make test-rust-pd-abi`
exchanges full queues with the production C virtualizer pump, including exact
payload comparison and repeated buffer recycling. This is host interoperability
evidence. `make test-native-rust` additionally assigns client 2 its own page and
network-only attach authority, then verifies three NIC ARP replies at its
assigned address. The service sleeps on a receive-only notification between
requests; queue kicks use a send-only notification, preserving wakeups while
the peer is busy. No guest page, driver-transfer page, MMIO or hardware IRQ is
granted to the Rust client. Link `runtime/network.c` for control and notification
bridges. `initialize_and_attach` requires an exclusively owned, quiescent page;
it must never reset a page already in use by the virtualizer.

The proof covers raw Ethernet and ARP through the real driver.
`make test-native-network-isolation` additionally boots ten separate images:
read and write attempts against each guest queue page, the driver-transfer page,
NIC MMIO and driver DMA. Each first proves the owned network path. The root
task then requires the exact native fault badge, address, data-fault class and
read/write direction before emitting success. The native PD has no logging
capability with which to forge that marker. Each tested image is retained in
`build/evidence/native-network-isolation/`. A production network stack and
native/guest concurrent traffic remain separate qualification work.
