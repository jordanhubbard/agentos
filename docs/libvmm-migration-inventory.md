# libvmm Migration Inventory: Microkit API Assumptions in the VMM Layer

**Issue:** E7-S1  
**Scope:** Audit of all Microkit API call sites in the VMM protection domains and libvmm, preparatory to replacing Microkit with direct seL4 syscalls.  
**Files audited:** `kernel/agentos-root-task/src/linux_vmm.c`, `kernel/freebsd-vmm/vmm.c`, `kernel/freebsd-vmm/vmm_mux.c`, `kernel/agentos-root-task/src/vmm_mux_stub.c`, `libvmm/src/guest.c`, `libvmm/src/arch/aarch64/vcpu.c`, `libvmm/src/arch/aarch64/virq.c`, `libvmm/src/arch/aarch64/fault.c`, `libvmm/src/arch/aarch64/psci.c`, `libvmm/src/arch/aarch64/smc.c`, `libvmm/src/arch/aarch64/tcb.c`, `libvmm/src/virtio/{block,net,console,sound}.c`, `libvmm/src/util/util.c`, `libvmm/include/libvmm/arch/aarch64/vgic/virq.h`, `kernel/agentos-root-task/agentos-aarch64.system`.

---

## 1. Microkit API Call Sites That Require Migration

### 1a. PD Entry-Point Signatures (linux_vmm + freebsd_vmm)

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:113` | `void init(void)` — Microkit PD entry | `main()` loop calling `seL4_ReplyRecv` | M | Both x86 and AArch64 stubs use this; must become a `seL4_ReplyRecv` server loop |
| `linux_vmm.c:122` | `void notified(microkit_channel ch)` — async notification handler | Dispatch on badge inside `seL4_ReplyRecv` loop | M | Called per-channel; replace with badged notification cap + dispatch table |
| `linux_vmm.c:628` | `seL4_Bool fault(microkit_child, microkit_msginfo, microkit_msginfo*)` — fault handler | `seL4_Fault_*` label dispatch inside `seL4_ReplyRecv` | M | `fault_handle()` already unpacks `microkit_msginfo_get_label`; see §2 |
| `freebsd_vmm/vmm.c:133` | `void vmm_init(void)` — Microkit PD entry | `main()` loop calling `seL4_ReplyRecv` | M | Same as linux_vmm |
| `freebsd_vmm/vmm.c:184` | `void vmm_notified(microkit_channel ch)` | Badged notification dispatch | M | Forwards to `vmm_mux_handle_notify` |
| `freebsd_vmm/vmm.c:199` | `microkit_msginfo vmm_protected(microkit_channel ch, microkit_msginfo msginfo)` — PPC handler | `seL4_ReplyRecv` with opcode dispatch; reply via `seL4_Reply` | M | Contains OP_VM_CREATE/DESTROY/SWITCH/STATUS/LIST dispatch |

### 1b. `microkit_ppcall` — Synchronous IPC to Service PDs

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:305` | `microkit_ppcall(SERIAL_PD_CH, microkit_msginfo_new(MSG_SERIAL_OPEN, 2))` | `seL4_Call(serial_ep_cap, msginfo)` | S | SERIAL_PD_CH=1; needs a badged endpoint cap for serial_pd |
| `freebsd_vmm/vmm.c:75` | `microkit_ppcall(CH_VMM_KERNEL_LOCAL, microkit_msginfo_new(MSG_VMM_REGISTER, 3))` | `seL4_Call(root_task_ep_cap, msginfo)` | S | CH_VMM_KERNEL_LOCAL=5; root-task registration |
| `freebsd_vmm/vmm_mux.c:205` | `microkit_ppcall(CH_VMM_SERIAL, microkit_msginfo_new(MSG_SERIAL_OPEN, 2))` | `seL4_Call(serial_ep_cap, msginfo)` | S | Per-slot serial open |
| `freebsd_vmm/vmm_mux.c:217` | `microkit_ppcall(CH_VMM_NET, microkit_msginfo_new(MSG_NET_OPEN, 2))` | `seL4_Call(net_ep_cap, msginfo)` | S | Per-slot net open |
| `freebsd_vmm/vmm_mux.c:230` | `microkit_ppcall(CH_VMM_BLOCK, microkit_msginfo_new(MSG_BLOCK_OPEN, 3))` | `seL4_Call(block_ep_cap, msginfo)` | S | Per-slot block open |
| `freebsd_vmm/vmm_mux.c:249` | `microkit_ppcall(CH_VMM_KERNEL_LOCAL, microkit_msginfo_new(MSG_GUEST_BIND_DEVICE, 4))` (×3) | `seL4_Call(root_task_ep_cap, msginfo)` | S | Three calls for serial/net/block binding |
| `freebsd_vmm/vmm_mux.c:278` | `microkit_ppcall(CH_VMM_KERNEL_LOCAL, microkit_msginfo_new(MSG_GUEST_SET_MEMORY, 6))` | `seL4_Call(root_task_ep_cap, msginfo)` | S | Guest RAM declaration |
| `freebsd_vmm/vmm_mux.c:604` | `microkit_ppcall(CH_VMM_SERIAL, microkit_msginfo_new(MSG_SERIAL_WRITE, 3))` | `seL4_Call(serial_ep_cap, msginfo)` | S | UART write path in vmm_uart_fault |
| `freebsd_vmm/vmm_mux.c:622` | `microkit_ppcall(CH_VMM_SERIAL, microkit_msginfo_new(MSG_SERIAL_READ, 3))` | `seL4_Call(serial_ep_cap, msginfo)` | S | UART read path |
| `freebsd_vmm/vmm_mux.c:815` | `microkit_ppcall(CH_VMM_BLOCK, microkit_msginfo_new(MSG_BLOCK_READ, 4))` | `seL4_Call(block_ep_cap, msginfo)` | S | Block read in vmm_blk_fault |
| `freebsd_vmm/vmm_mux.c:832` | `microkit_ppcall(CH_VMM_BLOCK, microkit_msginfo_new(MSG_BLOCK_WRITE, 4))` | `seL4_Call(block_ep_cap, msginfo)` | S | Block write |
| `freebsd_vmm/vmm_mux.c:924` | `microkit_ppcall(CH_VMM_NET, microkit_msginfo_new(MSG_NET_SEND, 3))` | `seL4_Call(net_ep_cap, msginfo)` | S | Net TX in vmm_net_fault |

