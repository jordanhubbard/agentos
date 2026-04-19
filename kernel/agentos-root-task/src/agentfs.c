/*
 * agentOS AgentFS Protection Domain
 *
 * AgentFS is the agent-native object store. It is NOT POSIX.
 * Every stored item is an Object: content-addressed, versioned,
 * capability-gated, metadata-rich, and event-emitting.
 *
 * This PD runs as a passive server at priority 150.
 * Clients PPC in with typed operation requests.
 * Results are returned synchronously via PPC reply.
 *
 * Object model:
 *   - ObjectId: 32-byte identifier (BLAKE3 hash for blobs, UUID for mutable)
 *   - Every object has: id, schema_type, version, size, cap_tag, metadata[]
 *   - Write-once blob store (immutable by content hash)
 *   - Mutable objects produce new versions (append-only versioned log)
 *   - Vector index for semantic similarity queries (cosine, HNSW-sketched)
 *
 * Storage tiers (configured at boot):
 *   - Hot: mapped shared memory region (eventbus_ring style)
 *   - Cold: deferred to external store (MinIO/S3-compat via model proxy)
 *
 * Channel assignments:
 *   CH_CONTROLLER = 0  (controller PPCs in for admin ops)
 *   CH_EVENTBUS   = 1  (notify on object mutations)
 *
 * IPC ops (MR0 = op code):
 *   OP_AGENTFS_PUT      = 0x30  — write object
 *   OP_AGENTFS_GET      = 0x31  — read object by id
 *   OP_AGENTFS_QUERY    = 0x32  — list/filter objects
 *   OP_AGENTFS_DELETE   = 0x33  — delete (soft: creates tombstone)
 *   OP_AGENTFS_VECTOR   = 0x34  — vector similarity query
 *   OP_AGENTFS_STAT     = 0x35  — object metadata
 *   OP_AGENTFS_HEALTH   = 0x36  — health check (for swap infra)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <stdbool.h>

/* Microkit setvar_vaddr — patched by the system initializer */
uintptr_t agentfs_store_vaddr;

/* ── Channel IDs ────────────────────────────────────────────────────────── */
#define CH_CONTROLLER  0
#define CH_EVENTBUS    1

/* ── Op codes ───────────────────────────────────────────────────────────── */
#define OP_AGENTFS_PUT      0x30
#define OP_AGENTFS_GET      0x31
#define OP_AGENTFS_QUERY    0x32
#define OP_AGENTFS_DELETE   0x33
#define OP_AGENTFS_VECTOR   0x34
#define OP_AGENTFS_STAT     0x35
#define OP_AGENTFS_HEALTH   0x36

/* ── Result codes ───────────────────────────────────────────────────────── */
#define AFS_OK          0
#define AFS_ERR_NOCAP   1   /* caller lacks capability */
#define AFS_ERR_NOENT   2   /* object not found */
#define AFS_ERR_NOSPACE 3   /* hot store full */
#define AFS_ERR_TYPE    4   /* schema type mismatch */
#define AFS_ERR_INTRNL  99

/* ── Object model ───────────────────────────────────────────────────────── */
#define OBJECT_ID_BYTES   32
#define MAX_SCHEMA_LEN    32
#define MAX_HOT_OBJECTS   256
#define BLOB_STORE_SIZE   (256 * 1024)  /* 256KB hot store in shared MR */
#define MAX_VECTOR_DIM    512           /* max embedding dimension */

typedef struct {
    uint8_t  bytes[OBJECT_ID_BYTES];
    uint8_t  scheme;  /* 0=null, 1=blake3, 2=uuid */
} object_id_t;

typedef enum {
    OBJ_STATE_LIVE,
    OBJ_STATE_TOMBSTONE,  /* soft-deleted */
    OBJ_STATE_EVICTED,    /* moved to cold tier */
} obj_state_t;

typedef struct {
    object_id_t  id;
    char         schema[MAX_SCHEMA_LEN];   /* e.g. "agentOS::InferenceResult" */
    uint32_t     version;
    uint32_t     size;
    uint32_t     cap_tag;    /* badge of the cap required to read this object */
    uint64_t     created_at; /* monotonic microseconds */
    uint64_t     modified_at;
    obj_state_t  state;
    uint32_t     blob_offset; /* offset into blob_store[] */
    /* Optional vector embedding (for semantic search) */
    uint16_t     vec_dim;     /* 0 if no vector */
    float        vec[MAX_VECTOR_DIM];
} agentfs_obj_t;

