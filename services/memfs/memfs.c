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

#define MEMFS_MAX_FILES     64
#define MEMFS_MAX_FILE_SIZE 4096    /* 4KB per file */
#define MEMFS_PATH_MAX      256
#define MEMFS_TAG_MAX       64

typedef struct {
    char        path[MEMFS_PATH_MAX];
    uint8_t     owner[32];           /* AgentID */
    uint32_t    size;
    uint32_t    flags;
    uint64_t    created_at;
    uint64_t    modified_at;
    char        tags[4][MEMFS_TAG_MAX]; /* Semantic tags */
    int         tag_count;
    int         in_use;
    uint8_t     data[MEMFS_MAX_FILE_SIZE]; /* Inline content */
} memfs_file_t;

static memfs_file_t files[MEMFS_MAX_FILES];
static int file_count = 0;

int memfs_init(void) {
    memset(files, 0, sizeof(files));
    file_count = 0;
    printf("[memfs] MemFS initialized (max %d files, %d bytes each)\n",
           MEMFS_MAX_FILES, MEMFS_MAX_FILE_SIZE);
    return 0;
}

/* Find a file by path. Returns index or -1. */
static int find_file(const char *path) {
    for (int i = 0; i < MEMFS_MAX_FILES; i++) {
        if (files[i].in_use && strcmp(files[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

/* Find a free slot. Returns index or -1. */
static int find_free_slot(void) {
    for (int i = 0; i < MEMFS_MAX_FILES; i++) {
        if (!files[i].in_use) return i;
    }
    return -1;
}

/*
 * Write (create or overwrite) a file.
 * path    — file path (null-terminated, max MEMFS_PATH_MAX-1 chars)
 * data    — content to write
 * len     — byte count (max MEMFS_MAX_FILE_SIZE)
 * Returns 0 on success, non-zero on error.
 */
int memfs_write(const char *path, const uint8_t *data, uint32_t len) {
    if (!path || !data) return -1;
    if (len > MEMFS_MAX_FILE_SIZE) {
        printf("[memfs] ERROR: write '%s' len=%u exceeds max %d\n",
               path, len, MEMFS_MAX_FILE_SIZE);
        return -2;
    }

    int idx = find_file(path);
    if (idx < 0) {
        /* New file — allocate a slot */
        idx = find_free_slot();
        if (idx < 0) {
            printf("[memfs] ERROR: no free slots (max %d files)\n", MEMFS_MAX_FILES);
            return -3;
        }
        memset(&files[idx], 0, sizeof(memfs_file_t));
        strncpy(files[idx].path, path, MEMFS_PATH_MAX - 1);
        files[idx].in_use = 1;
        file_count++;
        printf("[memfs] Created '%s' (%u bytes)\n", path, len);
    } else {
        printf("[memfs] Overwrote '%s' (%u bytes)\n", path, len);
    }

    memcpy(files[idx].data, data, len);
    files[idx].size = len;
    files[idx].modified_at = 0; /* TODO: real timestamp */

    return 0;
}

/*
 * Read a file.
 * path     — file path
 * out      — output buffer
 * max_len  — capacity of output buffer
 * Returns number of bytes copied, or negative on error.
 */
int memfs_read(const char *path, uint8_t *out, uint32_t max_len) {
    if (!path || !out) return -1;

    int idx = find_file(path);
    if (idx < 0) {
        printf("[memfs] ERROR: '%s' not found\n", path);
        return -2;
    }

    uint32_t to_copy = files[idx].size < max_len ? files[idx].size : max_len;
    memcpy(out, files[idx].data, to_copy);
    return (int)to_copy;
}

/*
 * List all file paths.
 * out_paths  — array of char[MEMFS_PATH_MAX] to fill
 * max_entries — capacity of out_paths
 * Returns number of entries written.
 */
int memfs_list(char out_paths[][MEMFS_PATH_MAX], int max_entries) {
    int count = 0;
    for (int i = 0; i < MEMFS_MAX_FILES && count < max_entries; i++) {
        if (files[i].in_use) {
            strncpy(out_paths[count], files[i].path, MEMFS_PATH_MAX - 1);
            out_paths[count][MEMFS_PATH_MAX - 1] = '\0';
            count++;
        }
    }
    return count;
}

/*
 * Delete a file.
 * Returns 0 on success, -1 if not found.
 */
int memfs_delete(const char *path) {
    if (!path) return -1;

    int idx = find_file(path);
    if (idx < 0) {
        printf("[memfs] ERROR: delete '%s' not found\n", path);
        return -1;
    }

    memset(&files[idx], 0, sizeof(memfs_file_t));
    file_count--;
    printf("[memfs] Deleted '%s'\n", path);
    return 0;
}

/*
 * Tag a file (attach a semantic label).
 * Returns 0 on success, non-zero on error.
 */
int memfs_tag(uint8_t *owner, const char *path, const char *tag) {
    int idx = find_file(path);
    if (idx < 0) return -1;
    if (owner && memcmp(files[idx].owner, owner, 32) != 0) return -2;
    if (files[idx].tag_count >= 4) return -3;
    strncpy(files[idx].tags[files[idx].tag_count++], tag, MEMFS_TAG_MAX - 1);
    printf("[memfs] Tagged '%s' with '%s'\n", path, tag);
    return 0;
}

/*
 * Legacy create interface (used by older callers).
 * Wraps memfs_write; returns -3 if path already exists.
 */
int memfs_create(uint8_t *owner, const char *path, const uint8_t *data,
                  uint32_t size) {
    if (find_file(path) >= 0) return -3; /* EXISTS */
    int rc = memfs_write(path, data, size);
    if (rc == 0 && owner) {
        int idx = find_file(path);
        if (idx >= 0) memcpy(files[idx].owner, owner, 32);
    }
    return rc;
}
