#include "platform/x86_string.h"

static uint32_t swap32(uint32_t n)
{
    return ((n&0xffu)<<24)|((n&0xff00u)<<8)|((n>>8)&0xff00u)|(n>>24);
}
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p)<<32)|be32(p+4);
}
static void put_be32(uint8_t *p,uint32_t n)
{
    p[0]=(uint8_t)(n>>24); p[1]=(uint8_t)(n>>16);
    p[2]=(uint8_t)(n>>8); p[3]=(uint8_t)n;
}

bool aos_x86_fw_dma_io(const aos_x86_memory_t *memory, uint8_t *writable_ram,
                       aos_x86_config_t *config, uint16_t port, unsigned width,
                       bool write, uint32_t *value)
{
    if (!memory || !writable_ram || memory->ram!=writable_ram || !config ||
        !value || width!=4u || (port!=0x514u && port!=0x518u)) return false;
    if (!write) {
        /* Raw x86 IN values preserve the bytes of the big-endian register. */
        *value=swap32(port==0x514u ? UINT32_C(0x51454d55) : UINT32_C(0x20434647));
        return true;
    }
    uint32_t half=swap32(*value);
    if (port==0x514u) {
        config->fw_dma_address=(uint64_t)half<<32;
        return true;
    }
    uint64_t descriptor=config->fw_dma_address|half;
    config->fw_dma_address=0;
    if (descriptor>memory->ram_size || memory->ram_size-descriptor<16u)
        return true; /* no writable status word, matching absent DMA memory */
    uint8_t *access=writable_ram+descriptor;
    uint32_t control=be32(access), length=be32(access+4);
    uint64_t destination=be64(access+8);
    bool read=(control&2u)!=0;
    bool valid=length && length<=AOS_X86_BOOT_BLOB_LIMIT &&
        (!read || (destination<=memory->ram_size &&
                   length<=memory->ram_size-destination));
    aos_x86_config_t next=*config;
    if (valid)
        valid=aos_x86_config_dma(&next,control,
            read ? writable_ram+destination : NULL,length);
    if (valid) *config=next;
    put_be32(access,valid ? 0u : 1u);
    return true;
}

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
