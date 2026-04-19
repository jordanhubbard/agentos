# vibeOS — Guest OS Lifecycle API

vibeOS is the primary external interface to agentOS for on-demand management of
guest operating system instances. It lets any agent or system service create,
destroy, configure, snapshot, and inspect Linux or FreeBSD guest VMs through a
pure seL4 IPC protocol, without any kernel modifications or privileged access
beyond a capability badge.

## Concept

agentOS separates *capability isolation* (seL4) from *OS-level services*
(guest VMs). Most agent workloads live natively in seL4 protection domains,
but some tasks require a full Linux or FreeBSD userland — package managers,
POSIX-only libraries, legacy agents. vibeOS provides these on demand.

Each guest instance is backed by a `vm_mux` slot in the `vm_manager` protection
domain. `vm_manager` runs at priority 145 and is the only entity that touches
the VMM layer directly. vibeOS is a thin IPC façade in front of `vm_manager`
that adds:

- Capability-gated access (rights bitmask per endpoint badge)
- Structured typed API (`vos_spec_t`, `vos_status_t`)
- Service attachment lifecycle (virtio serial, network, block, entropy, …)
- Snapshot / restore / migrate verbs

## Architecture

```
 ┌───────────────────────────────────────────────────────────────┐
 │  Agent PD / system service PD                                 │
 │                                                               │
 │  vos_handle_t h = vos_create(&spec);    // vibeOS PPC       │
 │  vos_attach_service(h, VOS_SVC_SERIAL); // vibeOS PPC       │
 └─────────────────────────┬─────────────────────────────────────┘
                            │ seL4 PPC (badged endpoint cap)
                            ▼
 ┌───────────────────────────────────────────────────────────────┐
 │  vibeOS server PD (priority 150)                              │
 │                                                               │
 │  • Validates caller badge rights                              │
 │  • Translates VOS_OP_* → OP_VM_* IPC to vm_manager           │
 │  • Maintains handle → slot mapping and uptime tracking        │
 └─────────────────────────┬─────────────────────────────────────┘
                            │ seL4 PPC (CH_VM_MANAGER)
                            ▼
 ┌───────────────────────────────────────────────────────────────┐
 │  vm_manager PD (priority 145)                                 │
 │                                                               │
 │  • Owns vm_mux_t: 4 slot round-robin multiplexer             │
 │  • Calls into libvmm (AArch64) or vmm stub (x86_64)          │
 │  • Runs vm_sched_tick() on timer notification                 │
 └───────────────────────────────────────────────────────────────┘
```

## Endpoint Discovery

The vibeOS server registers its endpoint with the agentOS nameserver under the
well-known path `vibeos.v1`. Callers obtain an endpoint capability from the
capability broker by invoking:

```
cap_broker_lookup("vibeos.v1", desired_rights, &ep_cap);
```

where `desired_rights` is a `vos_rights_t` bitmask specifying only the
operations needed (principle of least privilege). The broker mints a badged
copy of the vibeOS endpoint capability encoding those rights; the badge is
checked by the vibeOS server on every call.

Callers must also negotiate a shared-memory region with the vibeOS server for
passing large inputs (vos_spec_t, config blobs) and receiving large outputs
(vos_status_t, list arrays). This is done via a standard agentOS shmem
negotiation call before the first `VOS_OP_CREATE`.

## Creating a Guest OS — Step by Step

