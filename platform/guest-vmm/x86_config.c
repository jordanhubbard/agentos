#include "platform/x86_config.h"
#include <string.h>

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

bool aos_x86_config_boot(aos_x86_config_t *s, const aos_x86_boot_blobs_t *b)
{
    if (!s || !b || !s->ram_bytes || s->fw_reads || s->boot.kernel ||
        !b->kernel || !b->kernel_size || b->kernel_size>AOS_X86_BOOT_BLOB_LIMIT ||
        b->initrd_size>AOS_X86_BOOT_BLOB_LIMIT ||
        b->cmdline_size>AOS_X86_BOOT_CMDLINE_LIMIT ||
        (!!b->initrd != !!b->initrd_size) || (!!b->cmdline != !!b->cmdline_size))
        return false;
    if (b->cmdline_size) {
        if (b->cmdline[b->cmdline_size-1]) return false;
        for (uint32_t i=0; i+1<b->cmdline_size; i++)
            if (b->cmdline[i]<0x20u || b->cmdline[i]>0x7eu) return false;
    }
    s->boot=*b;
    return true;
}

bool aos_x86_config_acpi(aos_x86_config_t *s, const aos_x86_acpi_bundle_t *acpi)
{
    if (!s || !s->ram_bytes || !acpi || s->acpi || s->fw_reads ||
        !acpi->cpu_count || acpi->cpu_count>AOS_X86_ACPI_MAX_CPUS ||
        acpi->table_bytes!=846u+75u*(AOS_X86_ACPI_VIRTIO_DEVICES-3u)+
            37u*(acpi->cpu_count-1u)) return false;
    s->acpi=acpi;
    /* ACPI-only mode: no SMI handler or legacy-to-ACPI transition. */
    s->pm_control=1;
    put(s->pm+0x40,0xb001,2); s->pm[0x80]=1;
    return true;
}