### 1c. `microkit_notify` — Async Notifications

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:511` | `microkit_notify(CONTROLLER_CH)` — notify controller VMM ready | `seL4_Send(controller_notif_cap, seL4_MessageInfo_new(0,0,0,0))` | S | CONTROLLER_CH=2 |
| `linux_vmm.c:595` | `microkit_notify(CONTROLLER_CH)` — notify GPU result ready | Same as above | S | |
| `freebsd_vmm/vmm_mux.c:519` | `microkit_notify(CH_VMM_CONTROLLER_EVT)` — fault event to controller | `seL4_Send(controller_notif_cap, ...)` | S | CH_VMM_CONTROLLER_EVT=1 |
| `libvmm/src/virtio/block.c:570` | `microkit_notify(state->server_ch)` — block server notification | `seL4_Send(block_notif_cap, ...)` | S | `server_ch` is a Microkit channel integer |
| `libvmm/src/virtio/block.c:802` | `microkit_notify(state->server_ch)` | Same | S | Second call site |
| `libvmm/src/virtio/net.c:248` | `microkit_notify(state->tx_ch)` — net TX channel notification | `seL4_Send(net_tx_notif_cap, ...)` | S | |
| `libvmm/src/virtio/console.c:169` | `microkit_notify(console->tx_ch)` | `seL4_Send(serial_notif_cap, ...)` | S | |
| `libvmm/src/virtio/sound.c:655` | `microkit_notify(state->server_ch)` | `seL4_Send(sound_notif_cap, ...)` | S | |

### 1d. `microkit_irq_ack` — IRQ Acknowledgement

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:344` | `microkit_irq_ack(VIRTIO_NET_IRQ_CH)` — ack virtio-net IRQ (ch=5) | `seL4_IRQHandler_Ack(virtio_net_irq_handler_cap)` | S | Must obtain `seL4_IRQControl_Get` cap from root-task first |
| `linux_vmm.c:350` | `microkit_irq_ack(VIRTIO_BLK_IRQ_CH)` — ack virtio-blk IRQ (ch=6) | `seL4_IRQHandler_Ack(virtio_blk_irq_handler_cap)` | S | Same pattern |
| `linux_vmm.c:480` | `microkit_irq_ack(VIRTIO_NET_IRQ_CH)` — init-time pre-ack | Same | S | |
| `linux_vmm.c:488` | `microkit_irq_ack(VIRTIO_BLK_IRQ_CH)` — init-time pre-ack | Same | S | |
| `libvmm/src/arch/aarch64/virq.c:95` | `microkit_irq_ack((microkit_channel)(size_t)cookie)` — generic passthrough ack | `seL4_IRQHandler_Ack((seL4_IRQHandler)cookie)` | S | `cookie` stores the channel; must store the handler cap instead |

