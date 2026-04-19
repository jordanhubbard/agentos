#pragma once
/* AGENTFS contract — version 1
 * PD: agentfs | Source: src/agentfs.c | Channel: CH_CONTROLLER_AGENTFS=5 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define AGENTFS_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_AGENTFS             5   /* controller -> agentfs (from controller: CH_CONTROLLER_AGENTFS) */

/* ── Opcodes (0xAF00 range) ── */
#define AGENTFS_OP_READ        0xAF01u  /* read object by hash into shared region */
#define AGENTFS_OP_WRITE       0xAF02u  /* write object from shared region; returns hash */
#define AGENTFS_OP_STAT        0xAF03u  /* stat object: size, timestamp, flags */
#define AGENTFS_OP_LIST        0xAF04u  /* list objects (paginated) into shared region */
#define AGENTFS_OP_DELETE      0xAF05u  /* tombstone an object by hash */
#define AGENTFS_OP_SEARCH      0xAF06u  /* search objects by metadata tag */

/* ── Object flags ── */
#define AGENTFS_FLAG_COLD      (1u << 0)  /* object has been evicted to cold tier */
#define AGENTFS_FLAG_PINNED    (1u << 1)  /* object must not be evicted */
#define AGENTFS_FLAG_READONLY  (1u << 2)  /* object is immutable after write */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENTFS_OP_READ */
    uint64_t hash_lo;         /* lower 64 bits of content-addressed hash */
    uint64_t hash_hi;         /* upper 64 bits of content-addressed hash */
    uint32_t shmem_offset;    /* byte offset in shared region to write data */
    uint32_t max_size;        /* maximum bytes to read */
} agentfs_req_read_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else agentfs_error_t */
    uint32_t bytes_read;      /* actual bytes written to shmem */
} agentfs_reply_read_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENTFS_OP_WRITE */
    uint32_t shmem_offset;    /* byte offset in shared region containing data */
    uint32_t data_size;       /* byte size of data to store */
    uint32_t flags;           /* AGENTFS_FLAG_* bitmask */
} agentfs_req_write_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint64_t hash_lo;         /* content hash of stored object */
    uint64_t hash_hi;
} agentfs_reply_write_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENTFS_OP_STAT */
    uint64_t hash_lo;
    uint64_t hash_hi;
} agentfs_req_stat_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t size;            /* object size in bytes */
    uint64_t created_ns;      /* creation timestamp (nanoseconds) */
    uint32_t flags;           /* AGENTFS_FLAG_* bitmask */
    uint32_t ref_count;       /* number of current references */
} agentfs_reply_stat_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENTFS_OP_LIST */
    uint32_t start_idx;       /* pagination start index */
    uint32_t max_entries;     /* max entries to return in shmem */
    uint32_t shmem_offset;    /* byte offset in shared region for output */
} agentfs_req_list_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;     /* number of agentfs_list_entry_t written to shmem */
    uint32_t total_objects;   /* total objects in store (for pagination) */
} agentfs_reply_list_t;

/* Entry written into shmem for AGENTFS_OP_LIST results */
typedef struct __attribute__((packed)) {
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint32_t size;
    uint32_t flags;
} agentfs_list_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENTFS_OP_DELETE */
    uint64_t hash_lo;
    uint64_t hash_hi;
} agentfs_req_delete_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} agentfs_reply_delete_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* AGENTFS_OP_SEARCH */
    uint32_t tag;             /* metadata tag to search (EVT_OBJECT_* constants) */
    uint32_t shmem_offset;    /* byte offset for result list */
    uint32_t max_entries;
} agentfs_req_search_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;     /* agentfs_list_entry_t entries written */
} agentfs_reply_search_t;

/* ── Error codes ── */
typedef enum {
    AGENTFS_OK                = 0,
    AGENTFS_ERR_NOT_FOUND     = 1,  /* hash not in store */
    AGENTFS_ERR_NO_SPACE      = 2,  /* store is full */
    AGENTFS_ERR_BAD_OFFSET    = 3,  /* shmem_offset out of mapped region */
    AGENTFS_ERR_TOO_LARGE     = 4,  /* data_size exceeds store maximum */
    AGENTFS_ERR_COLD          = 5,  /* object in cold tier; must be recalled first */
    AGENTFS_ERR_PINNED        = 6,  /* delete rejected: object is pinned */
    AGENTFS_ERR_BAD_HASH      = 7,  /* hash mismatch on write verification */
} agentfs_error_t;

/* ── Invariants ──
 * - Objects are addressed by content hash (SHA-256); collisions are treated as errors.
 * - Shared memory region must be mapped before any READ, WRITE, LIST, or SEARCH call.
 * - AGENTFS_FLAG_READONLY objects cannot be deleted (only tombstoned after all refs gone).
 * - LIST and SEARCH results are agentfs_list_entry_t arrays in the shared region.
 * - EVT_OBJECT_CREATED / EVT_OBJECT_DELETED / EVT_OBJECT_EVICTED are published to EventBus.
 */