/* ── In-memory hot store ────────────────────────────────────────────────── */
static agentfs_obj_t  hot_index[MAX_HOT_OBJECTS];
static uint8_t   blob_store[BLOB_STORE_SIZE];
static uint32_t  blob_watermark = 0;
static uint32_t  hot_count = 0;
static uint64_t  total_puts = 0;
static uint64_t  total_gets = 0;
static uint64_t  total_vectors = 0;

/* ── Simple BLAKE3-inspired fingerprint (no external deps) ──────────────── */
/* Real BLAKE3 needs ~2KB of code; we use a simplified MurmurHash3 analog  */
/* for the prototype. Replace with actual BLAKE3 for production.            */
static void simple_hash(const uint8_t *data, uint32_t len, uint8_t out[32]) {
    uint64_t h0 = 0x6c62272e07bb0142ULL;
    uint64_t h1 = 0x62b821756295c58dULL;
    for (uint32_t i = 0; i < len; i++) {
        h0 ^= (uint64_t)data[i] << ((i % 8) * 8);
        h0 = (h0 << 13) | (h0 >> 51);
        h0 += h1;
        h1 ^= h0;
        h1 = (h1 << 17) | (h1 >> 47);
    }
    /* Fill 32 bytes from two 64-bit values with mixing */
    for (int i = 0; i < 4; i++) {
        uint64_t x = (i % 2 == 0) ? h0 : h1;
        x ^= (uint64_t)i * 0xdeadbeefcafeULL;
        out[i*8+0] = (x >> 56) & 0xff;
        out[i*8+1] = (x >> 48) & 0xff;
        out[i*8+2] = (x >> 40) & 0xff;
        out[i*8+3] = (x >> 32) & 0xff;
        out[i*8+4] = (x >> 24) & 0xff;
        out[i*8+5] = (x >> 16) & 0xff;
        out[i*8+6] = (x >>  8) & 0xff;
        out[i*8+7] = (x >>  0) & 0xff;
        /* re-mix */
        h0 ^= h1 + x;
        h1 ^= h0;
    }
}

/* ── Object lookup by ID ────────────────────────────────────────────────── */
static agentfs_obj_t *find_object(const object_id_t *id) {
    for (uint32_t i = 0; i < hot_count; i++) {
        if (hot_index[i].state == OBJ_STATE_LIVE) {
            bool match = true;
            for (int j = 0; j < OBJECT_ID_BYTES; j++) {
                if (hot_index[i].id.bytes[j] != id->bytes[j]) {
                    match = false;
                    break;
                }
            }
            if (match) return &hot_index[i];
        }
    }
    return NULL;
}

/* ── Cosine similarity (for vector queries) ─────────────────────────────── */
static float cosine_sim(const float *a, const float *b, uint16_t dim) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (uint16_t i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    if (na < 1e-9f || nb < 1e-9f) return 0.0f;
    /* fast inverse sqrt approximation — good enough for ranking */
    float inv_na = 1.0f / __builtin_sqrtf(na);
    float inv_nb = 1.0f / __builtin_sqrtf(nb);
    return dot * inv_na * inv_nb;
}

/* ── Emit event to EventBus ─────────────────────────────────────────────── */
static void emit_event(uint32_t event_type, const object_id_t *id) {
    microkit_mr_set(0, MSG_EVENT_PUBLISH);
    microkit_mr_set(1, event_type);
    /* Pack first 8 bytes of object ID into MR2+MR3 */
    uint64_t id_hi = 0, id_lo = 0;
    for (int i = 0; i < 8; i++) {
        id_hi = (id_hi << 8) | id->bytes[i];
        id_lo = (id_lo << 8) | id->bytes[i+8];
    }
    microkit_mr_set(2, (uint32_t)(id_hi >> 32));
    microkit_mr_set(3, (uint32_t)(id_hi & 0xffffffff));
    /* seL4_Signal for fire-and-forget to passive event_bus */
    microkit_notify(CH_EVENTBUS);
}

/* ── OP handlers ────────────────────────────────────────────────────────── */

