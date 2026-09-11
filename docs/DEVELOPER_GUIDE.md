# agentOS Developer Guide

For people building on agentOS: adding a protection domain, a virtualizer, a
guest profile, or a native agent. Read `docs/TCB.md` first; it is binding.
`CLAUDE.md` and `AGENTS.md` hold the rules this guide assumes.

Paths are relative to the repository root. `RT` below means
`kernel/agentos-root-task`.

## 1. Boot flow

```
QEMU -device loader
  -> kernel/loader            seL4 loader: places the kernel and agentos.img
  -> seL4                     EL2; never modified
  -> root task                RT/src/main.c, entry RT/src/start_aarch64.S
       parses agentos.img     format: docs/sel4-loader-format.md
       reads the descriptor   RT/src/system_desc_aarch64.c (pd_desc_t table)
       for each PD in order:  allocate CSpace/VSpace, load ELF from the
                              embedded PD bundle, mint init EPs, bind IRQs,
                              map device frames and shared regions, set
                              priority, start the thread at _start
       parks in seL4_Wait     no policy after spawn
  -> PDs                      nameserver first, then the rest by table order
```

`make build` runs `RT/Makefile`, which compiles every PD ELF in its `IMAGES`
list, packs the ELFs named in `RT/agentos.toml` into a `.pd_bundle` with
`cargo xtask gen-pd-bundle`, links that into `root_task.elf`, and produces
`build/<board>/agentos.img` with `cargo xtask gen-image`.

**Parity rule.** `RT/agentos.toml` (what is bundled) and
`RT/src/system_desc_aarch64.c` (what is spawned) must name the same PD set.
A bundle entry with no descriptor row is dead weight that never starts; a
descriptor row with no bundle entry fails at ELF load. The Makefile appends
`guest_vmm_secondary`, `fault_inject`, and `event_bus` + `test_runner` to
both for the image variants that need them. `make lint-source` fails if a
retired PD reappears in the default descriptor.

The `agentOS boot complete` marker that `make test` waits for is printed by
`cc_pd`, the lowest-priority PD in the image, right before it enters its
request loop.

## 2. Declaring a PD

A PD is one row in the `pd_desc_t` table in `RT/src/system_desc_aarch64.c`.
The type is in `RT/include/system_desc.h`:

| Field | Meaning |
|-------|---------|
| `name`, `elf_path` | PD name (also its `agentos.toml` name) and `<name>.elf` in the bundle |
| `stack_size`, `cnode_size_bits` | stack bytes (page-aligned) and log2 of the PD's CNode size |
| `priority` | seL4 priority. Providers run above their clients (see the priority DAG comment at the top of the descriptor) |
| `self_svc_id` | `SVC_ID_*` of this PD's own listen endpoint, or 0. The root task mints it at `PD_CNODE_SLOT_SELF_EP` and passes that slot as the first argument to `_start` |
| `init_eps[]` | `{ SVC_ID_x, PD_CNODE_SLOT_x_EP }` pairs: badged endpoint caps to other PDs, minted into this PD's CNode at boot. This is the capability graph; a PD can only call what is listed here |
| `irqs[]` | `irq_desc_t { irq_number, ntfn_badge, name }`; the root task calls `seL4_IRQControl_Get` and places the handler cap at `PD_IRQHANDLER_SLOT_BASE + i`. Driver PDs only |
| `device_frames[]` | `device_frame_desc_t { paddr, size_bits, cnode_slot, name }`; the MMIO frame. Driver PDs only, one owner per frame |
| `memory_regions[]` | `memory_region_desc_t { vaddr, size, name }`; 2 MB-aligned regions such as `guest_ram` |

The endpoint for a service is allocated once by `ep_alloc_for_service()`
(`RT/src/ep_alloc.c`) and minted with a per-client badge into each client's
CNode, so the server can tell callers apart by badge. `SVC_ID_*` and
`PD_CNODE_SLOT_*` constants live in `RT/include/system_desc.h`; add new ones
there.

Two worked rows in the descriptor:

- `serial_pd` (a driver): `irqs = { 33, "pl011-uart" }`,
  `device_frames = { 0x09000000, 12 bits, "pl011-mmio" }`, priority 225.
- `net_virt` (a virtualizer): `irq_count = 0`, `device_frame_count = 0`,
  priority 205 (below `net_pd` at 207, which it calls), `init_eps` holding
  the nameserver, log_drain, serial, `net_pd`, and guest VMM endpoints.

