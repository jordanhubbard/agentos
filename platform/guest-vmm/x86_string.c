#include "platform/x86_string.h"

bool aos_x86_fw_insb(const aos_x86_memory_t *memory, uint8_t *writable_ram,
                      uint64_t cr3, aos_x86_config_t *config,
                      uint64_t *address, uint64_t *count, bool backwards)
{
    if (!memory || !writable_ram || !config || !address || !count ||
        memory->ram != writable_ram) return false;
    unsigned n = *count > AOS_X86_STRING_BATCH ? AOS_X86_STRING_BATCH : (unsigned)*count;
    if (!n) return true;
    /* Reject wrapping the destination, including its post-transfer value. */
    if (backwards ? *address < n : *address > UINT64_MAX-n) return false;
    uint64_t physical[AOS_X86_STRING_BATCH];
    uint64_t entries[8];
    uint8_t flags[8];
    unsigned entry_count=0;
    for (unsigned i=0; i<n;) {
        uint64_t va = backwards ? *address-i : *address+i;
        aos_x86_walk_t walk;
        if (!aos_x86_walk(memory, cr3, va, true, false, &walk) ||
            walk.physical >= memory->ram_size) return false;
        /* A validated leaf maps a contiguous 4 KiB or 2 MiB page.  REP
         * transfers often contain megabytes, so do not repeat the identical
         * four-level walk for every byte in this bounded batch.  Keep every
         * destination and A/D update staged until all covered leaves validate;
         * the atomic rejection rule below is unchanged. */
        unsigned shift=walk.levels==3u ? 21u : 12u;
        uint64_t mask=(UINT64_C(1)<<shift)-1u;
        uint64_t offset=va&mask;
        uint64_t available=backwards ? offset+1u : (mask+1u)-offset;
        unsigned run=available>n-i ? n-i : (unsigned)available;
        if (run>memory->ram_size || walk.physical > memory->ram_size-run ||
            (backwards && walk.physical+1u<run)) return false;
        for (unsigned j=0;j<run;j++)
            physical[i+j]=backwards ? walk.physical-j : walk.physical+j;
        for (unsigned level=0; level<walk.levels; level++) {
            unsigned j=0;
            while (j<entry_count && entries[j]!=walk.entries[level]) j++;
            if (j==entry_count) {
                if (entry_count==8) return false;
                entries[j]=walk.entries[level]; flags[j]=0; entry_count++;
            }
            flags[j] |= level+1==walk.levels ? 0x60u : 0x20u;
        }
        i+=run;
    }
    /* Stage the device too: a rejected operation cannot consume its stream. */
    aos_x86_config_t next = *config;
    uint8_t bytes[AOS_X86_STRING_BATCH];
    for (unsigned i=0; i<n; i++) {
        uint32_t value=0;
        if (!aos_x86_config_io(&next, 0x511, 1, false, &value, 0)) return false;
        bytes[i]=(uint8_t)value;
    }
    /* Commit A/D before data, as a store into a page-table byte may overwrite
     * that byte. All offsets were validated while this single vCPU was stopped. */
    for (unsigned i=0; i<entry_count; i++) writable_ram[entries[i]] |= flags[i];
    for (unsigned i=0; i<n; i++) writable_ram[physical[i]]=bytes[i];
    *config=next;
    *address = backwards ? *address-n : *address+n;
    *count -= n;
    return true;
}