### 1e. Microkit Message Register API (`microkit_mr_set` / `microkit_mr_get` / `microkit_msginfo_*`)

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:303–308` | `microkit_mr_set(0, ...)` / `microkit_msginfo_new(...)` / `microkit_mr_get(0)` | `seL4_SetMR(0, val)` / `seL4_MessageInfo_new(label,0,0,count)` / `seL4_GetMR(0)` | S | Pattern used throughout vmm_mux for all IPC calls |
| `freebsd_vmm/vmm.c:71–86` | `microkit_mr_set` × 3, `microkit_mr_get` × 2, `microkit_msginfo_new`, `microkit_msginfo_get_label` | `seL4_SetMR` / `seL4_GetMR` / `seL4_MessageInfo_new` / `seL4_MessageInfo_getLabel` | S | Mechanical rename throughout all call sites |
| `libvmm/src/arch/aarch64/fault.c:451` | `microkit_msginfo_get_label(msginfo)` — extract fault label | `seL4_MessageInfo_getLabel(msginfo)` | S | Used in `fault_handle()` |

### 1f. Microkit VCPU API — Wraps seL4 VCPU Syscalls (libvmm internals)

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `libvmm/src/guest.c:25–26` | `seL4_TCB_WriteRegisters(BASE_VM_TCB_CAP + GUEST_BOOT_VCPU_ID, ...)` | Direct call (already seL4); `BASE_VM_TCB_CAP` is a Microkit-assigned constant | L | `BASE_VM_TCB_CAP` is not exported by Microkit API but depends on its CSpace layout. Replace with a cap passed in at init. |
| `libvmm/src/guest.c:41` | `microkit_vcpu_restart(GUEST_BOOT_VCPU_ID, regs.pc)` | `seL4_TCB_Resume(vcpu_tcb_cap)` (after `seL4_TCB_WriteRegisters`) | M | Need to hold the TCB cap explicitly |
| `libvmm/src/guest.c:49,57` | `microkit_vcpu_stop(GUEST_BOOT_VCPU_ID)` | `seL4_TCB_Suspend(vcpu_tcb_cap)` | S | |
| `libvmm/src/arch/aarch64/vcpu.c:56–90` | `microkit_vcpu_arm_write_reg(vcpu_id, seL4_VCPUReg_*, val)` × 17 calls in `vcpu_reset()` | `seL4_ARM_VCPU_WriteRegs(vcpu_cap, reg, val)` | M | `vcpu_id` indexes Microkit's static cap table; replace with explicit `seL4_CPtr vcpu_cap` |
| `libvmm/src/arch/aarch64/vcpu.c:98–130` | `microkit_vcpu_arm_read_reg(vcpu_id, seL4_VCPUReg_*)` × 16 calls in `vcpu_print_regs()` | `seL4_ARM_VCPU_ReadRegs(vcpu_cap, reg)` | M | Same cap-lookup issue |
| `libvmm/src/arch/aarch64/virq.c:23` | `microkit_vcpu_arm_ack_vppi(vcpu_id, irq)` | `seL4_ARM_VCPU_AckVPPI(vcpu_cap, irq)` | S | |
| `libvmm/src/arch/aarch64/virq.c:98` | `bool virq_register_passthrough(size_t vcpu_id, size_t irq, microkit_channel irq_ch)` | signature: `microkit_channel irq_ch` → `seL4_IRQHandler irq_handler` | M | API signature change propagates to all callers |
| `libvmm/src/arch/aarch64/virq.c:119` | `bool virq_handle_passthrough(microkit_channel irq_ch)` | `bool virq_handle_passthrough(seL4_IRQHandler irq_handler)` | M | Same signature change |
| `libvmm/include/.../virq.h:227` | `microkit_vcpu_arm_inject_irq(vcpu_id, virq->virq, 0, group, idx)` | `seL4_ARM_VCPU_InjectIRQ(vcpu_cap, irq, priority, group, index)` | M | Called inside vGIC emulation hot path |
| `libvmm/src/arch/aarch64/fault.c:297` | `microkit_vcpu_arm_ack_vppi(vcpu_id, ppi_irq)` | `seL4_ARM_VCPU_AckVPPI(vcpu_cap, ppi_irq)` | S | |
| `libvmm/src/arch/aarch64/fault.c:475` | `microkit_vcpu_stop(vcpu_id)` | `seL4_TCB_Suspend(vcpu_tcb_cap)` | S | |
| `libvmm/src/arch/aarch64/psci.c:66` | `microkit_vcpu_arm_write_reg(target_vcpu, seL4_VCPUReg_VMPIDR_EL2, ...)` | `seL4_ARM_VCPU_WriteRegs(vcpu_cap, ...)` | S | |
| `libvmm/src/arch/aarch64/psci.c:83` | `microkit_vcpu_restart(target_vcpu, vcpu_regs.pc)` | `seL4_TCB_Resume(vcpu_tcb_cap)` | S | |
| `libvmm/src/arch/aarch64/smc.c:198` | `seL4_TCB_ReadRegisters(BASE_VM_TCB_CAP + vcpu_id, ...)` | Direct call (already seL4); `BASE_VM_TCB_CAP` offset is Microkit-specific | L | Same `BASE_VM_TCB_CAP` problem as guest.c |
| `libvmm/src/arch/aarch64/tcb.c:22` | `seL4_TCB_ReadRegisters(BASE_VM_TCB_CAP + vcpu_id, ...)` | Same | L | |

### 1g. `microkit_dbg_puts` / `microkit_dbg_putc` — Debug Output

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:92,109,...` | `microkit_dbg_puts(...)` — many call sites in x86 stub + AArch64 | `seL4_DebugPutChar(c)` loop; or route via serial_pd | S | Mechanical; keep as debug helper, just swap the implementation |
| `freebsd_vmm/vmm.c:135–175` | `microkit_dbg_puts(...)` | Same | S | |
| `freebsd_vmm/vmm_mux.c:86,...` | `microkit_dbg_puts(...)` | Same | S | |
| `libvmm/src/util/util.c:15` | `microkit_dbg_putc(character)` | `seL4_DebugPutChar(character)` | S | Single call site; the rest of libvmm uses `printf` via this helper |