**Shared regions are mapped by name in `main.c`**, not by descriptor field.
`RT/src/main.c` allocates the shared net frame and the shared block region
once and maps them into the PDs whose names it checks (`net_pd`, `net_virt`,
and every guest VMM for the net frame at `AGENTOS_NET_SHARED_VA`; the block
region likewise; the serial transfer page into `serial_pd`, `log_drain`,
`cc_pd`, `net_virt`, `test_runner`, and the VMMs). When a new PD needs one of
these regions, add its name to the relevant condition in `main.c` and record
the mapping in `docs/TCB.md`. Nothing else may map a device frame.

## 3. PD skeleton

Every service PD is linked with `RT/src/pd_entry.c`, which defines `_start`:
it points `__sel4_ipc_buffer` at the root-task-mapped IPC page
(`PD_IPC_BUF_VA`) and calls

```c
void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep);
```

`my_ep` is the CNode slot of the PD's own listen endpoint (from
`self_svc_id`), `ns_ep` the nameserver endpoint slot. A missing `pd_main`
fails the link on purpose.

The shape used by `platform/net-virt/net_virt.c`:

```c
void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    agentos_log_boot("net_virt");
    /* reset local state */
    register_with_nameserver(ns_ep);        /* OP_NS_REGISTER Call */
    nv_puts("[net_virt] READY: ...\n");     /* serial_log marker */
    net_virt_run(my_ep);                    /* seL4_Recv loop, never returns */
}
```

The loop is a plain `seL4_Recv(ep, &badge)`; the message label selects
between a Call to reply to (`seL4_Reply`, or `seL4_Send` on the reply cap
under MCS) and a notification to service. Unknown labels are ignored.

**Logging.** Non-driver PDs must never map the UART. Use `serial_log_t` from
`RT/include/serial_log.h`: a line buffer written into the shared serial
transfer page and flushed to `serial_pd` with the `MSG_SERIAL_*` contract.
`log_drain_write` is the generic path, but its `MSG_SERIAL_WRITE` request
layout does not match `serial_pd`'s handler, so generic PD log output is
silent on the release kernel (see `CHANGELOG.md`, Known limitations). Any
marker a test must see goes through `serial_log`.

Build rules: add `$(BUILD_DIR)/<pd>.o` and `$(BUILD_DIR)/<pd>.elf` rules to
`RT/Makefile`, add `<pd>.elf` to `IMAGES`, and add a `PD_<PD>_SRCS` line for
the shared objects it links (`src/m3_bare_metal.c src/log.c` at minimum).
`net_virt` is the template: its sources are under `platform/net-virt/` and
compiled with the plain PD toolchain, with a `.pd.o` suffix keeping the pump
object apart from the copies `vmm.mk` compiles into the VMMs.

## 4. Contracts

Every live PD exposes one contract before it has callers.

- IPC control contracts: `RT/include/contracts/<pd>_contract.h`.
- Queue layouts for bulk data: `platform/include/platform/*_layout.h`
  (`net_layout.h`, `blk_layout.h`, `serial_layout.h`). These headers have no
  seL4 dependency so host tests can compile them.

A contract header contains a version constant, opcode/label constants,
status codes, and packed request/reply structs whose size is asserted where
they are used. `RT/include/contracts/net_virt_contract.h` is the worked
example:

```c
#define NET_VIRT_CONTRACT_VERSION   1u
#define NET_VIRT_OP_ATTACH          0x2201u   /* Call, VMM -> net_virt */
#define NET_VIRT_EVENT_KICK         0x2210u   /* NBSend, VMM -> net_virt */

typedef struct __attribute__((packed)) {
    uint32_t version;    /* NET_VIRT_CONTRACT_VERSION */
    uint32_t client_id;  /* queue stride index */
    uint32_t vmm_slot;   /* NET_VIRT_VMM_SLOT_* */
} net_virt_attach_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;     /* NET_VIRT_OK / NET_VIRT_ERR_* */
    uint32_t version;
    uint32_t hw_state;   /* NET_VIRT_HW_NONE / NET_VIRT_HW_NET_PD */
    uint8_t  mac[6];
    uint8_t  _pad[2];
} net_virt_attach_reply_t;
```

Payloads travel in `sel4_msg_t.data` with the opcode in the message label.
The header comment documents the transport (which labels are Calls, which are
NBSends) and the signalling protocol. `blk_virt_contract.h` follows the same
pattern with `BLK_VIRT_OP_ATTACH`, `BLK_VIRT_EVENT_KICK`, and
`BLK_VIRT_EVENT_RESP_READY`.

`cargo xtask gen-abi` validates per-PD opcode tables against
`tools/abi_spec.toml` and fails on collisions.

## 5. Notifications vs. IPC

Bulk data never crosses IPC. The rule is:

- **One Call to attach.** The client (a VMM or a native agent) does a single
  `ATTACH` Call to the virtualizer, naming its queue stride (`client_id`) and
  the endpoint it wants events on (`vmm_slot`).
- **Data in sDDF-shaped queues** in a shared region mapped by the root task
  into the client and the virtualizer (`AGENTOS_NET_SHARED_VA`, one 512 KB
  stride per client in a 2 MB frame for net; the shared block region for
  blk).
- **Kicks are `seL4_NBSend`** with a label and no payload. The sender never
  blocks. A kick that lands while the receiver is not in `Recv` is dropped,
  so the protocol makes every kick re-sendable:
  - The consumer owns a `consumer_signalled` flag per queue (sDDF style;
    `tx_active.consumer_signalled` and `rx_free.consumer_signalled` for net,
    the `req_consumer_signalled` word for blk). The consumer sets it to 1
    while draining and to 0 right before it blocks.
  - The producer kicks only when the queue is non-empty and the flag is 0.
    The producer never writes the flag, so a lost kick is repeated on the
    next guest MMIO exit.
  - Before blocking, the virtualizer rescans every queue and polls the driver
    once more, so a lost notification from the driver costs latency, not
    data.
- **The virtualizer alone speaks the driver's contract.** `net_virt` holds
  the only `net_pd` endpoint and does `RAW_SEND`/`RAW_RECV` Calls; `net_pd`
  NBSends `RX_READY` to `net_virt`, never to a VMM. No VMM holds a `net_pd`
  endpoint. `blk_virt` likewise holds the only `virtio_blk` endpoint and is
  the only non-driver PD that maps the bounded DMA window.

`tests/platform/lint_source_invariants.c` (`inv2:` checks) fails the build if
a VMM row acquires a driver endpoint or a virtualizer row acquires a device
frame or IRQ.

## 6. Adding a virtualizer or driver PD

Use the `net_virt` change as the template:

```bash
git log --oneline origin/main -- platform/net-virt
git show --stat 98172dd5      # net_virt: make the network virtualizer a real PD (#123)
```

The order that worked:

1. **Layout first.** Define or reuse the queue layout in
   `platform/include/platform/<class>_layout.h`, with static asserts on
   offsets and strides. Write a host-testable pump over it
   (`platform/<class>-virt/<class>_virt_pump.c`) and a host test
   (`tests/platform/test_<class>_virt_pump.c`).
2. **Contract.** Add `RT/include/contracts/<class>_virt_contract.h` with a
   version, `ATTACH`, `KICK`, and any event labels, plus packed structs.
3. **PD source.** `platform/<class>-virt/<class>_virt.c` with `pd_main`,
   nameserver registration, a `serial_log` READY marker, and the `Recv` loop
   from section 3.
4. **Descriptor row** in `RT/src/system_desc_aarch64.c`: no IRQs, no device
   frames, `self_svc_id` set, `init_eps` listing the driver endpoint and each
   client VMM endpoint, priority between the driver (above) and the VMMs it
   serves. Add the `SVC_ID_*` and `PD_CNODE_SLOT_*` constants to
   `RT/include/system_desc.h`.
5. **Manifest.** Add the PD to `RT/agentos.toml` (same name, priority noted
   for parity).
6. **Shared region.** In `RT/src/main.c`, add the PD name to the condition
   that maps the class's shared region. Remove the driver endpoint from the
   VMM rows so the VMM can no longer bypass the virtualizer.
7. **Client side.** Change the VMM glue (`platform/<class>-virt/vmm_virtio_<class>.c`)
   to do one `ATTACH` Call and then only `NBSend` kicks; drop any per-item
   IPC to the driver.
8. **Build.** `RT/Makefile`: object and ELF rules, `IMAGES`, `PD_*_SRCS`;
   `RT/vmm.mk` if the VMM side changed.
9. **Lint.** Add `inv2:` checks to `tests/platform/lint_source_invariants.c`
   for the new PD: exists in topology and manifest, no device caps, holds the
   driver EP, VMMs hold no driver EP, driver holds no VMM EP.
10. **Proof.** Add or extend the guest proof markers in `xtask/src/cmd_test.rs`
    so `make test-guest-<class>` and `make test-ubuntu-virtio` require the
    virtualizer's host-backed markers. Update `docs/TCB.md` ("How I/O flows
    today", invariant 2) and `CHANGELOG.md`.

For a **driver PD** the row differs: exactly one `device_frames` entry and
one `irqs` entry, a `self_svc_id`, and no client endpoints other than the
virtualizer's. `serial_pd` (`services/serial-mux/serial_pd.c`) and `net_pd`
(`services/net-service/net_pd.c`) are the examples. Under QEMU the "device"
is a QEMU virtio-mmio device on a bus that is never advertised to guests
(bus.8 block, bus.16 net, bus.2 control console).

