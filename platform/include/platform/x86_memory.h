#ifndef AOS_X86_MEMORY_H
#define AOS_X86_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *ram, *rom;
    uint64_t ram_size, rom_base, rom_size;
} aos_x86_memory_t;

typedef struct {
    uint64_t physical, entries[4];
    unsigned levels;
} aos_x86_walk_t;
/* Return validated RAM offsets of the page-table entries as well as the GPA.
 * Emulators use these offsets to commit architectural accessed/dirty bits. */
bool aos_x86_walk(const aos_x86_memory_t *m, uint64_t cr3, uint64_t va,
                  bool write, bool execute, aos_x86_walk_t *walk);

/* Long-mode, four-level paging with the advertised 36-bit physical width.
 * Translation never dereferences the resulting device GPA. Page tables must
 * reside in this guest's RAM. Outputs stay untouched on failure. */
bool aos_x86_translate(const aos_x86_memory_t *m, uint64_t cr3, uint64_t va,
                       bool write, bool execute, uint64_t *gpa);
bool aos_x86_fetch(const aos_x86_memory_t *m, uint64_t cr3, uint64_t va,
                   uint8_t *bytes, size_t size);

typedef struct {
    uint64_t address;
    uint32_t value;
    unsigned length, reg;
    bool write;
} aos_x86_mov_t;
/* Decode 32-bit MOV r/m forms only in a 64-bit code segment. Registers use
 * Intel encoding order, including RSP at index 4. No instruction execution. */
bool aos_x86_decode_mov32(const uint8_t *bytes, size_t size, uint64_t rip,
                          const uint64_t regs[16], aos_x86_mov_t *op);
#endif
