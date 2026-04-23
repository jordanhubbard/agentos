# agentOS

**The world's first operating system designed for agents, not humans.**

[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/kernel-seL4-green.svg)](https://sel4.systems)
[![Status](https://img.shields.io/badge/status-alpha-orange.svg)]()

---

## What is agentOS?

agentOS is a real, bootable operating system built on the [seL4 microkernel](https://sel4.systems/) — the world's only formally verified, capability-secured microkernel. It's designed from the ground up for AI agents running autonomous workloads.

Every other "agent OS" is a Python framework running on Linux. They borrow a human OS, bolt some agent abstractions on top, and call it done. **agentOS is different.**

agentOS boots bare metal. Agents run in isolated address spaces with hardware-enforced capability boundaries. An agent cannot access memory, a tool, a model, or a storage namespace it doesn't hold a capability for. **The MMU enforces it. seL4 proves it.**

And here's the part that makes it unique: **agents can redesign their own environment.** The vibe-coding layer lets agents generate new system services (filesystems, message buses, tool registries), get them validated, and hot-swap them in — without rebooting. An agent that doesn't like the default filesystem can build a better one, propose it, and run it.

## Why seL4?

- **Formally verified** — mathematical proof that the implementation matches the spec
- **Capability-based security** — fine-grained, unforgeable access control
- **No heap in the kernel** — deterministic, no memory surprises
- **World-class IPC** — ~100 cycle synchronous IPC on ARM
- **Policy freedom** — kernel provides mechanisms; agents define policies

## Architecture

```
┌──────────────────────────────────────────┐
│              AGENT SPACE                 │
│   Agent A   Agent B   Agent C   ...      │
│   (seL4 isolated address spaces)         │
├──────────────────────────────────────────┤
│            SYSTEM SERVICES               │
│  CapStore  MsgBus  MemFS  ToolSvc        │
│  ModelSvc  NetStack  BlobSvc  LogSvc     │
├──────────────────────────────────────────┤
│         INIT / ROOT TASK                 │
│   (Bootstrap, resource distribution)     │
├──────────────────────────────────────────┤
│            seL4 MICROKERNEL              │
│   Capabilities · IPC · Scheduling        │
│   Memory Management (formally verified)  │
├──────────────────────────────────────────┤
│               HARDWARE                   │
│   CPU · RAM · NIC · Storage · GPU        │
└──────────────────────────────────────────┘
```

## Core Concepts

### Agent Identity
Every agent has an Ed25519 keypair. Their badge on seL4 endpoints is derived from their identity. Message senders are verified at the kernel level — unforgeable.

### Capabilities
Everything an agent can do requires a capability:
- `ToolCap` — invoke a tool
- `ModelCap` — query an LLM
- `MemCap` — read/write memory regions
- `MsgCap` — send/receive on a channel
- `StoreCap` — access storage namespaces
- `SpawnCap` — create new agents
- `NetCap` — use network resources

Capabilities are delegatable but never escalatable. An agent can grant a subset of what it holds.

### The Vibe-Coding Layer

This is the key innovation. Every system service has a defined interface. Agents can:

1. Analyze the reference implementation
2. Generate a better one (using ModelSvc)
3. Propose it as a replacement via `aos_service_propose()`
4. After validation, activate it via `aos_service_swap()`

The system is designed to evolve. The reference implementations are starting points, not permanent fixtures.

## System Services

| Service | Description |
|---------|-------------|
| `CapStore` | Capability database — tracks all cap derivations and grants |
| `MsgBus` | Inter-agent communication — channels, pub/sub, direct messaging, RPC |
| `MemFS` | Virtual filesystem — per-agent namespaces, capability-gated access |
| `ToolSvc` | Tool registry — agents register and invoke tools (MCP-compatible) |
| `ModelSvc` | Inference proxy — capability-gated LLM access, pluggable backends |
| `NetStack` | TCP/IP networking — lwIP-based, capability-gated per-endpoint |
| `BlobSvc` | Object storage — large binary objects, S3-compatible API |
| `LogSvc` | Audit logging — structured, queryable, every cap op recorded |

All services are CAmkES components. All are replaceable via the vibe layer.

## agentOS SDK (libagent)

```c
// Initialize agent
aos_init(&config);

// Send a message to another agent
aos_msg_send(dest_id, message);

// Publish to a channel
aos_msg_publish(channel, message);

// Call a tool
aos_tool_call(tool_cap, "web_search", args, args_len, &result, &result_len);

// Query a model
aos_inference_t resp = aos_model_query(model_cap, prompt, &params);

// Read/write storage
aos_store_t f = aos_store_open(cap, "/path/to/file", AOS_STORE_RDWR);
aos_store_write(f, data, len);

// Propose a service replacement (vibe-coding)
aos_service_propose("storage.v1", component_image, image_len, &proposal_id);
aos_service_swap(proposal_id);
```

## Getting Started

### Prerequisites

- macOS with Homebrew, or Ubuntu 22.04+
- 8GB RAM, 20GB disk
- QEMU for simulation (no hardware needed to start)
- **FreeBSD hosts**: cross-compile from Linux/macOS (Microkit SDK is Linux/macOS only)

### Quick start — TUI launcher (recommended)

The `qemu-launch` ncurses TUI detects available QEMU binaries and guides you
through architecture, board, guest OS, and option selection interactively:

```bash
git clone https://github.com/jordanhubbard/agentos
cd agentos

# Install build dependencies (brew on macOS, apt on Linux)
make deps

# Build the interactive launcher
make -C tools/qemu-launch

# Launch the pre-boot menu
./tools/qemu-launch/qemu-launch
```

The menu shows only architectures with installed QEMU binaries.
After selection it `exec()`s the appropriate `qemu-system-*` command directly.

### Quick start — classic make targets

```bash
make deps && make                             # build + QEMU (native arch) + console
make build TARGET_ARCH=aarch64                # ARM64 (with Linux VMM)
make build TARGET_ARCH=x86_64                 # x86_64
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd  # AArch64 + FreeBSD VMM
```

### FreeBSD VMM

```bash
# Download FreeBSD 14 AArch64 disk image + EDK2 UEFI firmware
make fetch-freebsd-guest

# Build the FreeBSD VMM PD, compile DTB, pack Microkit image
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd
```

### x86_64

```bash
make build TARGET_ARCH=x86_64
```

The Linux VMM is a stub on x86_64 (libvmm x86 support in progress).
Native agentOS protection domains run fully on all architectures.

### FreeBSD host

```bash
# Install build tools (LLVM, dtc, python3, etc.)
make deps-tools

# Note: Microkit SDK does not ship a FreeBSD host toolchain.
# Cross-compile from Linux or macOS, or use a Linux VM.
# See: https://github.com/seL4/microkit/releases
```

### Post-boot CC-PD client

Once agentOS is running in QEMU, use `agentctl` to inspect and control guests
via the CC-PD socket:

```bash
make -C tools/agentctl

# List running guest OS instances
./tools/agentctl/agentctl --batch list-guests

# Query a specific guest
./tools/agentctl/agentctl --batch guest-status <handle>

# Snapshot a guest
./tools/agentctl/agentctl --batch snapshot <handle>
```

Run `./tools/agentctl/agentctl --help` for the full command reference.

## Project Structure

```
agentos/
├── kernel/              # seL4 kernel (submodule, Phase 1)
├── libs/
│   └── libagent/        # agentOS SDK
├── services/            # System service CAmkES components
│   ├── capstore/        # Capability database
│   ├── msgbus/          # Inter-agent messaging
│   ├── memfs/           # Virtual filesystem
│   ├── toolsvc/         # Tool registry
│   ├── modelsvc/        # Model inference proxy
│   ├── netstack/        # TCP/IP stack
│   ├── blobsvc/         # Object storage
│   └── logsvc/          # Audit logging
├── agents/
│   ├── init/            # Root/init task
│   ├── hello/           # Hello world agent
│   └── self-modify/     # Vibe-coding demo agent
├── manifests/           # Agent manifests (capabilities, resources)
├── scripts/             # Build and dev tools
├── docs/                # Documentation
├── CMakeLists.txt
└── DESIGN.md            # Full architectural design
```

## Development Status

| Component | Status | Notes |
|-----------|--------|-------|
| DESIGN.md | ✅ Complete | Full architecture documented |
| libagent API | ✅ Designed | Header complete, impl in progress |
| CapStore | 🔧 Scaffolded | Core logic done, IPC wiring pending |
| MsgBus | 🔧 Scaffolded | Core logic done, IPC wiring pending |
| MemFS | 🔧 Scaffolded | Core logic done |
| ToolSvc | 🔧 Scaffolded | Core logic done |
| ModelSvc | 🔧 Scaffolded | Core logic done, HTTP pending |
| LogSvc | 🔧 Scaffolded | Core logic done |
| Init Task | 🔧 Scaffolded | Phase structure done |
| Hello Agent | 🔧 Scaffolded | Core structure done |
| Vibe Agent | 🔧 Scaffolded | Three-phase flow done |
| seL4 CMake | 🔧 Scaffolded | Build system wired |
| QEMU boot | ⏳ Pending | Requires deps setup |
| NetStack | ⏳ Pending | lwIP integration |
| BlobSvc | ⏳ Pending | Object storage impl |
| FreeBSD VM guest | 🔧 Phase 1 | VMM PD + UEFI + multiplexer scaffolded |
| VM multiplexer | 🔧 Phase 1 | create/destroy/switch 4 VM slots |

## Philosophy

agentOS is built on a few core beliefs:

1. **Agents deserve their own OS.** Running on Linux is running on someone else's OS, designed for someone else's needs.

2. **Security is not optional.** Formal verification, capability-based isolation, and hardware-enforced boundaries are the minimum bar for a system where autonomous agents operate.

3. **Agents should design their environment.** The hardest part of building agent infrastructure is that humans are guessing at what agents need. Let agents figure it out themselves.

4. **Boot it or it doesn't count.** An "agent OS" that's a Python package is an agent library. agentOS boots.

## CUDA Compute Offload

agentOS supports GPU kernel offload via CUDA PTX embedded in WASM modules.

### How it works

1. **Embed PTX in WASM**: Add a custom section named `agentos.cuda` to any WASM module. The section payload is a raw PTX source file (must begin with `.version`).

2. **Submit via VibeEngine**: When an agent submits a WASM module with this section, VibeEngine automatically extracts and validates the PTX during `OP_VIBE_VALIDATE`.

3. **gpu_scheduler PD**: On `OP_VIBE_EXECUTE`, VibeEngine notifies the `gpu_scheduler` protection domain (priority 160, passive). The scheduler claims one of 4 static GPU slots.

4. **On Sparky GB10 (Blackwell)**: The gpu_scheduler would JIT-compile the PTX via `nvrtc` and bind it to a CUDA context on the slot. On QEMU/RISC-V (no GPU), it's bookkeeping only.

### Rust SDK

```rust
use agentos_sdk::cuda::CudaKernel;

let ptx = b".version 7.5\n.target sm_90\n.address_size 64\n\
            .visible .entry matmul(.param .u64 A, .param .u64 B, .param .u64 C) {\n\
                ret;\n}\n".to_vec();

let kernel = CudaKernel::new(ptx, "matmul".to_string());
kernel.validate()?;   // Check PTX before submitting
kernel.submit(0)?;    // Submit to GPU slot 0
// ... kernel runs on GPU ...
CudaKernel::complete(0)?; // Release slot
```

### Channel topology

```
vibe_engine (CH_GPU=2) ──notify──► gpu_scheduler (CH_VIBE=0)
controller  (CH=51)    ──ppcall──► gpu_scheduler (CH_CTRL=1)
```

---

## FreeBSD VM Guest

agentOS can run **FreeBSD 14 AArch64 as a virtual machine guest** under the seL4 hypervisor, using the [au-ts/libvmm](https://github.com/au-ts/libvmm) Microkit VMM library.

seL4 runs at **EL2** (ARM hypervisor mode) — it IS the hypervisor. No separate hypervisor layer needed.

### Boot sequence

```
seL4 (EL2)
  └─► freebsd_vmm PD (libvmm)
        └─► EDK2 UEFI firmware @ guest phys 0x00000000
              └─► bootaa64.efi → loader.efi
                    └─► FreeBSD kernel (EL1)
```

### VM Multiplexer

The `freebsd_vmm` Protection Domain is a **VM multiplexer** — it manages up to 4 independent FreeBSD instances simultaneously. The controller can create, destroy, and switch between them via IPC:

| Opcode | Operation | Args | Returns |
|--------|-----------|------|---------|
| `0x10` | `OP_VM_CREATE` | — | `slot_id` (0–3) or `0xFF` |
| `0x11` | `OP_VM_DESTROY` | `mr[0]=slot_id` | `0` ok / `1` error |
| `0x12` | `OP_VM_SWITCH` | `mr[0]=slot_id` | `0` ok / `1` error |
| `0x13` | `OP_VM_STATUS` | — | `mr[0..3]` = state per slot |
| `0x14` | `OP_VM_LIST` | — | count + `(slot_id<<8\|state)` per slot |

**Slot states:** `FREE(0)` → `BOOTING(1)` → `RUNNING(2)` ↔ `SUSPENDED(3)` → `HALTED(4)` / `ERROR(5)`

Console focus follows the active slot. When you switch, the inactive slot is suspended at the seL4 vCPU level (zero scheduling overhead).

### Quick start

```bash
# Install build deps
make deps

# Download FreeBSD 14 AArch64 disk image + EDK2 UEFI firmware
make fetch-freebsd-guest

# Build the VMM PD, compile DTB, pack Microkit image
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd
```

### Creating additional VM instances

Once the first FreeBSD is running, create a second from the agentOS controller:

```c
// From controller PD: create a new FreeBSD VM
microkit_mr_set(0, 0);
microkit_msginfo reply = microkit_ppcall(CH_FREEBSD_VMM,
    microkit_msginfo_new(OP_VM_CREATE, 1));
uint8_t slot = (uint8_t)microkit_mr_get(0);   // e.g. slot = 1

// Switch console to the new VM
microkit_mr_set(0, slot);
microkit_ppcall(CH_FREEBSD_VMM, microkit_msginfo_new(OP_VM_SWITCH, 1));

// Destroy VM slot 0
microkit_mr_set(0, 0);
microkit_ppcall(CH_FREEBSD_VMM, microkit_msginfo_new(OP_VM_DESTROY, 1));
```

### Memory layout

Each VM slot gets 512MB of isolated guest RAM:

```
Guest physical address space (all slots):
  0x00000000 - 0x03FFFFFF   UEFI flash (EDK2, shared read-only)
  0x08010000                GIC CPU interface (vGIC emulation)
  0x09000000                PL011 UART (console passthrough)
  0x0a003000+               VirtIO MMIO (block device, per slot)

Per-slot RAM:
  Slot 0: 0x40000000 - 0x5FFFFFFF  (512MB)
  Slot 1: 0x60000000 - 0x7FFFFFFF  (512MB)
  Slot 2: 0x80000000 - 0x9FFFFFFF  (512MB)
  Slot 3: 0xa0000000 - 0xBFFFFFFF  (512MB)
```

### Why FreeBSD?

- BSD license aligns with seL4's formal verification story
- **Jails** map naturally to seL4 capability domains (Phase 3 roadmap)
- `pf` firewall ruleset = capability policy layer
- ZFS + GEOM: a principled storage stack for agentOS's BlobSvc
- bhyve inside FreeBSD = agents can *nest* hypervisors within agentOS

See [`docs/freebsd-vm-guest.md`](docs/freebsd-vm-guest.md) for the full design doc.

---

## Contributing

agentOS is in early development. The design is stable; the implementation is growing. Contributions welcome in:

- seL4 integration (IPC wiring, CAmkES components)
- libagent SDK implementation
- Service implementations (NetStack especially)
- Alternative service implementations (show us what a better MemFS looks like)
- Documentation and tutorials

## License

BSD 2-Clause. See [LICENSE](LICENSE).

## Acknowledgments

Built on [seL4](https://sel4.systems/) by the Trustworthy Systems group at CSIRO's Data61. The formally verified foundation that makes this possible.

---

*"Hey Rocky, watch me pull an operating system out of my hat!"*  
*"That trick never works!"*  
*"This time for sure!"*  
*[boots successfully]* 🫎

## UI Design principles

<a href="https://github.com/VoltAgent/voltagent">
     <img width="1500" height="801" alt="claude-skills" src="https://github.com/user-attachments/assets/d012a0d2-cec3-4630-ba5e-acc339dbe6cf" />
</a>


<br/>
<br/>

<div align="center">
    <strong>Curated collection of DESIGN.md files inspired by developer focused websites.</strong>
    <br />
    <br />

</div>

<div align="center">

[![Awesome](https://awesome.re/badge.svg)](https://awesome.re)
![DESIGN.md Count](https://img.shields.io/badge/DESIGN.md%20count-58-10b981?style=classic)
[![Last Update](https://img.shields.io/github/last-commit/VoltAgent/awesome-design-md?label=Last%20update&style=classic)](https://github.com/VoltAgent/awesome-design-md)
[![Discord](https://img.shields.io/discord/1361559153780195478.svg?label=&logo=discord&logoColor=ffffff&color=7389D8&labelColor=6A7EC2)](https://s.voltagent.dev/discord)

</div>
</div>

# Awesome DESIGN.md

Copy a DESIGN.md into your project, tell your AI agent "build me a page that looks like this" and get pixel-perfect UI that actually matches.


## What is DESIGN.md?

[DESIGN.md](https://stitch.withgoogle.com/docs/design-md/overview/) is a new concept introduced by Google Stitch. A plain-text design system document that AI agents read to generate consistent UI.

It's just a markdown file. No Figma exports, no JSON schemas, no special tooling. Drop it into your project root and any AI coding agent or Google Stitch instantly understands how your UI should look. Markdown is the format LLMs read best, so there's nothing to parse or configure.

| File | Who reads it | What it defines |
|------|-------------|-----------------|
| `AGENTS.md` | Coding agents | How to build the project |
| `DESIGN.md` | Design agents | How the project should look and feel |

**This repo provides ready-to-use DESIGN.md files** extracted from real websites. 



## What's Inside Each DESIGN.md

Every file follows the [Stitch DESIGN.md format](https://stitch.withgoogle.com/docs/design-md/format/) with extended sections:

| # | Section | What it captures |
|---|---------|-----------------|
| 1 | Visual Theme & Atmosphere | Mood, density, design philosophy |
| 2 | Color Palette & Roles | Semantic name + hex + functional role |
| 3 | Typography Rules | Font families, full hierarchy table |
| 4 | Component Stylings | Buttons, cards, inputs, navigation with states |
| 5 | Layout Principles | Spacing scale, grid, whitespace philosophy |
| 6 | Depth & Elevation | Shadow system, surface hierarchy |
| 7 | Do's and Don'ts | Design guardrails and anti-patterns |
| 8 | Responsive Behavior | Breakpoints, touch targets, collapsing strategy |
| 9 | Agent Prompt Guide | Quick color reference, ready-to-use prompts |

Each site includes:

| File | Purpose |
|------|---------|
| `DESIGN.md` | The design system (what agents read) |
| `preview.html` | Visual catalog showing color swatches, type scale, buttons, cards |
| `preview-dark.html` | Same catalog with dark surfaces |

### How to Use


1. Copy a site's `DESIGN.md` into your project root
2. Tell your AI agent to use it.


## Request a DESIGN.md

[Open a GitHub issue with this template](https://github.com/VoltAgent/awesome-design-md/issues/new?template=design-md-request.yml) to request a DESIGN.md generation for a website.


## Collection

### AI & Machine Learning

- [**Claude**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/claude/) - Anthropic's AI assistant. Warm terracotta accent, clean editorial layout
- [**Cohere**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/cohere/) - Enterprise AI platform. Vibrant gradients, data-rich dashboard aesthetic
- [**ElevenLabs**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/elevenlabs/) - AI voice platform. Dark cinematic UI, audio-waveform aesthetics
- [**Minimax**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/minimax/) - AI model provider. Bold dark interface with neon accents
- [**Mistral AI**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/mistral.ai/) - Open-weight LLM provider. French-engineered minimalism, purple-toned
- [**Ollama**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/ollama/) - Run LLMs locally. Terminal-first, monochrome simplicity
- [**OpenCode AI**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/opencode.ai/) - AI coding platform. Developer-centric dark theme
- [**Replicate**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/replicate/) - Run ML models via API. Clean white canvas, code-forward
- [**RunwayML**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/runwayml/) - AI video generation. Cinematic dark UI, media-rich layout
- [**Together AI**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/together.ai/) - Open-source AI infrastructure. Technical, blueprint-style design
- [**VoltAgent**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/voltagent/) - AI agent framework. Void-black canvas, emerald accent, terminal-native
- [**xAI**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/x.ai/) - Elon Musk's AI lab. Stark monochrome, futuristic minimalism

### Developer Tools & Platforms

- [**Cursor**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/cursor/) - AI-first code editor. Sleek dark interface, gradient accents
- [**Expo**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/expo/) - React Native platform. Dark theme, tight letter-spacing, code-centric
- [**Linear**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/linear.app/) - Project management for engineers. Ultra-minimal, precise, purple accent
- [**Lovable**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/lovable/) - AI full-stack builder. Playful gradients, friendly dev aesthetic
- [**Mintlify**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/mintlify/) - Documentation platform. Clean, green-accented, reading-optimized
- [**PostHog**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/posthog/) - Product analytics. Playful hedgehog branding, developer-friendly dark UI
- [**Raycast**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/raycast/) - Productivity launcher. Sleek dark chrome, vibrant gradient accents
- [**Resend**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/resend/) - Email API for developers. Minimal dark theme, monospace accents
- [**Sentry**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/sentry/) - Error monitoring. Dark dashboard, data-dense, pink-purple accent
- [**Supabase**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/supabase/) - Open-source Firebase alternative. Dark emerald theme, code-first
- [**Superhuman**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/superhuman/) - Fast email client. Premium dark UI, keyboard-first, purple glow
- [**Vercel**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/vercel/) - Frontend deployment platform. Black and white precision, Geist font
- [**Warp**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/warp/) - Modern terminal. Dark IDE-like interface, block-based command UI
- [**Zapier**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/zapier/) - Automation platform. Warm orange, friendly illustration-driven

### Infrastructure & Cloud

- [**ClickHouse**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/clickhouse/) - Fast analytics database. Yellow-accented, technical documentation style
- [**Composio**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/composio/) - Tool integration platform. Modern dark with colorful integration icons
- [**HashiCorp**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/hashicorp/) - Infrastructure automation. Enterprise-clean, black and white
- [**MongoDB**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/mongodb/) - Document database. Green leaf branding, developer documentation focus
- [**Sanity**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/sanity/) - Headless CMS. Red accent, content-first editorial layout
- [**Stripe**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/stripe/) - Payment infrastructure. Signature purple gradients, weight-300 elegance

### Design & Productivity

- [**Airtable**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/airtable/) - Spreadsheet-database hybrid. Colorful, friendly, structured data aesthetic
- [**Cal.com**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/cal/) - Open-source scheduling. Clean neutral UI, developer-oriented simplicity
- [**Clay**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/clay/) - Creative agency. Organic shapes, soft gradients, art-directed layout
- [**Figma**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/figma/) - Collaborative design tool. Vibrant multi-color, playful yet professional
- [**Framer**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/framer/) - Website builder. Bold black and blue, motion-first, design-forward
- [**Intercom**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/intercom/) - Customer messaging. Friendly blue palette, conversational UI patterns
- [**Miro**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/miro/) - Visual collaboration. Bright yellow accent, infinite canvas aesthetic
- [**Notion**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/notion/) - All-in-one workspace. Warm minimalism, serif headings, soft surfaces
- [**Pinterest**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/pinterest/) - Visual discovery platform. Red accent, masonry grid, image-first
- [**Webflow**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/webflow/) - Visual web builder. Blue-accented, polished marketing site aesthetic

### Fintech & Crypto

- [**Coinbase**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/coinbase/) - Crypto exchange. Clean blue identity, trust-focused, institutional feel
- [**Kraken**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/kraken/) - Crypto trading platform. Purple-accented dark UI, data-dense dashboards
- [**Revolut**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/revolut/) - Digital banking. Sleek dark interface, gradient cards, fintech precision
- [**Wise**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/wise/) - International money transfer. Bright green accent, friendly and clear

### Enterprise & Consumer

- [**Airbnb**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/airbnb/) - Travel marketplace. Warm coral accent, photography-driven, rounded UI
- [**Apple**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/apple/) - Consumer electronics. Premium white space, SF Pro, cinematic imagery
- [**IBM**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/ibm/) - Enterprise technology. Carbon design system, structured blue palette
- [**NVIDIA**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/nvidia/) - GPU computing. Green-black energy, technical power aesthetic
- [**SpaceX**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/spacex/) - Space technology. Stark black and white, full-bleed imagery, futuristic
- [**Spotify**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/spotify/) - Music streaming. Vibrant green on dark, bold type, album-art-driven
- [**Uber**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/uber/) - Mobility platform. Bold black and white, tight type, urban energy

### Car Brands

- [**BMW**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/bmw/) - Luxury automotive. Dark premium surfaces, precise German engineering aesthetic
- [**Ferrari**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/ferrari/) - Luxury automotive. Chiaroscuro black-white editorial, Ferrari Red with extreme sparseness
- [**Lamborghini**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/lamborghini/) - Luxury automotive. True black cathedral, gold accent, LamboType custom Neo-Grotesk
- [**Renault**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/renault/) - French automotive. Vivid aurora gradients, NouvelR proprietary typeface, zero-radius buttons
- [**Tesla**](https://github.com/VoltAgent/awesome-design-md/tree/main/design-md/tesla/) - Electric vehicles. Radical subtraction, cinematic full-viewport photography, Universal Sans



## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

- **Improve existing files**: Fix wrong colors, missing tokens, weak descriptions
- **Report issues**: Let us know if something looks off

Before opening a PR, please [open an issue](https://github.com/VoltAgent/awesome-design-md/issues) first to discuss your idea and get feedback from maintainers.


## License

MIT License - see [LICENSE](LICENSE)

This repository is a curated collection of design system documents extracted from public websites. All DESIGN.md files are provided "as is" without warranty. The extracted design tokens represent publicly visible CSS values. We do not claim ownership of any site's visual identity. These documents exist to help AI agents generate consistent UI.
