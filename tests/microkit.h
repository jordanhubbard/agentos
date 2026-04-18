/* Minimal microkit.h stub for host-side contract tests. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo;

static inline microkit_msginfo microkit_msginfo_new(uint64_t label, uint32_t count) {
    (void)count; return label;
}
static inline uint64_t microkit_msginfo_get_label(microkit_msginfo i) { return i; }

static uint64_t _stub_mrs[64];
static inline void     microkit_mr_set(uint32_t i, uint64_t v) { _stub_mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)             { return _stub_mrs[i]; }

static inline microkit_msginfo microkit_ppcall(microkit_channel ch, microkit_msginfo m) {
    (void)ch; return m;
}
static inline void microkit_notify(microkit_channel ch) { (void)ch; }
static inline void microkit_dbg_puts(const char *s) { (void)s; }
