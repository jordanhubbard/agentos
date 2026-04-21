# agentOS VMM Device Audit — Task 8

**Date:** 2026-04-15
**Auditor:** Task 8 automated audit
**Scope:** Linux VMM (`kernel/agentos-root-task/src/linux_vmm.c`) and FreeBSD VMM
(`kernel/freebsd-vmm/vmm.c`, `kernel/freebsd-vmm/vmm_mux.c`) against the OS-neutral
generic device services.

---

## 1. Generic Services Inventory

Before examining the VMMs, these generic services are confirmed to exist in the tree:

| Service | Source File | Role |
|---|---|---|
| `net-service` (NetServer) | `kernel/agentos-root-task/src/net_server.c` | Virtual NIC management, packet TX/RX, ACL |
| `serial-mux` (console_mux) | `kernel/agentos-root-task/src/console_mux.c` | UART multiplexer, per-PD console rings, scrollback |
| `block-service` (virtio_blk) | `kernel/agentos-root-task/src/virtio_blk.c` | VirtIO-blk read/write/flush, vfs_server-facing IPC |
| `gpu-shmem` | `kernel/agentos-root-task/src/gpu_shmem.c` / `include/gpu_shmem.h` | Zero-copy tensor ring between seL4 PDs and Linux guest |

---

## 2. Device Audit Table

| Device Type | VMM | Generic Service Exists? | VMM Using Generic? | Verdict |
|---|---|---|---|---|
| UART / serial console | Linux VMM (AArch64) | YES — `console_mux` | NO — direct PL011 IRQ passthrough via `virq_register`/`virq_inject` | **Violation** — see §3.1 |
| UART / serial console | FreeBSD VMM | YES — `console_mux` | NO — raw PL011 UART at 0x09000000 passed direct to guest via DTS; console_mux not bound | **Violation** — see §3.2 |
| Block storage | Linux VMM (AArch64) | YES — `block-service` (`virtio_blk`) | NO — no block device defined; Linux guest gets no block device in current config | **Gap** — see §3.3 |
| Block storage | FreeBSD VMM | YES — `block-service` (`virtio_blk`) | NO — VirtIO block at guest PA 0x0a003000 is emulated inside the VMM PD; no IPC to `virtio_blk` PD | **Violation** — see §3.4 |
| Network (virtio-net) | Linux VMM (AArch64) | YES — `net-service` | NO — not present; Linux guest has no network device defined in current init | **Gap** — see §3.5 |
| Network (virtio-net) | FreeBSD VMM | YES — `net-service` | NO — VirtIO console at guest PA 0x0a004000 only; no net device exposed; VMM does not call NetServer | **Gap** — see §3.5 |
| GPU shared memory channel | Linux VMM (AArch64) | NO — purpose-built custom channel | YES — `gpu_shmem` is the custom service; linux_vmm consumes it correctly as `GPU_SHMEM_ROLE_CONSUMER` | **Approved exception** — DEFECT-001 |
| vCPU lifecycle / scheduling | Linux VMM + vm_manager | NO generic equivalent | N/A — vm_manager owns this; no generic service intended | N/A — correct design |
| UART / serial console | vm_manager (`OP_VM_CONSOLE`) | YES — `console_mux` | PARTIAL — `vmm_mux_switch` comments reference "multiplexed on single physical UART" without calling console_mux IPC | **Violation** — see §3.6 |

---

## 3. Violation Analysis

### 3.1 Linux VMM: Direct PL011 UART, no console_mux binding

**Location:** `kernel/agentos-root-task/src/linux_vmm.c:207–211` (AArch64 full impl)

```c
/* Register UART IRQ passthrough */
success = virq_register(GUEST_BOOT_VCPU_ID, SERIAL_IRQ, &serial_ack, NULL);
...
microkit_irq_ack(SERIAL_IRQ_CH);
```

**Also:** `linux_vmm.c:369–373` (notified handler)

```c
case SERIAL_IRQ_CH: {
    bool success = virq_inject(SERIAL_IRQ);
    ...
}
```