static uint8_t fw_byte(const aos_x86_config_t *s, uint32_t off)
{
    const uint8_t *data=0;
    uint32_t size=0;
    switch (s->fw_selector) {
    case 8u: size=s->boot.kernel_size; break;
    case 0xbu: size=s->boot.initrd_size; break;
    case 0x14u: size=s->boot.cmdline_size; break;
    case 0x11u: data=s->boot.kernel; size=s->boot.kernel_size; break;
    case 0x12u: data=s->boot.initrd; size=s->boot.initrd_size; break;
    case 0x15u: data=s->boot.cmdline; size=s->boot.cmdline_size; break;
    case 0x21u: if (s->acpi) { data=s->acpi->tables; size=s->acpi->table_bytes; } break;
    case 0x22u: if (s->acpi) { data=s->acpi->rsdp; size=sizeof(s->acpi->rsdp); } break;
    case 0x23u: if (s->acpi) { data=s->acpi->loader; size=sizeof(s->acpi->loader); } break;
    default: break;
    }
    if (data) return off<size ? data[off] : 0;
    if (size) return off<4u ? (uint8_t)(size >> (8u*off)) : 0;
    if (s->fw_selector == 0u) {
        static const uint8_t sig[] = {'Q','E','M','U'};
        return off < sizeof(sig) ? sig[off] : 0;
    }
    if (s->fw_selector == 1u) return off == 0u ? 3u : 0u; /* PIO and DMA */
    if (s->fw_selector == 3u)
        return off < 4u ? (uint8_t)(s->ram_bytes >> (8u*off)) : 0;
    if (s->fw_selector == 5u || s->fw_selector == 0xfu)
        return off == 0u ? (s->acpi ? s->acpi->cpu_count : 1u) : 0u;
    if (s->fw_selector == 0x19u) {
        /* Directory fields are big-endian, unlike the table contents. */
        static const char names[4][56]={"etc/e820","etc/acpi/tables",
                                       "etc/acpi/rsdp","etc/table-loader"};
        const uint32_t sizes[]={80,s->acpi ? s->acpi->table_bytes : 0u,
                               36,AOS_X86_ACPI_LOADER_BYTES};
        unsigned count=s->acpi ? 4u : 1u;
        if (off<4u) return off==3u ? (uint8_t)count : 0;
        unsigned entry=(off-4u)/64u, col=(off-4u)%64u;
        if (entry>=count) return 0;
        if (col<4u) return (uint8_t)(sizes[entry] >> (8u*(3u-col)));
        if (col==5u) return (uint8_t)(0x20u+entry);
        if (col>=8u) return (uint8_t)names[entry][col-8u];
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

bool aos_x86_config_dma(aos_x86_config_t *s, uint32_t control,
                        uint8_t *destination, uint32_t length)
{
    enum { DMA_READ=2u, DMA_SKIP=4u, DMA_SELECT=8u, DMA_WRITE=16u };
    if (!s || !length || length>AOS_X86_BOOT_BLOB_LIMIT ||
        control & ~(UINT32_C(0xffff0000)|DMA_READ|DMA_SKIP|DMA_SELECT|DMA_WRITE) ||
        (!!(control&DMA_READ)+!!(control&DMA_SKIP)+!!(control&DMA_WRITE))!=1u ||
        ((control&DMA_READ) && !destination) ||
        (!(control&DMA_READ) && destination)) return false;
    aos_x86_config_t next=*s;
    if (control&DMA_SELECT) {
        next.fw_selector=(uint16_t)(control>>16);
        next.fw_offset=0;
    }
    if (length>UINT32_MAX-next.fw_offset || (control&DMA_WRITE)) return false;
    if (control&DMA_READ) {
        unsigned blob=3;
        uint32_t size=0;
        const uint8_t *source=0;
        if (next.fw_selector==0x11u) { blob=0; source=next.boot.kernel; size=next.boot.kernel_size; }
        if (next.fw_selector==0x12u) { blob=1; source=next.boot.initrd; size=next.boot.initrd_size; }
        if (next.fw_selector==0x15u) { blob=2; source=next.boot.cmdline; size=next.boot.cmdline_size; }
        if (source) {
            uint32_t available=next.fw_offset<size ? size-next.fw_offset : 0u;
            if (available>length) available=length;
            if (available) memcpy(destination,source+next.fw_offset,available);
            if (available<length) memset(destination+available,0,length-available);
        } else {
            for (uint32_t i=0;i<length;i++)
                destination[i]=fw_byte(&next,next.fw_offset+i);
        }
        if (blob<3 && next.fw_offset<size) {
            uint32_t consumed=size-next.fw_offset;
            if (consumed>length) consumed=length;
            uint32_t room=UINT32_MAX-next.boot_reads[blob];
            next.boot_reads[blob]+=consumed>room ? room : consumed;
        }
        uint32_t room=UINT32_MAX-next.fw_reads;
        next.fw_reads+=length>room ? room : length;
    }
    next.fw_offset+=length;
    *s=next;
    return true;
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
        /* No firmware global-lock hardware or RTC wake source exists. Their
         * enable bits do not stick, matching discovery and FADT FIX_RTC.
         * Other nonzero enables still require real interrupt sources. */
        if (write && (data & ~0x420u)) return false;
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
    /* PIIX reset control, independent of the overlapping PCI address port.
     * Only byte accesses decode here. RCPU requests a whole guest reset;
     * SRST is retained for readback, matching the board's register model. */
    if (port == 0xcf9u && width == 1u) {
        if (write) {
            s->reset_control = (uint8_t)*value & 2u;
            if (*value & 4u) s->reset_requested = true;
        } else *value = s->reset_control;
        return true;
    }
    /* Legacy I/O-delay writes have no device state. All emulated register
     * operations complete synchronously before the guest resumes. */
    if (write && width==1u && (port==0x80u || port==0xedu)) return true;
    /* Removed legacy DMA page registers read all ones. Linux probes 0x87
     * before registering 8237A support. No DMA programming path is exposed. */
    if (!write && width==1u && ((port>=0x81u && port<=0x83u) || port==0x87u ||
        (port>=0x89u && port<=0x8bu) || port==0x8fu)) {
        *value=0xffu; return true;
    }
    /* No PS/2 controller is provisioned. Undecoded byte I/O reads all ones;
     * writes have no effect. Linux's bounded i8042 flush detects absence.
     * The firmware's reset pulse is a platform reset request, without
     * creating keyboard data or an interrupt source. */
    if (width==1u && (port==0x60u || port==0x64u)) {
        if (write && port==0x64u && (*value & 0xffu)==0xfeu)
            s->reset_requested = true;
        if (!write) *value=0xffu;
        return true;
    }
    /* No ISA UART is provisioned. Legacy 8250 probing must see an absent
     * device, not a writable interrupt-enable register or a host UART. */
    if (width==1u && ((port>=0x3f8u && port<=0x3ffu) ||
        (port>=0x2f8u && port<=0x2ffu) || (port>=0x3e8u && port<=0x3efu) ||
        (port>=0x2e8u && port<=0x2efu))) {
        if (!write) *value=0xffu;
        return true;
    }
    /* No PIT clock or IRQ0 source is advertised. Linux still writes its
     * channel-0 shutdown sequence after selecting the LAPIC clockevent.
     * Accept only mode-0 reset and its two zero count bytes; do not pretend
     * to supply PIT calibration, periodic interrupts or a running counter. */
    if (write && width==1u && port==0x43u && *value==0x30u) {
        s->pit_disable_remaining=2; return true;
    }
    if (write && width==1u && port==0x40u && !*value && s->pit_disable_remaining) {
        s->pit_disable_remaining--; return true;
    }
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
    /* No legacy PIC or ISA interrupt sources exist in this machine.
     * Absent command/mask ports read all ones and discard byte writes.
     * In particular, a mask probe must not echo a writable PIC register. */
    if ((port == 0x20u || port == 0x21u || port == 0xa0u || port == 0xa1u) &&
        width == 1u) {
        if (write) return true;
        *value = 0xffu;
        return true;
    }
    /* The fixed mechanism-1 address register only accepts DWORD writes.
     * Aligned byte/word writes are ignored, including Linux's CFB probe;
     * they neither select mechanism 2 nor alter the current PCI address. */
    if (write && port>=0xcf8u && port<0xcfcu && width<4u &&
        !(port & (width-1u)) && width<=0xcfcu-port) return true;
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
        unsigned blob=3;
        uint32_t size=0;
        if (s->fw_selector==0x11u) { blob=0; size=s->boot.kernel_size; }
        if (s->fw_selector==0x12u) { blob=1; size=s->boot.initrd_size; }
        if (s->fw_selector==0x15u) { blob=2; size=s->boot.cmdline_size; }
        if (blob<3 && s->fw_offset<size && s->boot_reads[blob]<UINT32_MAX)
            s->boot_reads[blob]++;
        if (s->fw_offset < UINT32_MAX) s->fw_offset++;
        if (s->fw_reads<UINT32_MAX) s->fw_reads++;
        return true;
    }
    if (port == 0x70u && width == 1u) {
        /* MC146818 index port is write-only; QEMU's board reads all ones. */
        if (write) s->cmos_index = (uint8_t)*value & 0x7fu;
        else *value=0xffu;
        return true;
    }
    if (port == 0x71u && width == 1u && s->cmos_index == 0x0fu) {
        /* Linux brackets AP startup with warm-reset marker 0x0a and zero.
         * This is guest-private scratch state. CPU execution starts only via
         * INIT/SIPI; no host reset, persistent CMOS or sleep state is invoked. */
        if (write) {
            uint8_t status=(uint8_t)*value;
            if (status!=0u && status!=0x0au) return false;
            s->cmos_shutdown=status;
        } else *value=s->cmos_shutdown;
        return true;
    }
    if (port == 0x71u && width == 1u && (s->cmos_index<=0x0du || s->cmos_index==0x32u))
        return aos_x86_rtc_io(&s->rtc,s->cmos_index,write,value,timer_ticks);
    if (port == 0x71u && width == 1u && !write) {
        uint32_t above16 = (s->ram_bytes - 0x1000000u) >> 16;
        if (s->cmos_index == 0x34u) *value = above16 & 0xffu;
        else if (s->cmos_index == 0x35u) *value = above16 >> 8;
        else if (s->cmos_index >= 0x5bu && s->cmos_index <= 0x5du) *value = 0;
        else return false; /* no pretend RTC clock */
        return true;
    }
    return false;
}