Do not add `MSG_*_SEND`-style opcodes that carry payload through IPC
registers as a substitute for queues; that is the pattern invariant 2 exists
to remove.

## 7. Adding a guest profile

Guest identity is data (`docs/guest-profiles.md`). A profile is a TOML file
under `guest-profiles/` that `cargo xtask guest-profile` validates and
compiles into a fixed 640-byte manifest consumed by the VMM.

```text
schema = 2
id = "buildroot-proof-aarch64"
status = "runtime"                  # abstract | planned | runtime
extends = "linux-base.toml"
aliases = ["buildroot", "linux"]    # what GUEST_OS=<alias> resolves to

[boot]           command_line, optional media_initrd_path
[artifacts.*]    kernel / dtb / initrd: source, cache path, sha256, max_bytes
[placements.*]   ram_size, dtb_load_address per slot placement
[[host.acquire]] bounded acquisition recipe (download, extract, convert)
[host.build]     DTB template merge (linux-virtio-overlay.dts.in)
[host.qemu]      machine, memory, host media, bus numbers
[host.console]   expect rules: markers, sends, retry ceilings
[[host.test]]    assert-virtio devices and scope (emulated | host-backed)
```

`runtime` profiles need pinned SHA-256 for every artifact. The compiler
rejects unknown fields, unbounded text, absolute or `..` paths, and artifacts
outside guest RAM. Recipe actions and DTB adapters come from closed Rust
whitelists in `xtask/src/`; the target never interprets TOML.

Steps:

1. Copy the nearest profile (`buildroot.toml`, `ubuntu-e2e.toml`,
   `freebsd.toml`, or `debian.toml`) and set `id`, `aliases`, artifacts, and
   recipes. Start with `status = "planned"` until the hashes are pinned.
2. `make guest-profile-check` (runs `cargo xtask guest-profile --check-all`
   and the host profile tests).
3. `make fetch-guest GUEST_PROFILE=<file>.toml`, then
   `make run GUEST_PROFILE=<file>.toml`.
4. For a proof, add `[[host.test]]` entries and run
   `cargo xtask qemu-test --guest-os <alias> --assert-emulated-net` (or
   `-blk`, `-console`, `--assert-agentos-virtio`).
5. Multi-guest compositions go in `guest-scenarios/*.toml`
   (`dual-release.toml` is `GUEST_OS=both`).

Adding a profile does not add a VMM personality; there is one guest-neutral
`guest_vmm.c`. Do not add guest drivers: guests boot stock kernels with
in-tree virtio drivers and an FDT.

## 8. Writing a native agent

The target shape (`docs/TCB.md`): a native agent is a client of `net_virt`
and `blk_virt`, exactly like a VMM backend. It gets a queue stride in the
shared region, does one `ATTACH` Call, and moves frames or block requests
through the sDDF queues with kicks. It owns no device and is not in the TCB.

What exists today:

- `RT/src/native_net_client.c` with `RT/include/native_net_client.h` and the
  host test `tests/platform/test_native_net_client.c`. It is the client
  described in the 0.2.0 changelog, but it speaks `net_pd`'s raw-frame
  contract directly, which predates `net_virt`, and its only in-tree user
  (`init_agent`) is no longer in the image. It is host-tested, not booted.
- No native PD attaches to `net_virt` or `blk_virt` yet. PLAN.md step 9
  (`task_ec992e5743354a538d1c3235a2e2c0da`) tracks it.

To build one now, follow section 6 from the client side: a PD row with a
`net_virt` (or `blk_virt`) endpoint in `init_eps`, its name added to the
shared-region mapping condition in `main.c`, one `ATTACH` Call using the
contract structs, then produce into `tx_active` / consume from `rx_active`
with `platform/include/platform/net_layout.h` and kick per section 5.
`platform/net-virt/vmm_virtio_net.c` is the reference client, minus the
virtio emulation.

WASM is a guest or agent binary format only; the interpreter in
`RT/wasm3/` is museum code and is not a path to I/O.

## 9. Testing policy