**Assessment:** The Linux VMM registers its own hardware IRQ passthrough for the PL011
UART (`SERIAL_IRQ = 33`, channel `SERIAL_IRQ_CH = 1`) and injects it directly into the
guest vGIC. The `console_mux` PD is never notified; the Linux guest's UART output
bypasses all session multiplexing, scrollback, and attach/detach semantics.

**Classification:** DEFECT — the generic service (`console_mux`) can be enhanced.
`console_mux` already supports a `OP_CONSOLE_WRITE` path and session registration.
The Linux VMM should intercept guest UART output at the virtIO console level and
forward it to the console_mux ring rather than wiring the IRQ through directly.

**Required refactoring (Phase 2):**
- Add a `OP_CONSOLE_VM_REGISTER` opcode to `console_mux` for VMM-hosted sessions.
- Linux VMM forwards guest UART bytes to its console_mux session ring rather than
  relying on bare PL011 passthrough.
- IRQ passthrough can remain for input; output should go through the mux.

---

### 3.2 FreeBSD VMM: Direct PL011 UART in DTS, no console_mux binding

**Location:** `kernel/freebsd-vmm/freebsd-vmm.dts:65–72`

```dts
serial0: pl011@9000000 {
    compatible = "arm,pl011", "arm,primecell";
    reg = <0x00 0x09000000 0x00 0x1000>;
    interrupts = <0 1 4>;
    ...
};
```

**Also:** `kernel/freebsd-vmm/vmm_mux.c:338–341` (vmm_mux_switch)

```c
microkit_dbg_puts("\n[vmm_mux] Switching to VM ");
...
microkit_dbg_puts("──────────────────────────────\n");
```

**Also:** `kernel/freebsd-vmm/vmm_mux.h:42–46` (comment)

> All VM UARTs are passed through to the same physical UART (PL011). Only the
> "active" slot has its UART interrupt connected; others are paused on UART input.

**Assessment:** The FreeBSD VMM exposes a raw PL011 UART directly to the guest via
the DTS. Console switching is managed entirely within `vmm_mux_switch()` by
suspending/resuming vCPUs — no IPC to `console_mux` at any point. The `console_mux`
session table explicitly allocates slot 14 for `linux_vmm` but has no slot for
FreeBSD VMM sessions at all. Multiple FreeBSD VM slots sharing a single physical UART
with no muxing is a known design limitation acknowledged in the header comment.

**Classification:** DEFECT — the generic service (`console_mux`) can be enhanced.
The fix requires `console_mux` to support VMM-sourced sessions (one session per VM
slot) and the FreeBSD VMM to register and drain rings per-slot.

---

### 3.3 Linux VMM: No block device for guest

**Assessment:** The Linux VMM loads a guest kernel from embedded symbols
(`_guest_kernel_image`, `_guest_initrd_image`) but does not expose any VirtIO block
device to the Linux guest. There is a generic `block-service` PD (`virtio_blk`), but
no IPC channel from `linux_vmm` to `virtio_blk` is wired. The Linux guest therefore
has no persistent storage.

**Classification:** GAP — not a violation (no duplicated implementation), but a
missing capability binding. The generic `block-service` PD should be connected via
capability grant at VM creation time. See §5 for the vm_manager hook location.

---

### 3.4 FreeBSD VMM: Self-contained VirtIO block device, bypasses block-service PD

**Location:** `kernel/freebsd-vmm/vmm.h:37–38`

```c
#define GUEST_VIRTIO_BLK_PADDR  0x0a003000UL  /* VirtIO block device base */
#define GUEST_VIRTIO_SIZE       0x00001000UL
```

**Also:** `kernel/freebsd-vmm/freebsd-vmm.dts:74–79`

```dts
virtio_blk@a003000 {
    compatible = "virtio,mmio";
    reg = <0x00 0x0a003000 0x00 0x1000>;
    interrupts = <0 6 4>;
};
```

**Also:** `kernel/freebsd-vmm/vmm_mux.c:174`

```c
vm_register_mmio_fault_handler(&slot->vm, NULL, NULL);
```