### 1h. Hard-Coded Channel Integer Constants

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:211` | `#define SERIAL_PD_CH 1` — channel to serial_pd | Named endpoint cap slot in CSpace | S | All CH_ defines become cap slot indices passed in at init |
| `linux_vmm.c:213` | `#define CONTROLLER_CH 2` | Same | S | |
| `linux_vmm.c:217,220` | `#define VIRTIO_NET_IRQ_CH 5`, `#define VIRTIO_BLK_IRQ_CH 6` | `seL4_IRQHandler` caps | S | |
| `linux_vmm.c:225,226` | `#define GPU_SHMEM_NOTIFY_IN_CH 3`, `GPU_SHMEM_NOTIFY_OUT_CH 4` | Notification cap indices | S | |
| `freebsd_vmm/vmm.h:101–106` | `CH_VMM_CONTROLLER_PPC 0`, `CH_VMM_CONTROLLER_EVT 1`, `CH_VMM_SERIAL 2`, `CH_VMM_NET 3`, `CH_VMM_BLOCK 4`, `CH_VMM_KERNEL_LOCAL 5` | Named endpoint/notification caps | S | Six defines; all become explicit cap slots |

### 1i. Microkit-Managed Symbols and Priorities

| File:Line | API call / assumption | seL4 equivalent | Effort | Notes |
|-----------|----------------------|-----------------|--------|-------|
| `linux_vmm.c:239,244` | `uintptr_t guest_ram_vaddr` — set by Microkit linker from `setvar_vaddr` | Root-task must pass vaddr via IPC at init, or via a known slot in shared memory | M | All `setvar_vaddr` symbols need an explicit init-time handoff |
| `linux_vmm.c:244,248` | `gpu_tensor_buf_vaddr`, `serial_shmem_linux_vaddr` (weak symbols) | Same mechanism | M | |
| `freebsd_vmm/vmm_mux.c:41–52` | `guest_ram_vaddr_0..3`, `guest_flash_vaddr` — Microkit linker-set | Root-task handoff via init IPC | M | |
| `libvmm/src/guest.h:10` | `GUEST_BOOT_VCPU_ID 0` — assumes vCPU 0 is always the boot vCPU | Explicit cap passed at VMM init | S | Low risk; constant holds as long as single-vCPU assumption holds |
| `libvmm/src/arch/aarch64/vcpu.c:29` | `BASE_VM_TCB_CAP` (used but defined by Microkit's SDK) — offset into static CSpace for TCB caps | seL4 root-task distributes individual TCB caps; VMM receives them at init | L | Hardest single dependency to break; entire libvmm VCPU/fault/SMC/TCB path depends on this arithmetic |
| `agentos-aarch64.system:353–367` | `<virtual_machine name="linux"><vcpu id="0" />` — VCPU wired by Microkit system description | `seL4_ARM_VCPUControl_new` + `seL4_ARM_VCPU_SetTCB` + `seL4_TCBPool` allocation by root-task | L | Entire `<virtual_machine>` element is Microkit-specific topology |
| `agentos-aarch64.system:175` | `priority="175"` for linux_vmm — chosen to satisfy Microkit PPC ordering | seL4 MCS: priority must still be set explicitly on `seL4_SchedContext`; no change in value but mechanism changes | S | PPC ordering constraints become endpoint badge ordering in native seL4 |

---

## 2. seL4 Operations Already Used Correctly (No Migration Needed)

These call sites use raw seL4 syscalls directly and do not need to change:

| File:Line | seL4 call | Notes |
|-----------|-----------|-------|
| `libvmm/src/guest.c:25` | `seL4_TCB_WriteRegisters(BASE_VM_TCB_CAP + id, ...)` | seL4 syscall; only `BASE_VM_TCB_CAP` offset needs fixing |
| `libvmm/src/arch/aarch64/fault.c:35,270,341,424` | `seL4_TCB_WriteRegisters` / `seL4_TCB_ReadRegisters` | Raw seL4; same `BASE_VM_TCB_CAP` caveat |
| `libvmm/src/arch/aarch64/smc.c:198` | `seL4_TCB_ReadRegisters` | Same |
| `libvmm/src/arch/aarch64/psci.c:69` | `seL4_TCB_WriteRegisters` | Same |
| `libvmm/src/arch/aarch64/tcb.c:22` | `seL4_TCB_ReadRegisters` | Same |
| `libvmm/src/arch/aarch64/fault.c:44–57` | `seL4_Fault_VMFault`, `seL4_Fault_VCPUFault`, `seL4_Fault_VGICMaintenance`, etc. — fault label dispatch | Already correct seL4 fault labels; no change needed |
| `freebsd_vmm/vmm_mux.c:186` | `vmm_slot_t.vcpu_id` used as `seL4_TCB_WriteRegisters` index | Correct; will map to cap once `BASE_VM_TCB_CAP` is eliminated |
| `linux_vmm.c:371,378` | `seL4_TCB_SetAffinity` (comment, not yet wired) | Correct seL4 API when implemented |

---

## 3. Summary

### Total Microkit API Call Sites
- **`microkit_ppcall`:** 12 unique call sites (freebsd_vmm + linux_vmm)
- **`microkit_notify`:** 8 call sites (kernel + libvmm virtio)
- **`microkit_irq_ack`:** 5 call sites (linux_vmm + libvmm virq)
- **`microkit_mr_set/get`:** ~40 call sites (every ppcall site uses 2–6 MR ops)
- **`microkit_msginfo_*`:** ~30 call sites
- **`microkit_vcpu_arm_write_reg`:** 17 call sites (`vcpu_reset`)
- **`microkit_vcpu_arm_read_reg`:** 16 call sites (`vcpu_print_regs`)
- **`microkit_vcpu_restart/stop`:** 5 call sites
- **`microkit_vcpu_arm_inject_irq/ack_vppi`:** 3 call sites
- **`microkit_dbg_puts/putc`:** ~25 call sites (low risk, mechanical)
- **Entry-point signatures** (`init`, `notified`, `fault`, `protected`): 6
- **`BASE_VM_TCB_CAP` offset dependency:** 7 sites in libvmm
- **Linker-set `setvar_vaddr` symbols:** 7 symbols across linux_vmm + freebsd_vmm

**Total distinct sites requiring change: ~78** (not counting debug calls)

### Effort Breakdown
- **S (< 1 day):** ~55 sites — MR get/set rename, notify/ppcall swap to seL4_Call/Send, irq_ack rename, debug calls, channel constant rename
- **M (1–3 days):** ~15 sites — entry-point loop rewrite, `setvar_vaddr` handoff mechanism, VCPU restart/stop/read/write, virq passthrough API signature
- **L (> 3 days):** ~8 sites — `BASE_VM_TCB_CAP` elimination (requires libvmm-wide refactor to accept explicit cap slots), `<virtual_machine>` topology replacement

### Recommended Migration Order

1. **First (unblocks everything):** Eliminate `BASE_VM_TCB_CAP`. This is the deepest dependency — it forces libvmm's `guest.c`, `fault.c`, `smc.c`, `tcb.c`, and `psci.c` to derive TCB caps from a Microkit-internal constant. Replace with a `vmm_caps_t` struct passed at init that holds `vcpu_tcb_cap[]` and `vcpu_arm_cap[]` arrays. All L-effort items collapse once this is done.

2. **Second:** Rewrite PD entry points (`init`/`notified`/`fault`/`protected`) to a `seL4_ReplyRecv` server loop. This affects 6 call sites but is the architectural change that makes all the S-effort IPC swaps possible.

3. **Third (parallel, all S-effort):** Mechanical rename of `microkit_ppcall` → `seL4_Call`, `microkit_notify` → `seL4_Send`, `microkit_irq_ack` → `seL4_IRQHandler_Ack`, `microkit_mr_set/get` → `seL4_SetMR/GetMR`, `microkit_msginfo_*` → `seL4_MessageInfo_*`.

4. **Fourth:** Replace `virq_register_passthrough` / `virq_handle_passthrough` API to accept `seL4_IRQHandler` caps instead of channel integers. This requires coordinated change in libvmm + every VMM that calls it.

5. **Last:** Replace `<virtual_machine>` / `setvar_vaddr` Microkit topology with explicit seL4 VCPU object allocation in the root-task and init-time cap handoff to the VMM PD.

### Non-Obvious Risks

- **`BASE_VM_TCB_CAP` is not an exported Microkit API.** It is an internal layout constant (`FIRST_VM_TCB` in Microkit's CSpace allocator). Breaking this requires either reading Microkit source or empirically probing the CSpace — this is the highest-risk item.
- **`microkit_irq_ack` stores channel integers as cookie pointers** in `virq_register_passthrough`. The migration must change `void *cookie` to store `seL4_IRQHandler` caps, which requires touching the callback signature passed through vGIC.
- **Priority 175 for linux_vmm** is not a Microkit PPC ordering constraint — it is a scheduling priority. It stays unchanged in seL4-native mode.
- **libvmm virtio** (`block.c`, `net.c`, `console.c`, `sound.c`) uses `microkit_channel` integers as field types in their state structs. These will need to become `seL4_CPtr` notification caps, requiring struct layout changes that may affect existing callers.
- **`linux_vmm.c` commented-out code** at lines 293–337 (binding protocol TODOs) already shows the intended Microkit ppcall pattern — do not implement those TODOs; implement the seL4_Call equivalents directly.
