# agentOS Image Format — `agentos.img`

This document specifies the flat-binary image format produced by
`cargo xtask gen-image`.  The agentOS root task parses this image at boot
time to locate the kernel ELF, root task ELF, and all protection-domain ELFs.

---

## Rationale

The standard seL4 ELF-loader format (used by the Microkit SDK's `bin/microkit`
tool) is tightly coupled to the Microkit build toolchain and requires a
separately-maintained host binary.  agentOS replaces this with a
self-describing flat-binary format that:

- Has no toolchain dependency beyond `cargo xtask`.
- Is trivial to parse in C from the root task (sequential reads, no
  relocations, no ELF parsing required for the manifest itself).
- Is architecture-neutral — the same format works for aarch64, riscv64, and
  x86\_64.
- Can be extended without breaking existing parsers (version field, pad
  region).

---

## Overall Layout

```
Byte offset   Contents
──────────────────────────────────────────────────────────────────────────────
0             File header (64 bytes, fixed)
64            PD entry table  (num_pds × 64 bytes)
header.kernel_off   seL4 kernel ELF blob  (header.kernel_len bytes)
header.root_off     Root task ELF blob    (header.root_len bytes)
pd[0].elf_off       PD 0 ELF blob         (pd[0].elf_len bytes)
pd[1].elf_off       PD 1 ELF blob         (pd[1].elf_len bytes)
...
```

All multi-byte integers are **little-endian**.

---

## File Header (64 bytes)

| Offset | Size | Field          | Description                                      |
|--------|------|----------------|--------------------------------------------------|
| 0      | 8    | `magic`        | `0x4147454E544F5300` (ASCII `AGENTOS\0`)         |
| 8      | 4    | `version`      | Format version, currently `1`                    |
| 12     | 4    | `num_pds`      | Number of protection-domain entries              |
| 16     | 4    | `kernel_off`   | Byte offset of the kernel ELF in this file       |
| 20     | 4    | `kernel_len`   | Byte length of the kernel ELF                    |
| 24     | 4    | `root_off`     | Byte offset of the root task ELF in this file    |
| 28     | 4    | `root_len`     | Byte length of the root task ELF                 |
| 32     | 4    | `pd_table_off` | Byte offset of the PD entry table                |
| 36     | 28   | `_pad`         | Reserved, must be zero                           |

The magic value written as a C constant:

```c
#define AGENTOS_IMAGE_MAGIC   UINT64_C(0x4147454E544F5300)
#define AGENTOS_IMAGE_VERSION 1U
```

---

## PD Entry Table

Each entry is exactly **64 bytes**:

| Offset | Size | Field      | Description                                      |
|--------|------|------------|--------------------------------------------------|
| 0      | 48   | `name`     | Protection-domain name, NUL-terminated, padded   |
| 48     | 4    | `elf_off`  | Byte offset of this PD's ELF blob in the file    |
| 52     | 4    | `elf_len`  | Byte length of the PD ELF blob                   |
| 56     | 1    | `priority` | Scheduling priority (0 = lowest, 255 = highest)  |
| 57     | 7    | `_pad`     | Reserved, must be zero                           |

PD entries appear in the same order as the `[[pd]]` entries in the system
TOML used as input to `gen-image`.

---

## C Header Equivalent

```c
#include <stdint.h>

#define AGENTOS_IMAGE_MAGIC   UINT64_C(0x4147454E544F5300)
#define AGENTOS_IMAGE_VERSION 1U
#define AGENTOS_IMG_HDR_SIZE  64U
#define AGENTOS_PD_ENTRY_SIZE 64U

typedef struct __attribute__((packed)) {
    uint64_t magic;        /* AGENTOS_IMAGE_MAGIC            */
    uint32_t version;      /* AGENTOS_IMAGE_VERSION          */
    uint32_t num_pds;      /* number of PD entries           */
    uint32_t kernel_off;   /* byte offset of kernel ELF      */
    uint32_t kernel_len;   /* byte length of kernel ELF      */
    uint32_t root_off;     /* byte offset of root-task ELF   */
    uint32_t root_len;     /* byte length of root-task ELF   */
    uint32_t pd_table_off; /* byte offset of PD entry table  */
    uint8_t  _pad[28];
} agentos_img_hdr_t;

typedef struct __attribute__((packed)) {
    char     name[48];     /* NUL-terminated PD name         */
    uint32_t elf_off;      /* byte offset of PD ELF          */
    uint32_t elf_len;      /* byte length of PD ELF          */
    uint8_t  priority;     /* scheduling priority            */
    uint8_t  _pad[7];
} agentos_pd_entry_t;
```

---

## How the Root Task Reads the Image at Boot

The seL4 ELF-loader places the image in physical memory and passes the load
address to the root task via the `seL4_BootInfo` structure's
`extraBIPages` field or a platform-specific mechanism (exact mechanism is
being finalised in issue E1-S6: root-task boot sequence).

The root task performs the following steps:

1. Receive the physical base address of the image from the loader.
2. Cast the first 64 bytes to `agentos_img_hdr_t *` and verify:
   - `magic == AGENTOS_IMAGE_MAGIC`
   - `version == AGENTOS_IMAGE_VERSION` (or a version it supports)
3. Walk the PD entry table at `base + hdr->pd_table_off`, one
   `agentos_pd_entry_t` per PD.
4. For each PD, map `base + entry->elf_off` with length `entry->elf_len` into
   a fresh VSpace, then create a TCB and schedule it at `entry->priority`.
5. Load the kernel ELF from `base + hdr->kernel_off` if running in a
   simulation context where the kernel has not already been loaded by the
   hardware ELF-loader.

No dynamic allocation is required to parse the manifest — it is a single
linear scan.

---

## Generating an Image

```
cargo xtask gen-image \
  --arch      aarch64 \
  --system    path/to/system.toml \
  --kernel    path/to/sel4.elf \
  --root-task path/to/root_task.elf \
  --pd-dir    path/to/pd/elfs/ \
  --out       agentos.img
```

The `--pd-dir` directory must contain one file named `<pd_name>.elf` for
every `[[pd]]` entry in `system.toml`.

---

## Testing

Run the unit tests with:

```
cargo test -p xtask
```

The test suite in `xtask/src/cmd_gen_image.rs` generates images from in-memory
fake ELFs, verifies the magic value, version, and PD count, and checks the
minimum expected image size.

For a manual smoke-test using the checked-in test fixtures:

```
cargo xtask gen-image \
  --arch      aarch64 \
  --system    xtask/test-data/minimal.toml \
  --kernel    xtask/test-data/fake-kernel.elf \
  --root-task xtask/test-data/fake-root.elf \
  --pd-dir    xtask/test-data/fake-pd-dir/ \
  --out       /tmp/test.img

xxd /tmp/test.img | head -4   # first 8 bytes must be: 00 53 4f 54 4e 45 47 41
```

Note that because the magic is stored little-endian, the first 8 bytes in a
hex dump appear reversed: `00 53 4f 54 4e 45 47 41`.
