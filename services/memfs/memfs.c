/*
 * agentOS MemFS — Virtual Filesystem Service
 *
 * In-memory filesystem with per-agent namespaces.
 * This is the REFERENCE implementation — agents can vibe-code replacements.
 *
 * Features:
 *   - Flat namespace (agent_name/path/to/file)
 *   - Content-addressable storage (SHA-256 dedup)
 *   - Capability-gated access per namespace
 *   - Semantic tagging (agents can attach metadata/embeddings)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define MEMFS_MAX_FILES     1024
#define MEMFS_MAX_FILE_SIZE (1 << 20)  /* 1MB per file */
#define MEMFS_PATH_MAX      256
#define MEMFS_TAG_MAX       64

typedef struct {
    char        path[MEMFS_PATH_MAX];
    uint8_t     owner[32];           /* AgentID */
    uint8_t     content_hash[32];    /* SHA-256 of content */
    uint32_t    size;
    uint32_t    flags;
    uint64_t    created_at;
    uint64_t    modified_at;
    char        tags[4][MEMFS_TAG_MAX]; /* Semantic tags */
    int         tag_count;
    /* Content stored separately in content pool */
    int         content_idx;
} memfs_file_t;

/* Simple content pool (dedup by hash) */
#define MEMFS_CONTENT_POOL_SIZE 256
typedef struct {
    uint8_t     hash[32];
    uint8_t    *data;
    uint32_t    size;
    uint32_t    refcount;
} memfs_content_t;

static memfs_file_t files[MEMFS_MAX_FILES];
static int file_count = 0;

static memfs_content_t content_pool[MEMFS_CONTENT_POOL_SIZE];
static int content_count = 0;

int memfs_init(void) {
    memset(files, 0, sizeof(files));
    memset(content_pool, 0, sizeof(content_pool));
    file_count = 0;
    content_count = 0;
    printf("[memfs] MemFS initialized (max %d files)\n", MEMFS_MAX_FILES);
    return 0;
}

int memfs_create(uint8_t *owner, const char *path, const uint8_t *data, 
                  uint32_t size) {
    if (file_count >= MEMFS_MAX_FILES) return -1;
    if (size > MEMFS_MAX_FILE_SIZE) return -2;
    
    /* Check if path already exists */
    for (int i = 0; i < file_count; i++) {
        if (strcmp(files[i].path, path) == 0) return -3; /* EXISTS */
    }
    
    int idx = file_count++;
    strncpy(files[idx].path, path, MEMFS_PATH_MAX - 1);
    memcpy(files[idx].owner, owner, 32);
    files[idx].size = size;
    files[idx].flags = 0;
    files[idx].tag_count = 0;
    
    /* TODO: Compute SHA-256, check content pool for dedup */
    /* For now, simple storage */
    files[idx].content_idx = -1;  /* Inline for now */
    
    printf("[memfs] Created '%s' (%u bytes)\n", path, size);
    return 0;
}

int memfs_read(uint8_t *requester, const char *path, uint8_t *buf, 
                uint32_t buf_size, uint32_t *bytes_read) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(files[i].path, path) == 0) {
            /* TODO: Capability check — does requester have access? */
            uint32_t to_read = files[i].size < buf_size ? files[i].size : buf_size;
            /* TODO: Actually copy data from content pool */
            *bytes_read = to_read;
            return 0;
        }
    }
    return -1; /* NOT FOUND */
}

int memfs_tag(uint8_t *owner, const char *path, const char *tag) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(files[i].path, path) == 0) {
            if (memcmp(files[i].owner, owner, 32) != 0) return -2;
            if (files[i].tag_count >= 4) return -3;
            strncpy(files[i].tags[files[i].tag_count++], tag, MEMFS_TAG_MAX - 1);
            printf("[memfs] Tagged '%s' with '%s'\n", path, tag);
            return 0;
        }
    }
    return -1;
}

int memfs_list(const char *prefix, char **out_paths, int *out_count) {
    int count = 0;
    for (int i = 0; i < file_count; i++) {
        if (strncmp(files[i].path, prefix, strlen(prefix)) == 0) {
            /* TODO: Fill output buffer */
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int memfs_delete(uint8_t *requester, const char *path) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(files[i].path, path) == 0) {
            /* TODO: Capability check */
            /* Swap with last */
            if (i < file_count - 1) {
                memcpy(&files[i], &files[file_count - 1], sizeof(memfs_file_t));
            }
            file_count--;
            printf("[memfs] Deleted '%s'\n", path);
            return 0;
        }
    }
    return -1;
}
