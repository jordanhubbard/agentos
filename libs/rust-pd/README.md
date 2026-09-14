# Rust PD runtime

This crate is a runtime under development. Its existing `export_pd!` macro
exports Microkit callbacks. The current agentOS root task starts services via
`pd_entry.c` and `pd_main(endpoint, nameserver)` and does not link libmicrokit.
The callback template alone therefore does not produce a bootable agentOS PD.

Message-register access uses `runtime/ipc.c`, a linkable C bridge around the
SDK's static-inline functions. Rust checks indices before entering FFI. The
bridge also rejects invalid indices and asserts the supported 64-bit ABI and
120-word buffer against the actual SDK headers. The runtime must initialize
the current thread's IPC-buffer pointer before any access.

`make test-rust-pd-abi` runs host behavior tests and compiles this bridge against
the AArch64 and x86_64 SDKs. It proves neither target IPC delivery nor PD boot.
The full runtime task, `task_3d190486ab18c12663a2d724bb602778`, also requires
native entry/link integration, allocation, bounded asynchronous execution
where needed, virtualizer network bindings and a real native RCC service.