**Assessment:** The FreeBSD VMM exposes a VirtIO block MMIO region directly to the
guest (guest PA 0x0a003000). The `vm_register_mmio_fault_handler` call registers an
MMIO handler that, in the full libvmm implementation, would emulate the VirtIO block
device within the VMM PD itself. This completely bypasses the generic `block-service`
PD (`virtio_blk.c`), which implements the identical VirtIO-blk protocol against the
same QEMU virt device.

There is no IPC from the FreeBSD VMM to the `virtio_blk` PD. The MMIO fault handler
is set to `NULL` callbacks (prototype phase), but the architecture clearly intends
in-VMM emulation rather than delegation to the generic block-service.

**Classification:** DEFECT — requires an approved defect task unless the VMM is
refactored to proxy block I/O through the `block-service` PD. The `block-service` PD
already exposes `OP_BLK_READ`, `OP_BLK_WRITE`, `OP_BLK_FLUSH` via IPC. The FreeBSD
VMM's MMIO fault handler could forward these calls to the `block-service` PD via PPC
rather than emulating locally.

**Required refactoring (Phase 2):**
- Wire an IPC channel from `freebsd_vmm` to `virtio_blk` in the `.system` file.
- The MMIO fault handler intercepts VirtIO block MMIO writes, translates them to
  `OP_BLK_READ`/`OP_BLK_WRITE` PPCs to `virtio_blk`, and injects completion IRQs
  back to the guest via `virq_inject()`.

---

### 3.5 Network: No VMM uses net-service

**Assessment:** Neither the Linux VMM nor the FreeBSD VMM creates a vNIC via the
`net-service` PD (`OP_NET_VNIC_CREATE`). The Linux VMM has no network device at all.
The FreeBSD VMM exposes a VirtIO console device (0x0a004000) for serial I/O but no
VirtIO net device. No IPC channel from either VMM to `net_server` is defined.

This is a GAP rather than a violation: the VMMs are not duplicating their own network
stacks — they simply have no network. The generic service exists and is ready; the
binding just needs to be established.

**Classification:** GAP — capability grant from `vm_manager` to `net-service` is
missing for all VM slots. See §5 for the vm_manager hook location.

---

### 3.6 vm_manager: OP_VM_CONSOLE does not call console_mux

**Location:** `kernel/agentos-root-task/src/vm_manager.c:419–424`

```c
case OP_VM_CONSOLE: {
    uint8_t slot_id = (uint8_t)microkit_mr_get(1);
    int r = vmm_mux_switch(&g_mux, slot_id);
    microkit_mr_set(0, r == 0 ? VM_OK : VM_ERR);
    return microkit_msginfo_new(0, 1);
}
```

**Assessment:** `OP_VM_CONSOLE` calls `vmm_mux_switch()`, which suspends the previous
slot's vCPU and resumes the target. This is correct for vCPU scheduling. However,
it never notifies `console_mux` to switch its active session, meaning the console_mux
session state diverges from the VMM's active slot. The user's terminal will continue
showing output from all sessions in broadcast mode regardless of which VM slot was
selected.

**Classification:** DEFECT — `OP_VM_CONSOLE` should notify `console_mux` via
`OP_CONSOLE_ATTACH(pd_id)` after switching the VMM slot. This requires a channel from
`vm_manager` to `console_mux` in the `.system` file.

---

## 4. Approved Exceptions

### DEFECT-001: GPU Shared Memory Channel (linux_vmm + gpu_shmem)

See `docs/defects/DEFECT-001-gpu-shmem.md` for full justification.

**Summary:** The GPU shared memory channel (`gpu_shmem.c`/`gpu_shmem.h`) is a
purpose-built tensor descriptor ring for dispatching CUDA/PyTorch operations from
seL4 PDs to the Linux guest GPU stack. No generic block, network, or console service
can substitute for it. The `linux_vmm` correctly consumes this channel as
`GPU_SHMEM_ROLE_CONSUMER`. This is an approved exception to the generic service rule.

---

## 5. Recommended Refactoring: vm_manager.c Capability Grant at VM Creation

The `OP_VM_CREATE` handler in `vm_manager.c` is the correct location to grant
capability-based access to generic services for each new VM. Currently it calls
`vmm_mux_create()` and returns a slot ID, but it does not bind any generic services
to the new VM's protection domain.