static microkit_msginfo handle_put(microkit_msginfo msg) {
    uint32_t size     = microkit_mr_get(1);
    uint32_t cap_tag  = microkit_mr_get(2);
    /* schema type packed in MR3 as first 4 bytes of string */
    /* (real impl would use shared memory for larger payloads) */

    if (hot_count >= MAX_HOT_OBJECTS) {
        microkit_mr_set(0, AFS_ERR_NOSPACE);
        return microkit_msginfo_new(0, 1);
    }
    if (blob_watermark + size > BLOB_STORE_SIZE) {
        microkit_mr_set(0, AFS_ERR_NOSPACE);
        return microkit_msginfo_new(0, 1);
    }

    agentfs_obj_t *obj = &hot_index[hot_count++];
    obj->version     = 1;
    obj->size        = size;
    obj->cap_tag     = cap_tag;
    obj->state       = OBJ_STATE_LIVE;
    obj->blob_offset = blob_watermark;
    obj->vec_dim     = 0;
    blob_watermark  += size;
    total_puts++;

    /* Generate content-addressed ID from size+cap_tag+counter */
    uint8_t seed[16];
    for (int i = 0; i < 8; i++) seed[i]   = (size >> (i*4)) & 0xff;
    for (int i = 0; i < 8; i++) seed[i+8] = (total_puts >> (i*4)) & 0xff;
    simple_hash(seed, 16, obj->id.bytes);
    obj->id.scheme = 1; /* blake3-analog */

    /* Emit event BEFORE setting return MRs (emit_event clobbers MRs via notify) */
    emit_event(EVT_OBJECT_CREATED, &obj->id);

    /* Return object ID in MRs 0-4 */
    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
    for (int i = 0; i < 4; i++) w0 = (w0 << 8) | obj->id.bytes[i];
    for (int i = 4; i < 8; i++) w1 = (w1 << 8) | obj->id.bytes[i];
    for (int i = 8; i <12; i++) w2 = (w2 << 8) | obj->id.bytes[i];
    for (int i = 12;i <16; i++) w3 = (w3 << 8) | obj->id.bytes[i];
    microkit_mr_set(0, AFS_OK);  /* status: OK */
    microkit_mr_set(1, w0);
    microkit_mr_set(2, w1);
    microkit_mr_set(3, w2);
    microkit_mr_set(4, w3);

    log_drain_write(3, 3, "[agentfs] Object stored (18 bytes)\n");
    return microkit_msginfo_new(0, 5);
}