```c
#include <contracts/vibeos/interface.h>

/* 1. Obtain a badged endpoint capability from the capability broker */
seL4_CPtr ep;
cap_broker_lookup("vibeos.v1", VOS_RIGHT_CREATE | VOS_RIGHT_INSPECT, &ep);

/* 2. Populate a creation spec in the negotiated shared-memory region */
vos_spec_t *spec = (vos_spec_t *)vos_shmem_base;
spec->os_type      = VOS_OS_LINUX;
spec->vcpu_count   = 1;
spec->cpu_quota_pct= 25;             /* up to 25% of one host CPU */
spec->memory_pages = 65536;          /* 256 MiB guest RAM */
spec->cpu_affinity = 0xFFFFFFFF;     /* any core */
spec->boot_image_cap = linux_image_cap;
spec->config_blob  = (uint64_t)(uintptr_t)"console=ttyAMA0 root=/dev/vda";
spec->config_len   = 31;
__builtin_strncpy(spec->label, "my-linux", 16);

/* 3. Issue the create PPC */
seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 2);
seL4_SetMR(0, VOS_OP_CREATE);
tag = seL4_Call(ep, tag);

uint32_t err    = (uint32_t)seL4_GetMR(0);
vos_handle_t h  = (vos_handle_t)seL4_GetMR(1);

if (err != VOS_ERR_OK) { /* handle error */ }

/* 4. Attach a serial console so the guest has a UART */
seL4_SetMR(0, VOS_OP_ATTACH_SERVICE);
seL4_SetMR(1, h);
seL4_SetMR(2, VOS_SVC_SERIAL);
tag = seL4_MessageInfo_new(0, 0, 0, 3);
tag = seL4_Call(ep, tag);

uint32_t serial_cap = (uint32_t)seL4_GetMR(1);  /* cap to the serial endpoint */

/* 5. Poll status */
seL4_SetMR(0, VOS_OP_STATUS);
seL4_SetMR(1, h);
tag = seL4_MessageInfo_new(0, 0, 0, 2);
tag = seL4_Call(ep, tag);

vos_status_t *status = (vos_status_t *)vos_shmem_base;
/* status->state == VOS_STATE_RUNNING when the guest has booted */
```

## Lifecycle State Diagram

```
                          ┌─────────────┐
              VOS_OP_CREATE│             │
   ─────────────────────►  │  CREATING   │
                           │             │
                           └──────┬──────┘
                                  │ boot complete
                                  ▼
              ┌──────────────────────────────────────┐
              │             RUNNING                   │
              │                                       │
              │  VOS_OP_ATTACH_SERVICE ──► attach ok │
              │  VOS_OP_DETACH_SERVICE ──► detach ok │
              │  VOS_OP_CONFIGURE      ──► reconfig  │
              │  VOS_OP_SNAPSHOT       ──► snap id   │
              │  VOS_OP_MIGRATE        ──► new handle │
              └───────┬──────────────────────┬────────┘
                      │ VOS_OP_DESTROY       │ quota exhausted /
                      │                      │ explicit suspend
                      ▼                      ▼
              ┌──────────────┐     ┌─────────────────┐
              │  DESTROYED   │     │   SUSPENDED      │
              │              │     │                   │
              │ (handle inval│     │ VOS_OP_RESTORE ──┼──► RUNNING
              │ resources    │     │ VOS_OP_DESTROY ──┼──► DESTROYED
              │ reclaimed)   │     └─────────────────┘
              └──────────────┘
```

## Snapshot and Restore

`VOS_OP_SNAPSHOT` briefly suspends the guest, serialises its memory into the
caller-supplied destination capability (an agentfs object or a caller-mapped
frame), then resumes the guest. It returns a 64-bit snapshot ID (content hash).

`VOS_OP_RESTORE` creates a *new* guest instance from a snapshot ID and a
`vos_spec_t` that describes the resources for the restored instance. The
`os_type` and `memory_pages` must match the original snapshot.

## Migration

`VOS_OP_MIGRATE` transfers a live guest to a different vibeOS endpoint. This
may be a different protection domain on the same physical host (tested and
supported), or a remote host reached through the agentOS mesh layer (planned).
The source handle is invalidated on success; the caller receives a new handle
valid in the destination domain.

## Error Handling

All operations return `VOS_ERR_OK` (0) in reply MR0 on success. On failure the
MR0 contains a `VOS_ERR_*` code and other reply registers are undefined.

Callers should treat `VOS_ERR_INTERNAL` as an agentOS bug and report it. All
other errors are expected and recoverable: retry after `VOS_ERR_OUT_OF_MEMORY`
when a slot frees up, or fix the spec before retrying `VOS_ERR_INVALID_SPEC`.

## Supported OS Types by Platform

| os_type          | AArch64 (QEMU) | AArch64 (native) | x86_64 |
|------------------|:--------------:|:----------------:|:------:|
| VOS_OS_LINUX     | yes            | stub             | stub   |
| VOS_OS_FREEBSD   | yes            | stub             | yes    |
| VOS_OS_CUSTOM    | yes            | yes              | yes    |

Platforms returning `VOS_ERR_UNSUPPORTED_OS` for a given combination indicate
that the VMM layer (libvmm / freebsd-vmm) is not compiled in for that target.
