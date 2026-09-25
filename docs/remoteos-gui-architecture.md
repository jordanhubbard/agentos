# AgentOS GUI and RemoteOS-SDL Architecture

**Status:** Accepted design decision, 2026-09-25. Runtime relay integration is
not yet implemented or qualified.

## Decision

AgentOS has one supported operator application: `agentos_gui`.
It remains responsible for guest lifecycle, inventory, device state, serial
console, logs, traces, topology, snapshots and connection management through
the CC contract.

RemoteOS-SDL is the common graphical presentation backend beneath that
application. It owns host window, pixel presentation, keyboard, pointer and
audio mechanics. `agentos_gui` must not grow a second framebuffer renderer,
and RemoteOS-SDL must not grow AgentOS lifecycle or control-plane policy.

This is consolidation by responsibility, not a repository or process merge:

| Component | Owns | Does not own |
|---|---|---|
| agentOS | Guest resources, virtual devices, framebuffer and input queues, CC authorization | Host windows, SDL devices, a human UI |
| `agentos_gui` | The AgentOS operator experience and selection of the active guest | Pixel rendering, SDL input devices, on-target capabilities |
| AgentOS display relay | Translation between one selected CC framebuffer/input stream and one RemoteOS connection | Guest policy, device frames, IRQs, a second UI |
| RemoteOS-SDL | Host display, input and audio presentation behind protocol v2 | AgentOS guest lifecycle, CC sessions, seL4 capabilities |

The result is one AgentOS GUI assembled from a control plane and a reusable
presentation service, rather than two independently evolving GUIs.

## Required data flow

```text
                              host boundary
 agentOS                                              RemoteOS-SDL

 guest virtio-gpu -> framebuffer_queue -> CC observer -> relay -> surface.upload
 guest virtio-input <- input_virt       <- CC input    <- relay <- SDL events
 guest console      <-> serial_virt     <-> CC         <------> agentos_gui
 lifecycle/logs/traces/snapshots        <-> CC         <------> agentos_gui
```

The relay is a host-side part of the `agentos_gui` deployment. It may be an
in-process native module or a supervised sidecar, but it consumes the same
public CC contract as any other external client. It must not be introduced as
a new privileged on-target PD merely to reach host SDL.

For each selected guest the relay:

1. connects to CC and resolves the public guest handle;
2. opens and negotiates one RemoteOS protocol-v2 display;
3. captures immutable committed frames through the CC framebuffer observer;
4. uploads bounded pixel payloads and commits the RemoteOS frame;
5. converts RemoteOS keyboard and pointer events to the versioned CC input
   event format for that same public guest handle;
6. releases held input and destroys presentation state on disconnect, guest
   replacement or teardown.

Serial console remains a control-plane surface in `agentos_gui`; it is not a
surrogate for graphical display. Audio is a future RemoteOS-backed data path
and does not become an AgentOS claim until an AgentOS audio contract and target
gate exist.

## Trust and security boundary

RemoteOS-SDL and the relay are untrusted host software. Neither is part of the
agentOS TCB, and neither may receive an seL4 capability, guest RAM mapping,
device frame, IRQ or direct QEMU transport mapping.

The following invariants are mandatory:

- Pixel export uses the existing immutable CC observer/snapshot mechanism.
  The relay never maps the framebuffer service's private surface arena.
- Input returns through the existing CC input contract and `input_virt` queue.
  A host event is always scoped to a resolved public guest handle.
- QEMU VirtIO-MMIO, UART and SDL devices remain outside guest and relay
  mappings. Driver PDs remain their sole on-target owners.
- Bulk pixels do not move through seL4 message registers. AgentOS queues and
  bounded CC transfers remain the OS boundary; RemoteOS binary trailers are a
  host protocol detail.
- RemoteOS connections use a local Unix socket or loopback endpoint by
  default. A remote connection requires an authenticated tunnel; protocol v2
  is not itself an authorization boundary.
- A disconnect fails closed: stop presenting, release held input, discard
  connection-scoped RemoteOS handles, then renegotiate before resuming.

The transport-independent C client in
`kernel/agentos-root-task/src/remoteos_client.c` is currently protocol
foundation and host-test material. It is not linked into the release image and
does not establish an on-target relay, framebuffer path or input path.

## Integration sequence

1. Keep the protocol-v2 framing client covered by deterministic mock tests and
   a live headless RemoteOS-SDL interoperability test.
2. Add the host relay to the external `agentos_gui` deployment, using CC frame
   capture and input APIs without importing kernel headers or private memory.
3. Make RemoteOS-SDL the only graphical viewport backend exposed by
   `agentos_gui`; remove or decline any parallel pixel renderer.
4. Qualify frame identity, exact pixels, keyboard and pointer delivery, held
   input release, reconnect, suspend/resume and guest recreation on the same
   revision pair.
5. Only then describe the combined application as the supported graphical
   AgentOS experience. Keep the protocol foundation claim separate until that
   evidence exists.

## Rejected alternatives

### Replace `agentos_gui` with RemoteOS-SDL

Rejected. RemoteOS-SDL is a presentation service, not an AgentOS control
plane. Reimplementing lifecycle, logs, traces, topology and snapshots there
would duplicate CC policy and couple a reusable renderer to one OS.

### Maintain two graphical renderers

Rejected. Independent AgentOS and RemoteOS framebuffer/input stacks would
double platform behavior, testing and accessibility work, and would allow
their event semantics to drift.

### Add a display relay PD that owns the host device

Rejected. It would widen the TCB and violate the one-owner device rule. The
existing framebuffer observer and input virtualizer already provide the
bounded OS-side contracts needed by an external relay.

## Current qualification boundary

The repository currently proves protocol-v2 request framing, bounded parsing,
binary surface upload, error handling and interoperability with a live
headless RemoteOS-SDL service. Those checks do not prove CC frame forwarding,
RemoteOS event forwarding, graphical guest operation, audio, physical display
hardware or a released integrated `agentos_gui` build. Those remain tracked by
MAC task `task_e935516782f647c2891ad93c2aa7e48e`.