**Target function:** `protected()` → `case OP_VM_CREATE:` in
`kernel/agentos-root-task/src/vm_manager.c:330–354`

The sequence that should happen after a successful `vmm_mux_create()` call is:

1. **net-service binding:** Grant the new VM's PD a capability to `net_server` and
   call `OP_NET_VNIC_CREATE` on its behalf, storing the assigned `vnic_id` in the
   slot's quota state (`g_quotas[slot_id]`).

2. **serial-mux binding:** Call `OP_CONSOLE_ATTACH` (or a new
   `OP_CONSOLE_VM_REGISTER`) on `console_mux`, passing the VM slot's PD identifier,
   so the console_mux session table gets an entry for this VM slot.

3. **block-service binding:** Grant the new VM's PD a capability to `virtio_blk` if
   the VM spec (`vm_spec`) includes a block device requirement. Wire the channel so the
   VMM's MMIO fault handler can PPC into `virtio_blk`.

The VM spec should be passed as additional message registers on `OP_VM_CREATE` (or via
a shared memory descriptor in `vm_list_shmem`) so `vm_manager` can make
policy-driven binding decisions without hardcoding which services every VM gets.

A sketch of the Phase 2 call site (not a code change — see task constraint):

```c
/* ── OP_VM_CREATE ─────────────────────────────────────────────────────────
 *
 * TODO (Phase 2 — capability grant for generic services):
 *
 * After vmm_mux_create() succeeds, grant the new VM slot capabilities to
 * generic services based on the VM spec in MR3 (vm_flags bitmask):
 *
 *   if (vm_flags & VM_FLAG_NETWORK) {
 *       // PPC to net_server: OP_NET_VNIC_CREATE for this slot's PD
 *       // Store assigned vnic_id in g_quotas[slot_id].net_vnic_id
 *   }
 *   if (vm_flags & VM_FLAG_CONSOLE) {
 *       // PPC to console_mux: OP_CONSOLE_VM_REGISTER(slot_id)
 *       // console_mux assigns a session slot and drains ring on notify
 *   }
 *   if (vm_flags & VM_FLAG_BLOCK) {
 *       // PPC to virtio_blk: OP_BLK_HEALTH to verify device is ready
 *       // Store block channel assignment for MMIO fault handler routing
 *   }
 *
 * Channel IDs required (to be added to the .system file):
 *   CH_VM_MGR_TO_NET    — vm_manager -> net_server   (pp=true)
 *   CH_VM_MGR_TO_CONSOLE — vm_manager -> console_mux (pp=true)
 *   CH_VM_MGR_TO_BLK    — vm_manager -> virtio_blk   (pp=true)
 */
```

---

## 6. Summary of Findings

| Finding | File:Line | Severity | Disposition |
|---|---|---|---|
| Linux VMM bypasses console_mux (direct PL011 passthrough) | `linux_vmm.c:207–211, 367–373` | High | Defect — refactor Phase 2 |
| FreeBSD VMM bypasses console_mux (raw DTS UART + vCPU suspend) | `freebsd-vmm.dts:65–72`, `vmm_mux.h:42–46` | High | Defect — refactor Phase 2 |
| FreeBSD VMM bypasses block-service (in-VMM VirtIO blk emulation) | `vmm.h:37–38`, `freebsd-vmm.dts:74–79`, `vmm_mux.c:174` | High | Defect — refactor Phase 2 |
| vm_manager OP_VM_CONSOLE doesn't notify console_mux | `vm_manager.c:419–424` | Medium | Defect — refactor Phase 2 |
| Linux VMM has no block device (no block-service binding) | `linux_vmm.c` (absent) | Medium | Gap — add capability grant in Phase 2 |
| Neither VMM uses net-service (no vNIC binding at VM creation) | `vm_manager.c:330–354` (absent) | Medium | Gap — add capability grant in Phase 2 |
| GPU shmem channel used correctly in linux_vmm | `linux_vmm.c:346–438` | — | Approved exception (DEFECT-001) |
