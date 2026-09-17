#include "platform/x86_memory.h"

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint64_t)p[i] << (8u*i);
    return v;
}
static bool canonical(uint64_t va)
{ return (va >> 47) == 0 || (va >> 47) == 0x1ffffu; }

bool aos_x86_walk(const aos_x86_memory_t *m, uint64_t cr3, uint64_t va,
                  bool write, bool execute, aos_x86_walk_t *walk)
{
    if (!m || !m->ram || !walk || !canonical(va) ||
        (cr3 & ~UINT64_C(0xffffff018))) return false; /* PCID unsupported */
    uint64_t table = cr3 & UINT64_C(0xffffff000);
    aos_x86_walk_t result = {0};
    for (unsigned level = 4; level; level--) {
        unsigned shift = 12 + 9*(level-1);
        uint64_t at = table + ((va >> shift) & 511u)*8;
        if (at > m->ram_size || m->ram_size-at < 8) return false;
        uint64_t e = le(m->ram + at, 8);
        result.entries[result.levels++]=at;
        /* Bits 51:36 are reserved for this CPU's physical width. */
        if (!(e & 1) || (e & UINT64_C(0x000ffff000000000)) ||
            (write && !(e & 2)) || (execute && (e >> 63))) return false;
        bool large = (e & 128) && level > 1;
        if (large && level != 2) return false; /* 1 GiB pages not advertised */
        if (level == 1 || large) {
            uint64_t mask = (UINT64_C(1) << shift)-1;
            if (large && (e & (mask & ~UINT64_C(0x1fff)))) return false;
            result.physical = (e & UINT64_C(0xffffff000) & ~mask) | (va & mask);
            *walk=result;
            return true;
        }
        table = e & UINT64_C(0xffffff000);
    }
    return false;
}

bool aos_x86_translate(const aos_x86_memory_t *m, uint64_t cr3, uint64_t va,
                       bool write, bool execute, uint64_t *gpa)
{
    aos_x86_walk_t walk;
    if (!gpa || !aos_x86_walk(m,cr3,va,write,execute,&walk)) return false;
    *gpa=walk.physical;
    return true;
}

bool aos_x86_fetch(const aos_x86_memory_t *m, uint64_t cr3, uint64_t va,
                   uint8_t *bytes, size_t size)
{
    if (!bytes || !size || size > 15 || va > UINT64_MAX-(size-1)) return false;
    uint8_t copy[15];
    for (size_t i = 0; i < size; i++) {
        uint64_t pa;
        if (!aos_x86_translate(m, cr3, va+i, false, true, &pa)) return false;
        if (pa < m->ram_size) copy[i] = m->ram[pa];
        else if (m->rom && pa >= m->rom_base && pa-m->rom_base < m->rom_size)
            copy[i] = m->rom[pa-m->rom_base];
        else return false;
    }
    for (size_t i = 0; i < size; i++) bytes[i] = copy[i];
    return true;
}

bool aos_x86_decode_mov32(const uint8_t *b, size_t n, uint64_t rip,
                          const uint64_t r[16], aos_x86_mov_t *op)
{
    if (!b || !r || !op || !n || n > 15) return false;
    unsigned i = 0, rex = 0;
    if ((b[i] & 0xf0) == 0x40) { rex = b[i++]; if ((rex & 8) || i == n) return false; }
    unsigned opcode = b[i++];
    if ((opcode != 0x89 && opcode != 0x8b && opcode != 0xc7) || i == n) return false;
    unsigned modrm = b[i++], mod = modrm >> 6, rm = modrm & 7;
    unsigned reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    if (mod == 3 || (opcode == 0xc7 && reg)) return false;
    bool relative = false, absent = false;
    uint64_t address = 0;
    if (rm == 4) {
        if (i == n) return false;
        unsigned sib = b[i++], index = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
        unsigned base = (sib & 7) | ((rex & 1) ? 8 : 0);
        absent = mod == 0 && (sib & 7) == 5;
        if (!absent) address = r[base];
        if (index != 4) address += r[index] << (sib >> 6);
    } else if (mod == 0 && rm == 5) { relative = true; absent = true; }
    else address = r[rm | ((rex & 1) ? 8 : 0)];
    unsigned displacement = mod == 1 ? 1 : mod == 2 || absent ? 4 : 0;
    if (n-i < displacement) return false;
    if (displacement == 1) address += (uint64_t)(int64_t)(int8_t)b[i];
    if (displacement == 4) address += (uint64_t)(int64_t)(int32_t)le(b+i, 4);
    i += displacement;
    uint32_t value = (uint32_t)r[reg];
    if (opcode == 0xc7) {
        if (n-i < 4) return false;
        value = (uint32_t)le(b+i, 4); i += 4;
    }
    if (relative) address += rip+i;
    if (!canonical(address)) return false;
    *op = (aos_x86_mov_t){.address=address, .value=value, .length=i,
                         .reg=reg, .write=opcode != 0x8b};
    return true;
}