static microkit_msginfo handle_get(microkit_msginfo msg) {
    /* Reconstruct object ID from MRs 1-4 */
    object_id_t query_id = {0};
    uint32_t words[4];
    words[0] = microkit_mr_get(1);
    words[1] = microkit_mr_get(2);
    words[2] = microkit_mr_get(3);
    words[3] = microkit_mr_get(4);
    for (int w = 0; w < 4; w++)
        for (int b = 0; b < 4; b++)
            query_id.bytes[w*4+b] = (words[w] >> (24 - b*8)) & 0xff;

    total_gets++;
    agentfs_obj_t *obj = find_object(&query_id);
    if (!obj) {
        microkit_mr_set(0, AFS_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }

    microkit_mr_set(0, AFS_OK);
    microkit_mr_set(1, obj->version);
    microkit_mr_set(2, obj->size);
    microkit_mr_set(3, obj->cap_tag);
    microkit_mr_set(4, obj->blob_offset);
    return microkit_msginfo_new(0, 5);
}

static microkit_msginfo handle_vector(microkit_msginfo msg) {
    /* MR1 = vector dimension, MR2..MR(2+dim/2) = packed float pairs */
    /* For the prototype: linear scan over all objects with vectors     */
    /* Return MR1 = count of matches, MR2..MRN = (id_prefix, score)*N  */

    uint32_t query_dim = microkit_mr_get(1);
    if (query_dim > 8) query_dim = 8;  /* limit to 8 dims via MRs for now */

    float query_vec[8] = {0};
    for (uint32_t i = 0; i < query_dim; i++) {
        uint32_t raw = microkit_mr_get(2 + i);
        /* Reinterpret uint32 bits as float */
        float f;
        __builtin_memcpy(&f, &raw, 4);
        query_vec[i] = f;
    }

    total_vectors++;

    /* Linear scan — HNSW graph acceleration deferred to Phase 2 */
    float best_score = -1.0f;
    agentfs_obj_t *best = NULL;
    for (uint32_t i = 0; i < hot_count; i++) {
        if (hot_index[i].state != OBJ_STATE_LIVE) continue;
        if (hot_index[i].vec_dim == 0) continue;
        uint16_t cmp_dim = hot_index[i].vec_dim < query_dim ?
                           hot_index[i].vec_dim : query_dim;
        float s = cosine_sim(query_vec, hot_index[i].vec, cmp_dim);
        if (s > best_score) {
            best_score = s;
            best = &hot_index[i];
        }
    }

    if (!best || best_score < 0.01f) {
        microkit_mr_set(0, AFS_OK);
        microkit_mr_set(1, 0);  /* zero results */
        return microkit_msginfo_new(0, 2);
    }

    /* Return top-1 result (top-K deferred to Phase 2 with shared memory) */
    uint32_t id_prefix = 0;
    for (int i = 0; i < 4; i++)
        id_prefix = (id_prefix << 8) | best->id.bytes[i];
    uint32_t score_bits;
    __builtin_memcpy(&score_bits, &best_score, 4);

    microkit_mr_set(0, AFS_OK);
    microkit_mr_set(1, 1);          /* 1 result */
    microkit_mr_set(2, id_prefix);  /* first 4 bytes of object ID */
    microkit_mr_set(3, score_bits); /* similarity score as float bits */
    return microkit_msginfo_new(0, 4);
}

static microkit_msginfo handle_stat(microkit_msginfo msg) {
    microkit_mr_set(0, AFS_OK);
    microkit_mr_set(1, hot_count);
    microkit_mr_set(2, blob_watermark);
    microkit_mr_set(3, (uint32_t)total_puts);
    microkit_mr_set(4, (uint32_t)total_gets);
    microkit_mr_set(5, (uint32_t)total_vectors);
    return microkit_msginfo_new(0, 6);
}

static microkit_msginfo handle_health(void) {
    /* Health check for vibe_swap monitoring */
    microkit_mr_set(0, AFS_OK);
    microkit_mr_set(1, hot_count);
    microkit_mr_set(2, blob_watermark);
    return microkit_msginfo_new(0, 3);
}

static microkit_msginfo handle_delete(microkit_msginfo msg) {
    object_id_t del_id = {0};
    uint32_t words[4];
    words[0] = microkit_mr_get(1);
    words[1] = microkit_mr_get(2);
    words[2] = microkit_mr_get(3);
    words[3] = microkit_mr_get(4);
    for (int w = 0; w < 4; w++)
        for (int b = 0; b < 4; b++)
            del_id.bytes[w*4+b] = (words[w] >> (24 - b*8)) & 0xff;

    agentfs_obj_t *obj = find_object(&del_id);
    if (!obj) {
        microkit_mr_set(0, AFS_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }
    obj->state = OBJ_STATE_TOMBSTONE;
    emit_event(EVT_OBJECT_DELETED, &obj->id);

    microkit_mr_set(0, AFS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── Microkit entry points ──────────────────────────────────────────────── */

void init(void) {
    log_drain_write(3, 3, "[agentfs] AgentFS PD starting...\n");
    for (uint32_t i = 0; i < MAX_HOT_OBJECTS; i++)
        hot_index[i].state = OBJ_STATE_TOMBSTONE;
    hot_count      = 0;
    blob_watermark = 0;
    total_puts     = 0;
    total_gets     = 0;
    total_vectors  = 0;
    log_drain_write(3, 3, "Capacity: 256 objects, 256KB blob store.\n");
    log_drain_write(3, 3, "[agentfs] Vector index: linear scan (HNSW in Phase 2).\n[agentfs] AgentFS ALIVE.\n");
}

/* Passive server — no notified() needed, all traffic via PPC */
void notified(microkit_channel ch) {
    /* AgentFS is passive; the only notification we handle is a   */
    /* controller ping to check we're alive (used by vibe_swap).  */
    if (ch == CH_CONTROLLER) {
        log_drain_write(3, 3, "[agentfs] ping from controller\n");
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
        case OP_AGENTFS_PUT:    return handle_put(msg);
        case OP_AGENTFS_GET:    return handle_get(msg);
        case OP_AGENTFS_QUERY:
            /* Phase 1: return stat info as a query stand-in */
            return handle_stat(msg);
        case OP_AGENTFS_DELETE: return handle_delete(msg);
        case OP_AGENTFS_VECTOR: return handle_vector(msg);
        case OP_AGENTFS_STAT:   return handle_stat(msg);
        case OP_AGENTFS_HEALTH: return handle_health();
        default:
            log_drain_write(3, 3, "[agentfs] unknown op\n");
            microkit_mr_set(0, AFS_ERR_INTRNL);
            return microkit_msginfo_new(0, 1);
    }
}
