#include "platform/x86_config.h"

static uint32_t load(const uint8_t *p, unsigned n)
{
    uint32_t v = 0; for (unsigned i = 0; i < n; i++) v |= (uint32_t)p[i] << (8u*i);
    return v;
}
static void put(uint8_t *p, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8u*i));
}
bool aos_x86_config_init(aos_x86_config_t *s, uint32_t ram_bytes)
{
    if (!s || ram_bytes < 32u*1024u*1024u || ram_bytes > 0x80000000u ||
        (ram_bytes & 0xffffu)) return false;
    *s = (aos_x86_config_t){.ram_bytes = ram_bytes};
    if (!aos_x86_rtc_init(&s->rtc,AOS_X86_RTC_BOOT_EPOCH,0)) return false;
    put(s->host, 0x12378086u, 4); /* virtual i440FX host bridge */
    s->host[8] = 2; s->host[11] = 6;
    put(s->pm, 0x71138086u, 4); /* virtual PIIX4 PM, bus 0 slot 1 function 3 */
    s->pm[8] = 3; s->pm[11] = 6; s->pm[10] = 0x80;
    s->pm[0x40] = 1; /* PM base I/O indicator, decode initially disabled */
    return true;
}

static uint8_t fw_byte(const aos_x86_config_t *s, uint32_t off)
{
    if (s->fw_selector == 0u) {
        static const uint8_t sig[] = {'Q','E','M','U'};
        return off < sizeof(sig) ? sig[off] : 0;
    }
    if (s->fw_selector == 1u) return off == 0u ? 1u : 0u; /* traditional PIO, no DMA */
    if (s->fw_selector == 3u)
        return off < 4u ? (uint8_t)(s->ram_bytes >> (8u*off)) : 0;
    if (s->fw_selector == 5u || s->fw_selector == 0xfu) return off == 0u ? 1u : 0u;
    if (s->fw_selector == 0x19u) {
        /* One directory entry, big-endian size/select; all reserved/name tail zero. */
        static const char name[] = "etc/e820";
        if (off == 3u) return 1;
        if (off == 7u) return 80;
        if (off == 9u) return 0x20;
        if (off >= 12u && off < 12u + sizeof(name)) return (uint8_t)name[off-12u];
        return 0;
    }
    if (s->fw_selector == 0x20u && off < 80u) {
        const uint64_t starts[] = {0, 0xa0000u, 0x100000u, 0xffc00000u};
        const uint64_t sizes[] = {0xa0000u, 0x60000u, s->ram_bytes-0x100000u, 0x400000u};
        unsigned row = off / 20u, col = off % 20u;
        if (col < 8u) return (uint8_t)(starts[row] >> (8u*col));
        if (col < 16u) return (uint8_t)(sizes[row] >> (8u*(col-8u)));
        return col == 16u ? (row == 0u || row == 2u ? 1u : 2u) : 0u;
    }
    return 0;
}

static bool pm_io(aos_x86_config_t *s, unsigned off, unsigned width,
                  bool write, uint32_t *value, uint64_t ticks)
{
    bool timer=off==8u && width==4u && !write;
    bool word=off<6u && (width==1u || (width==2u && !(off&1u)));
    if ((!timer && !word) || ticks<s->pm_last_ticks) return false;
    uint16_t status=s->pm_status, control=s->pm_control;
    /* TMR_STS latches whenever bit 23 changes, including skipped wraps. */
    if ((ticks >> 23)!=(s->pm_last_ticks >> 23)) status|=1u;
    uint32_t result=*value;
    unsigned shift=(off&1u)*8u, mask=width==1u ? 0xffu : 0xffffu;
    unsigned data=(*value&mask)<<shift;
    if (timer) result=(uint32_t)ticks & 0xffffffu;
    else if ((off&~1u)==0u) {
        if (write) status&=~data; /* W1C; reserved status bits stay zero */
        else result=(status >> shift)&mask;
    } else if ((off&~1u)==2u) {
        if (write && data) return false; /* SCI routing not implemented */
        if (!write) result=0;
    } else {
        if (write) {
            unsigned next=(control & ~(mask<<shift)) | data;
            /* SCI_EN/BM_RLD and sleep type are state, not a sleep request.
             * SLP_EN, GBL_RLS and reserved control bits fail without mutation. */
            if (next & ~0x1c03u) return false;
            control=(uint16_t)next;
        } else result=(control >> shift)&mask;
    }
    s->pm_status=status; s->pm_control=control; s->pm_last_ticks=ticks;
    if (timer) s->timer_reads++;
    *value=result;
    return true;
}