| Layer | Command | Proves |
|-------|---------|--------|
| Policy | `make policy-check` | No forbidden languages or UI files; xtask formatted |
| Source lint | `make lint-source` | `docs/TCB.md` invariants are visible in the compiled topology, headers, FDTs, profiles. Not a test |
| Host tests | `make test-host` (includes the two above, `guest-profile-check`, `test-integration`) | Logic and struct shapes with seL4 IPC stubbed (`-DAGENTOS_TEST_HOST`, `tests/microkit.h`). Cannot be cited for I/O or IPC |
| Boot | `make test TARGET_ARCH=aarch64 GUEST_OS=none`, same for `x86_64` | PDs load, root task parks. Stub VMM |
| Guest I/O | `make test-guest-net`, `test-guest-blk` (Buildroot), `test-guest-console`, `test-ubuntu-virtio` (Ubuntu) | A guest saw the emulated device, reached `DRIVER_OK`, and real I/O crossed the virtualizer |
| Gate | `make gate` | All of the above except `test-ubuntu-virtio`. Required before any OS-level claim |
| Acceptance | `make demo-test` | Concurrent Ubuntu + FreeBSD with key-only SSH |
| Nightly | `make test-ubuntu-live` | Casper live filesystem to login; not a per-push gate |

A device-class claim needs its guest I/O assertion through the virtualizer,
not a host test and not QEMU bus ownership. `xtask qemu-test` fails a
host-backed proof that satisfied itself with the VMM-local loopback.

CI (`.github/workflows/ci.yml`) runs host tests, both boot gates, the
Buildroot net and blk proofs, and the Ubuntu VirtIO proof, and folds them
into the `OS-claim gate (boot + guest net/blk/console proofs)` summary job.

## 10. Language and UI policy

First-party code is **C, Rust, or Assembly**: target code, host tools, tests,
generators, and skill helpers. Python, JavaScript, TypeScript, HTML, CSS, Go,
Zig, and other implementation languages are forbidden, including in vendored
code. CMake and Make orchestrate builds; shell is CI glue and one-line
wrappers. `cargo xtask policy-check` enforces this on every `make test-host`.

No human UI in this repository: no dashboards, HTML, WebSocket terminals, or
interactive TUIs. `tools/agentctl` (a CLI with structured stdout) is the only
in-tree consumer of `cc_pd`; GUIs are external consumers of the contracts.

## 11. What not to extend

`docs/TCB.md`, section "What is not TCB (museum)", lists the PDs and ideas
that must not be extended, given new opcodes, or "finished": `oom_killer`,
`spawn_server`, `proc_server`, `term_server`, `app_manager`, `http_svc`,
`wg_net`, `vibe_swap`/`swap_slot` as a path to I/O, `gpu_shmem` as a guest
channel, CapStore/MsgBus/ModelSvc/ToolSvc as core OS, and the rest of that
list. Their sources still compile from `RT/Makefile`'s `IMAGES` list but they
are not bundled or booted. If you find one in your way, file a task; do not
build on it.

Also forbidden as architecture: passing a QEMU virtio device through to a
guest, adding a guest-specific driver for a class that has a virtualizer,
carrying bulk data through IPC registers, and cloud or LLM SDKs inside PDs.

## 12. Tasks and workflow

Work is tracked with `mac task` in project `agentos`, not GitHub issues or
TODO lists:

```bash
mac task ready --project agentos --limit 10
mac task show <id>
mac task create "title" --project agentos --description-file=desc.txt --no-dispatch
mac task close <id> --reason="..."
```

Commit messages follow `platform: <short description>` with a `Refs:
<mac-task-id>` trailer (`AGENTS.md`). Before pushing: `make test-host` for
any change, `make gate` for anything that touches a booted PD, and update
`docs/TCB.md` if the booted PD set or an I/O path changed. Work is not done
until `git push` succeeds.

## Reference map

| Topic | Where |
|-------|-------|
| Trust boundary, invariants, museum list | `docs/TCB.md` |
| Root task boot | `RT/src/main.c`, `RT/src/start_aarch64.S`, `docs/sel4-loader-format.md` |
| PD descriptor | `RT/src/system_desc_aarch64.c`, `RT/include/system_desc.h` |
| PD entry and logging | `RT/src/pd_entry.c`, `RT/include/serial_log.h` |
| IPC contracts | `RT/include/contracts/` |
| Queue layouts, GPA translation | `platform/include/platform/` |
| Virtualizers | `platform/net-virt/`, `platform/blk-virt/`, `platform/serial-virt/` |
| VMM | `RT/src/guest_vmm.c`, `platform/guest-vmm/`, `libvmm/` |
| Guest profiles | `docs/guest-profiles.md`, `guest-profiles/`, `guest-scenarios/` |
| Test harness | `xtask/src/cmd_test.rs`, `tests/TARGET_TESTS.md` |
| Roadmap and releases | `docs/ROADMAP.md`, `docs/RELEASES.md`, `PLAN.md` |
| Operating notes per block | `skills/*/SKILL.md` |
