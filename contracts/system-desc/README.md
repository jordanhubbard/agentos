# system-desc — C-Struct System Description Format

**Version:** 1  
**Header:** `kernel/agentos-root-task/include/system_desc.h`  
**Replaces:** Microkit `.system` XML files

## Purpose

`system_desc_t` is a statically-initialized C struct that describes the complete protection domain topology for one agentOS boot image. The build tool (`xtask gen-image`) emits it as a `.rodata` section embedded in the root task ELF. The root task reads it at startup to allocate all seL4 kernel objects and configure IPC endpoints.

## How system_desc_t Maps to seL4 Boot

At boot the root task iterates the three arrays in order:

1. **`mrs[]` (mr_desc_t)** — for each entry, the root task reuses or carves an untyped memory region matching `paddr`/`size`/`page_size_bits`. Page-alignment is enforced: `paddr` must be aligned to `1 << page_size_bits` and `size` must be a non-zero multiple of 4096. A `paddr` of zero asks the root task to choose a physical address from the untyped pool.

2. **`pds[]` (pd_desc_t)** — for each protection domain, the root task allocates a TCB, a VSpace (page directory hierarchy), and a CNode of depth `cnode_size_bits`. It loads the ELF image at `elf_path`, maps each `mr_maps[]` entry into the PD's VSpace at the specified `vaddr` with the given permission subset, and mints one badged endpoint cap per `init_eps[]` entry into the designated `cnode_slot`.

3. **`endpoints[]` (endpoint_desc_t)** — for each entry, the root task creates one seL4 Endpoint object and registers it with the nameserver under `service_name`, associating it with `provider_pd_index` and `service_id` as the badge base.

## pd_desc_t and TCB/CNode/VSpace Allocation

Each `pd_desc_t` drives three kernel object allocations: a TCB (scheduling priority from `priority`, affinity from `affinity`), a CNode (`2^cnode_size_bits` slots), and a multi-level VSpace. Passive PDs (`passive == 1`) receive no scheduling context; they are driven entirely by protected procedure calls (PPC). Stack size (`stack_size`) must be a power of two and at least 4096 bytes.

## mr_desc_t Constraint Enforcement

The build tool validates before image generation:
- `paddr != 0` implies `paddr % (1u << page_size_bits) == 0`
- `size % 4096 == 0` and `size > 0`
- `page_size_bits` is one of {12, 21, 30}
- `perms` is a subset of `MR_PERM_RWX`

Any `pd_mr_map_t` referencing this region must have `perms` that are a subset of the parent `mr_desc_t.perms`.

## Version Policy

Increment `SYSTEM_DESC_VERSION` whenever the wire layout of any struct changes. The root task halts early boot with a diagnostic if the embedded version does not match the compiled-in constant.
