#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sel4/sel4.h>
#include <platform/x86_memory_rebuild.h>
#include <libvmm/virtio/gpa.h>
#include "contracts/x86_guest_memory_caps.h"
#include "contracts/x86_vtx_proof.h"

enum operation { RETYPE, COPY, MAP, UNMAP, EPT };
struct expected { enum operation op; uintptr_t a, b, c; };
static struct expected expected[128];
static unsigned count, calls, fail_at, disabled;
static const size_t frame_size = 1u << AOS_GUEST_RAM_FRAME_BITS;
static int step(enum operation op, uintptr_t a, uintptr_t b, uintptr_t c)
{
    assert(calls < count);
    struct expected e = expected[calls++];
    assert(e.op == op && e.a == a && e.b == b && e.c == c);
    return calls == fail_at ? 19 : seL4_NoError;
}
void virtio_gpa_set_translate(virtio_gpa_translate_fn fn)
{ assert(fn == NULL); disabled++; }
seL4_Error seL4_Untyped_Retype(seL4_CPtr pool, seL4_Word type, seL4_Word bits,
    seL4_CPtr root, seL4_Word node, seL4_Word depth, seL4_Word slot, seL4_Word n)
{
    assert(type == seL4_X86_LargePageObject && !bits && !node && !depth && n == 1);
    assert(root == AOS_GUEST_RAM_SELF_CNODE);
    return step(RETYPE, pool, slot, 0);
}
seL4_Error seL4_CNode_Copy(seL4_CPtr root, seL4_Word dest, uint8_t depth,
    seL4_CPtr source_root, seL4_Word source, uint8_t source_depth, seL4_CapRights_t rights)
{
    assert(root == AOS_GUEST_RAM_SELF_CNODE && source_root == root);
    assert(depth == AOS_GUEST_RAM_CNODE_BITS && source_depth == depth);
    return step(COPY, dest, source, rights);
}
seL4_Error seL4_X86_Page_Map(seL4_CPtr frame, seL4_CPtr vspace, seL4_Word va,
    seL4_CapRights_t rights, seL4_Word attr)
{
    assert(vspace == AOS_GUEST_RAM_VMM_VSPACE && attr == seL4_X86_Default_VMAttributes);
    int err = step(MAP, frame, va, rights);
    if (err) return err;
    if (rights == seL4_AllRights) {
        assert(mmap((void *)va, frame_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)va);
    } else {
        assert(rights == 1 && mprotect((void *)va, frame_size, PROT_READ) == 0);
    }
    return 0;
}
seL4_Error seL4_X86_Page_Unmap(seL4_CPtr frame)
{
    int err = step(UNMAP, frame, 0, 0);
    if (!err) assert(mprotect((void *)(AOS_X86_FIRMWARE_ROM_VA +
        (frame - AOS_X86_GUEST_ROM_FRAME_BASE) * frame_size), frame_size, PROT_NONE) == 0);
    return err;
}
seL4_Error seL4_X86_Page_MapEPT(seL4_CPtr frame, seL4_CPtr ept, seL4_Word gpa,
    seL4_CapRights_t rights, seL4_Word attr)
{
    assert(ept == AOS_GUEST_RAM_GUEST_VSPACE && attr == seL4_X86_EPT_Default_VMAttributes);
    return step(EPT, frame, gpa, rights);
}
static void add(enum operation op, uintptr_t a, uintptr_t b, uintptr_t c)
{ assert(count < 128); expected[count++] = (struct expected){op, a, b, c}; }
int main(void)
{
    uint8_t *image = malloc(AOS_X86_FIRMWARE_BYTES);
    assert(image);
    for (size_t i = 0; i < AOS_X86_FIRMWARE_BYTES; i++) image[i] = (uint8_t)(i ^ (i >> 13));
    assert(!aos_x86_guest_memory_rebuild(NULL, AOS_X86_FIRMWARE_BYTES, AOS_X86_FIRMWARE_RAM));
    assert(!aos_x86_guest_memory_rebuild(image, 0, AOS_X86_FIRMWARE_RAM));
    assert(!aos_x86_guest_memory_rebuild(image, AOS_X86_FIRMWARE_BYTES, 0));
    assert(!aos_x86_guest_memory_rebuild((void *)AOS_X86_FIRMWARE_RAM_VA,
        AOS_X86_FIRMWARE_BYTES, AOS_X86_FIRMWARE_RAM));
    assert(!aos_x86_guest_memory_rebuild((void *)AOS_X86_FIRMWARE_ROM_VA,
        AOS_X86_FIRMWARE_BYTES, AOS_X86_FIRMWARE_RAM));
    assert(!aos_x86_guest_memory_rebuild((void *)(UINTPTR_MAX - 1),
        AOS_X86_FIRMWARE_BYTES, AOS_X86_FIRMWARE_RAM));
    assert(!calls && !disabled);
    for (unsigned i = 0; i < AOS_X86_FIRMWARE_RAM / frame_size + 2u; i++) {
        bool rom = i >= AOS_X86_FIRMWARE_RAM / frame_size;
        unsigned index = rom ? i - AOS_X86_FIRMWARE_RAM / frame_size : i;
        uintptr_t frame = (rom ? AOS_X86_GUEST_ROM_FRAME_BASE : AOS_GUEST_RAM_FRAME_BASE) + index;
        uintptr_t alias = (rom ? AOS_X86_GUEST_ROM_ALIAS_BASE : AOS_GUEST_RAM_ALIAS_BASE) + index;
        uintptr_t pool = (rom ? AOS_X86_GUEST_ROM_POOL_BASE : AOS_GUEST_RAM_POOL_BASE) + index;
        uintptr_t va = (rom ? AOS_X86_FIRMWARE_ROM_VA : AOS_X86_FIRMWARE_RAM_VA) + index * frame_size;
        uintptr_t pa = (rom ? AOS_X86_FIRMWARE_BASE : 0u) + index * frame_size;
        unsigned rights = rom ? 1u : 3u;
        add(RETYPE, pool, frame, 0);
        if (rom) { add(MAP, frame, va, 3); add(UNMAP, frame, 0, 0); }
        add(COPY, alias, frame, rights);
        add(MAP, alias, va, rights);
        add(EPT, frame, pa, rights);
    }
    for (fail_at = 0; fail_at <= count; fail_at++) {
        calls = disabled = 0;
        assert(aos_x86_guest_memory_rebuild(image, AOS_X86_FIRMWARE_BYTES,
            AOS_X86_FIRMWARE_RAM) == (fail_at == 0));
        assert(calls == (fail_at ? fail_at : count) && disabled == 1);
        if (!fail_at) {
            const uint64_t *ram = (void *)AOS_X86_FIRMWARE_RAM_VA;
            for (size_t i = 0; i < AOS_X86_FIRMWARE_RAM / sizeof(*ram); i++) assert(!ram[i]);
            assert(memcmp((void *)AOS_X86_FIRMWARE_ROM_VA, image, AOS_X86_FIRMWARE_BYTES) == 0);
        }
        assert(munmap((void *)AOS_X86_FIRMWARE_RAM_VA, AOS_X86_FIRMWARE_RAM) == 0);
        assert(munmap((void *)AOS_X86_FIRMWARE_ROM_VA, AOS_X86_FIRMWARE_BYTES) == 0);
    }
    free(image);
    puts("PASS: fresh x86 RAM, exact ROM restoration, read-only mappings and every failure boundary");
}
