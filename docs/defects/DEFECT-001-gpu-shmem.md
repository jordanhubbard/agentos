# DEFECT-001: GPU Shared Memory Channel — Approved Custom Implementation

**Status:** APPROVED EXCEPTION
**Date filed:** 2026-04-15
**Affected VMM:** Linux VMM (`kernel/agentos-root-task/src/linux_vmm.c`)
**Generic service rule:** Before any guest OS VMM implements its own device, it must
use the generic device service. A custom implementation requires an approved defect task.

---

## Description

The Linux VMM implements a custom inter-PD communication channel called `gpu_shmem`
(`kernel/agentos-root-task/src/gpu_shmem.c`, `kernel/agentos-root-task/include/gpu_shmem.h`).
This channel carries GPU tensor descriptors from seL4 native PDs (controller, worker,
swap_slot) into the Linux guest for dispatch to CUDA/PyTorch workloads running on the
NVIDIA GB10 SoC's 128GB unified VRAM.

The `linux_vmm` PD is wired into this channel as `GPU_SHMEM_ROLE_CONSUMER`:

- At `linux_vmm.c:346–356`: `gpu_shmem_init()` is called when the `gpu_tensor_buf`
  MR is mapped.
- At `linux_vmm.c:386–426` (`GPU_SHMEM_NOTIFY_IN_CH` handler): Tensor descriptors
  are dequeued from the ring and forwarded to the Linux guest's `gpu_shmem_linux`
  kernel module via virtual IRQ injection.
- At `linux_vmm.c:428–438` (`GPU_SHMEM_NOTIFY_OUT_CH` handler): Completion
  notifications from the Linux guest are relayed back to the controller.

---

## Justification: Why the Generic Services Cannot Substitute

### 1. `net-service` (NetServer) cannot carry GPU tensor traffic

NetServer manages virtual NICs, packet TX/RX rings, and TCP/UDP port binding. Its
shared memory layout (`net_packet_shmem`, 256KB) is designed for network packet
payloads (up to ~15KB per vNIC slot). GPU tensor operations require zero-copy transfer
of tensors that are routinely hundreds of megabytes to gigabytes. The `gpu_tensor_buf`
MR is 64MB and serves as a staging area for tensor payloads; this is four orders of
magnitude larger than a network packet. Encoding GPU tensor descriptors as network
packets would require fragmentation, reassembly, and sequencing logic that would
re-implement a reliable transport protocol — all to avoid using the correct primitive.

### 2. `block-service` (virtio_blk) cannot dispatch GPU operations

The `virtio_blk` PD exposes `OP_BLK_READ`, `OP_BLK_WRITE`, and `OP_BLK_FLUSH`
semantics against a sector-addressed block device. GPU operations are not
read/write/flush against LBA sectors:

- `GPU_OP_INFER`: Run a neural network forward pass on a tensor and return a result
  tensor. This involves dispatching to CUDA kernels, managing VRAM allocations, and
  returning a result asynchronously.
- `GPU_OP_COPY_IN` / `GPU_OP_COPY_OUT`: DMA between the seL4 shared MR and GPU VRAM.
- `GPU_OP_BARRIER`: Synchronization barrier across the seL4/Linux boundary.

None of these operations have meaningful block-device analogs. Encoding
`GPU_OP_INFER` as a series of sector reads and writes would impose a fixed-sector
framing on variable-length tensor shapes and require the VFS server to understand GPU
operation semantics, which violates separation of concerns.

### 3. `console_mux` (serial-mux) is a character stream multiplexer

`console_mux` manages byte-stream sessions over 4KB ring buffers. GPU tensor
descriptors are 64-byte structured records with alignment requirements and a
producer/consumer protocol built around cache-line-aligned ring heads and tails with
AArch64 DMB barriers. Encoding GPU descriptors as console character streams would
destroy alignment, break the barrier protocol, and require the Linux gpu_shmem kernel
module to re-parse an ad-hoc ASCII or binary encoding from the console ring — again
reimplementing the same ring protocol on top of an inappropriate substrate.

### 4. The gpu_shmem channel is the correct primitive

The `gpu_shmem` design is purpose-built for its task:

- **Zero-copy**: The 64MB MR is mapped directly into both the VMM PD and the
  controller/worker PDs. Tensor payload data is never copied; only the 64-byte
  descriptor (offset + size + shape + op) is passed through the ring.
- **Alignment-safe**: Descriptor slots are 64 bytes, cache-line aligned
  (`__attribute__((packed, aligned(64)))`). The ring header is also 64-byte aligned.
- **Barrier-correct**: Full AArch64 DMB barriers (`WMB()`, `RMB()`) are inserted at
  every producer/consumer hand-off point, which is required for correct operation on
  the weakly-ordered AArch64 memory model.
- **Type-safe for ML operations**: The `gpu_dtype_t` and `gpu_op_t` enumerations
  express the actual semantic operations (infer, copy, barrier) without losing
  information through translation to block or network abstractions.

---

## Scope of the Exception

This exception applies **only** to the `gpu_shmem` channel as implemented in:

- `kernel/agentos-root-task/src/gpu_shmem.c`
- `kernel/agentos-root-task/include/gpu_shmem.h`
- The `GPU_SHMEM_NOTIFY_IN_CH` and `GPU_SHMEM_NOTIFY_OUT_CH` notification handlers
  in `kernel/agentos-root-task/src/linux_vmm.c` (lines 386–438)

The exception does **not** cover:

- Any future VMM that uses `gpu_shmem` without the specific AArch64 GB10 / CUDA
  dispatch rationale. A RISC-V or x86 VMM targeting a different accelerator must file
  its own defect task.
- The use of `virq_inject(SERIAL_IRQ)` at `linux_vmm.c:423` as the mechanism for
  waking the Linux gpu_shmem driver. This is a known prototype shortcut (marked
  `TODO: dedicate a GPU shmem IRQ`) and must be replaced with a dedicated virtual IRQ
  before production. The `SERIAL_IRQ` reuse does not require a separate defect task
  because it is a prototype simplification within the already-approved custom channel,
  but it must not be carried forward to a production build.

---

## Conditions for Continued Approval

1. The `gpu_tensor_buf` MR must remain a fixed-size, Microkit-declared memory region
   with explicit capability grants from the root task. It must not be dynamically
   allocated or shared beyond the approved PDs (controller, worker, swap_slot,
   linux_vmm).

2. The ring protocol version (`gpu_shmem_ring_t.version == 1`) must be bumped and
   compatibility validation updated if the descriptor layout changes.

3. Before production, `virq_inject(SERIAL_IRQ)` at `linux_vmm.c:423` must be
   replaced with a dedicated virtual IRQ allocated for the GPU shmem completion path.

4. Any extension of the `gpu_shmem` channel to a FreeBSD VMM or a second Linux VMM
   instance requires a separate defect task or a formal generalization into a generic
   accelerator-dispatch service.
