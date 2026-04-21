# Generic Device Abstraction Layer

## What Is This?

The agentOS generic device abstraction layer defines a uniform set of IPC
contracts between protection domains (PDs) and device services.  Each device
type — serial console, network, block storage, USB, timers, entropy — has a
single canonical service PD that owns the hardware and exports a versioned
interface to all clients.

Contracts are declared as C headers under `contracts/<service-name>/interface.h`.
Each header is self-contained: it specifies the interface version, all opcode
values, error codes, and `__attribute__((packed))` request/reply structs.  The
headers carry no implementation; they are shared between the service PD and its
clients via the normal include path.

## The Mandatory Rule

**Every guest OS, VMM, and agent MUST use these generic services.  Direct
hardware access (MMIO, port I/O, hardware RNG registers) by any PD other than
the designated service PD is prohibited.**

The rule exists because:

- seL4 capability isolation is only effective when hardware access is brokered.
  A PD that bypasses the device service subverts the entire security model.
- Device services enforce ACLs, quotas, and audit logging that are invisible to
  direct hardware users.
- A shared service allows the hypervisor layer (vm_manager) to hot-swap or mock
  a device without modifying guest code.

If your guest OS or VMM needs a device behaviour that the generic service does
not provide, the correct path is:

1. Open a defect task describing the missing functionality.
2. Obtain approval from the agentOS maintainers.
3. Extend the canonical service and its contract header — do **not** create a
   parallel private driver.

Bypass implementations discovered during review will be reverted.

## Available Services

| Service directory         | Contract header                           | Status      |
|---------------------------|-------------------------------------------|-------------|
| `serial-mux`              | `contracts/serial-mux/interface.h`        | IMPLEMENTED |
| `net-service`             | `contracts/net-service/interface.h`       | IMPLEMENTED |
| `block-service`           | `contracts/block-service/interface.h`     | IMPLEMENTED |
| `timer-service`           | `contracts/timer-service/interface.h`     | IMPLEMENTED |
| `entropy-service`         | `contracts/entropy-service/interface.h`   | IMPLEMENTED |
| `usb-service`             | `contracts/usb-service/interface.h`       | PLANNED     |

## How a Guest OS Discovers and Binds to a Service

Device service capabilities are granted at creation time by `vm_manager.c`.
The vm_manager is the root authority for all guest OS lifecycle events.  When
it creates a new VM slot it:

1. Grants a PPC endpoint capability to each device service the guest is
   permitted to use.  The set of permitted services is determined by the
   capability policy (`cap_policy.nano` / `cap_policy.c`).

2. Maps the relevant shared-memory regions into the guest's address space
   (e.g., `net_packet_shmem` for the net-service, `blk_dma_shmem` for the
   block-service, `console_rings` for the serial-mux).

3. Writes the virtual addresses of those regions into the guest's boot info
   or a well-known page in guest physical memory, depending on the VMM type.

After boot the guest OS queries each service to discover its assigned
identifiers (e.g., vNIC id, serial ring slot) by calling the service's
`OP_CONNECT` / `OP_OPEN` / `OP_ENUMERATE` opcode.

### Example: guest OS binding to net-service

```c
#include "contracts/net-service/interface.h"

/* 1. Call CONNECT to allocate a vNIC.  The PPC endpoint cap was granted
      by vm_manager at slot creation time. */
microkit_mr_set(0, NET_SVC_OP_CONNECT);
microkit_mr_set(1, 0xFF);             /* auto-assign vnic_id */
microkit_mr_set(2, CAP_CLASS_NET);    /* required capability class */
microkit_mr_set(3, my_pd_id);
microkit_msginfo reply = microkit_ppcall(CH_NET_SERVICE, microkit_msginfo_new(0, 4));

uint32_t status   = microkit_mr_get(0);
uint32_t vnic_id  = microkit_mr_get(1);
uint32_t shmem_off = microkit_mr_get(2);

if (status != NET_SVC_ERR_OK) { /* handle error */ }

/* 2. The net_packet_shmem region is already mapped by vm_manager.
      shmem_off is the byte offset within that region for this vNIC's ring. */
volatile net_svc_vnic_ring_t *ring =
    (volatile net_svc_vnic_ring_t *)(net_shmem_base + shmem_off);
```

## Requesting a Capability to a Device Service

PD authors do not request capabilities directly.  The flow is:

1. **Declare** the required capability class in the PD's capability policy
   annotation (e.g., `CAP_CLASS_NET`, `CAP_CLASS_BLOCK`).

2. **vm_manager** reads the policy at VM slot creation time via `cap_policy.c`.
   If the policy grants the class, vm_manager issues the relevant seL4 capability
   grant and shmem mapping.

3. The device service verifies the capability class on every PPC call.  A PD
   that was not granted `CAP_CLASS_NET` at creation will receive
   `NET_SVC_ERR_PERM` from every net-service call.

For kernel PDs (non-VM agents), capability grants are static and declared in
`agentos.system`.  For guest OSes, they are dynamic and controlled by
vm_manager.

## Interface Version Compatibility

Each contract header defines `<SERVICE>_INTERFACE_VERSION`.  Service PDs
reject requests from clients that present an incompatible version in a
version-check opcode (to be standardised in a future revision; for now callers
are expected to be compiled against the same header version as the service).

When a contract changes in an incompatible way the version number is
incremented and both the service and all known clients must be updated together.

## Adding a New Service

1. Create `contracts/<service-name>/interface.h` following the patterns in the
   existing headers: `#pragma once`, interface version define, opcode defines,
   error code defines, packed structs.

2. Mark it `// STATUS: PLANNED` until a service PD exists.

3. Implement the service PD under `kernel/agentos-root-task/src/` (for kernel
   services) or `services/<service-name>/` (for userspace services).

4. Register the shared-memory region and PPC channel in `agentos.system`.

5. Update vm_manager.c to grant the new capability class to guest OSes that
   request it.

6. Change the status comment to `// STATUS: IMPLEMENTED` and update this table.