bool aos_x86_config_io(aos_x86_config_t *s, uint16_t port, unsigned width,
                       bool write, uint32_t *value, uint64_t timer_ticks)
{
    if (!s || !value || (width != 1u && width != 2u && width != 4u)) return false;
    /* Fixed one-vCPU topology discovery. No insertion/removal events or
     * hotplug commands are supported. Mirrors the fw_cfg boot CPU count. */
    if (port == 0xaf00u && width == 4u) {
        if (write) s->cpu_selector=*value;
        else *value=0; /* command data high: APIC ID zero, no pending events */
        return true;
    }
    if (port == 0xaf05u && width == 1u && write &&
        ((*value & 0xffu) == 0u || (*value & 0xffu) == 3u)) {
        s->cpu_command=(uint8_t)*value; return true;
    }
    if (port == 0xaf04u && width == 1u && !write) {
        *value=s->cpu_selector == 0u ? 1u : 0u; return true;
    }
    if (port == 0xaf08u && width == 4u && !write) {
        *value=0; /* selected CPU/APIC ID zero; invalid selectors also read zero */
        return true;
    }
    /* A20 is enabled by the guest's address model; reset/disable unsupported. */
    if (port == 0x92u && width == 1u) {
        if (write) return (*value & 0xffu) == 2u;
        *value=2; return true;
    }
    /* Bootstrap has no interrupt sources yet. Keep both legacy PICs fully
     * masked; reject unmasking until routing and injection are implemented. */
    if ((port == 0x21u || port == 0xa1u) && width == 1u) {
        if (write) return (*value & 0xffu) == 0xffu;
        *value = 0xffu;
        return true;
    }
    if (port == 0xcf8u && width == 4u) {
        if (write) s->pci_address = *value & 0x80fffffcu;
        else *value = s->pci_address;
        return true;
    }
    if (port >= 0xcfcu && port <= 0xcffu && (port & (width-1u)) == 0u &&
        (unsigned)(port - 0xcfcu) + width <= 4u) {
        uint8_t *cfg = 0;
        uint32_t bdf = s->pci_address & 0x00ffff00u;
        if (s->pci_address & 0x80000000u) {
            if (bdf == 0u) cfg = s->host;
            if (bdf == 0xb00u) cfg = s->pm;
        }
        unsigned off = (s->pci_address & 0xfcu) + port - 0xcfcu;
        if (!write) {
            *value = cfg ? load(cfg+off, width) : width == 4u ? 0xffffffffu : (1u << (width*8u))-1u;
            s->pci_reads++;
        } else if (cfg) {
            for (unsigned i = 0; i < width; i++) {
                unsigned reg = off+i;
                uint8_t mask = reg == 4u ? 7u : 0u;
                if (cfg == s->pm && reg == 0x40u) mask = 0xc0u;
                if (cfg == s->pm && reg == 0x41u) mask = 0xffu;
                if (cfg == s->pm && reg == 0x80u) mask = 1u;
                cfg[reg] = (cfg[reg] & ~mask) | ((uint8_t)(*value >> (8u*i)) & mask);
            }
        }
        return true;
    }
    uint16_t pm_base = (uint16_t)load(s->pm+0x40u, 2) & 0xffc0u;
    /* PIIX4's legacy PM decode is controlled by PMIOSE, independently of
     * the ordinary PCI command I/O-enable bit. */
    if (pm_base && (s->pm[0x80] & 1u) &&
        port>=pm_base && (uint32_t)port-(uint32_t)pm_base<12u)
        return pm_io(s,(unsigned)(port-pm_base),width,write,value,timer_ticks);
    if (port == 0x510u && width == 2u && write) {
        s->fw_selector = (uint16_t)*value; s->fw_offset = 0; return true;
    }
    if (port == 0x511u && width == 1u && !write) {
        *value = fw_byte(s, s->fw_offset);
        if (s->fw_offset < 80u) s->fw_offset++;
        s->fw_reads++;
        return true;
    }
    if (port == 0x70u && width == 1u) {
        /* MC146818 index port is write-only; QEMU's board reads all ones. */
        if (write) s->cmos_index = (uint8_t)*value & 0x7fu;
        else *value=0xffu;
        return true;
    }
    if (port == 0x71u && width == 1u && write && s->cmos_index == 0x0fu)
        return (*value & 0xffu) == 0; /* acknowledge cold boot; no S3 resume */
    if (port == 0x71u && width == 1u && (s->cmos_index<=0x0du || s->cmos_index==0x32u))
        return aos_x86_rtc_io(&s->rtc,s->cmos_index,write,value,timer_ticks);
    if (port == 0x71u && width == 1u && !write) {
        uint32_t above16 = (s->ram_bytes - 0x1000000u) >> 16;
        if (s->cmos_index == 0x0fu) *value = 0; /* cold boot; no S3 resume state */
        else if (s->cmos_index == 0x34u) *value = above16 & 0xffu;
        else if (s->cmos_index == 0x35u) *value = above16 >> 8;
        else if (s->cmos_index >= 0x5bu && s->cmos_index <= 0x5du) *value = 0;
        else return false; /* no pretend RTC clock */
        return true;
    }
    return false;
}
