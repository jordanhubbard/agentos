#pragma once
/* IRQ_PD contract — version 1
 * PD: irq_pd | Source: src/irq_pd.c | Channel: CH_IRQ_PD (66) from controller
 * Provides OS-neutral IPC IRQ routing. Registers hardware IRQs and forwards notifications.
 */
#include <stdint.h>
#include <stdbool.h>

#define IRQ_PD_CONTRACT_VERSION 1

/* ── Channel ─────────────────────────────────────────────────────────────── */
#define CH_IRQ_PD  66u

/* ── Opcodes (from agentos_msg_tag_t) ───────────────────────────────────── */
#define IRQ_OP_REGISTER    0x1050u
#define IRQ_OP_UNREGISTER  0x1051u
#define IRQ_OP_ACKNOWLEDGE 0x1052u
#define IRQ_OP_MASK        0x1053u
#define IRQ_OP_UNMASK      0x1054u
#define IRQ_OP_STATUS      0x1055u

#define IRQ_MAX_IRQS       64u

/* ── Request structs ─────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t op;            /* IRQ_OP_REGISTER */
    uint32_t irq_num;       /* hardware IRQ number */
    uint32_t notify_ch;     /* microkit channel to notify when IRQ fires */
} irq_req_register_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* IRQ_OP_UNREGISTER */
    uint32_t irq_num;
} irq_req_unregister_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* IRQ_OP_ACKNOWLEDGE */
    uint32_t irq_num;
} irq_req_acknowledge_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* IRQ_OP_MASK */
    uint32_t irq_num;
} irq_req_mask_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* IRQ_OP_UNMASK */
    uint32_t irq_num;
} irq_req_unmask_t;

typedef struct __attribute__((packed)) {
    uint32_t op;            /* IRQ_OP_STATUS */
    uint32_t irq_num;
} irq_req_status_t;

/* ── Reply structs ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t result;        /* 0 = ok */
} irq_reply_register_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t masked;        /* 1 = currently masked */
    uint32_t pending;       /* 1 = IRQ pending acknowledgement */
    uint32_t count;         /* total fires since REGISTER */
} irq_reply_status_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum {
    IRQ_OK               = 0,
    IRQ_ERR_NO_SLOT      = 1,  /* no free IRQ slots */
    IRQ_ERR_BAD_SLOT     = 2,  /* irq_num not registered */
    IRQ_ERR_BAD_IRQ      = 3,  /* irq_num >= IRQ_MAX_IRQS */
    IRQ_ERR_HW           = 4,  /* GIC / hardware error */
    IRQ_ERR_NOT_IMPL     = 5,  /* operation not yet implemented */
    IRQ_ERR_ALREADY_REG  = 6,  /* irq_num already registered by another caller */
} irq_error_t;

/* ── Invariants ──────────────────────────────────────────────────────────
 * - ACKNOWLEDGE must be called after handling each IRQ before another fire is possible.
 * - MASK suppresses notification but does not deregister the IRQ.
 * - UNREGISTER implicitly masks the IRQ first.
 * - notify_ch must be a valid microkit channel owned by the caller's PD.
 * - IRQ_MAX_IRQS is the GIC SPI range supported; SGIs (0-15) are not routable via this PD.
 */
