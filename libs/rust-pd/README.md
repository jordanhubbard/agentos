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
bounded asynchronous execution
where needed, virtualizer network bindings and a real native RCC service.

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
