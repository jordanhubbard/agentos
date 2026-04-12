/* ipc_bridge_daemon — Linux side of the agentOS IPC bridge.
 *
 * Maps the agentOS shared memory ring via /dev/mem and polls for
 * commands from native seL4 PDs.  Executes them and writes responses.
 *
 * Usage: ipc_bridge_daemon [--shmem-phys ADDR] [--shmem-size SIZE]
 *   Default: --shmem-phys 0x4000000 --shmem-size 0x10000 (64KB)
 *
 * Requires: root access (for /dev/mem) or a UIO driver mapping.
 *
 * Ring layout within the shared memory region:
 *   [IPC_CMD_RING_OFFSET  .. +sizeof(ipc_cmd_ring_t)]   seL4→Linux cmds
 *   [IPC_RESP_RING_OFFSET .. +sizeof(ipc_resp_ring_t)]  Linux→seL4 resps
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

/* _GNU_SOURCE is set via -D in the Makefile; guard against double-definition
 * when including this file in environments that pre-define it. */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Inline ring definitions (mirrored from ipc_bridge.h) ──────────────────
 *
 * Kept inline so this file compiles standalone inside the Linux guest without
 * access to the seL4 kernel tree.  Must stay in sync with
 * kernel/agentos-root-task/include/ipc_bridge.h.
 */

#define IPC_SHMEM_BASE          0x4000000UL
#define IPC_CMD_RING_OFFSET     0x1000UL
#define IPC_RESP_RING_OFFSET    0x2000UL

#define IPC_CMD_MAGIC           0x49504343UL  /* "IPCC" */
#define IPC_RESP_MAGIC          0x49504352UL  /* "IPCR" */

#define IPC_OP_EXEC             0x01
#define IPC_OP_WRITE            0x02
#define IPC_OP_READ             0x03
#define IPC_OP_PING             0x04
#define IPC_OP_SPAWN            0x05
#define IPC_OP_SIGNAL           0x06

#define IPC_RING_DEPTH          64
#define IPC_PAYLOAD_LEN         128

typedef struct {
    uint32_t seq;
    uint32_t op;
    uint32_t vm_slot;
    uint32_t payload_len;
    uint8_t  payload[IPC_PAYLOAD_LEN];
} ipc_cmd_t;

typedef struct {
    uint32_t seq;
    uint32_t status;
    uint32_t payload_len;
    uint32_t _pad;
    uint8_t  payload[IPC_PAYLOAD_LEN];
} ipc_resp_t;

typedef struct {
    uint32_t  magic;
    uint32_t  head;   /* seL4 write index (producer) */
    uint32_t  tail;   /* Linux read index (consumer) */
    uint32_t  _pad;
    ipc_cmd_t cmds[IPC_RING_DEPTH];
} ipc_cmd_ring_t;

typedef struct {
    uint32_t   magic;
    uint32_t   head;  /* Linux write index (producer) */
    uint32_t   tail;  /* seL4 read index (consumer) */
    uint32_t   _pad;
    ipc_resp_t resps[IPC_RING_DEPTH];
} ipc_resp_ring_t;

/* ── Globals ────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_quit = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Sleep for milliseconds without restarting on EINTR. */
static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/*
 * safe_payload_copy — copy at most IPC_PAYLOAD_LEN bytes from src into dest,
 * guaranteeing NUL termination.  Returns bytes copied (excluding NUL).
 */
static size_t safe_payload_copy(uint8_t *dest, const uint8_t *src, uint32_t len)
{
    size_t n = (len < IPC_PAYLOAD_LEN) ? len : IPC_PAYLOAD_LEN - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
    return n;
}

/* ── Command handlers ─────────────────────────────────────────────────────── */

/*
 * handle_ping — respond with a "pong" payload.
 */
static void handle_ping(const ipc_cmd_t *cmd, ipc_resp_t *resp)
{
    (void)cmd;
    const char *msg = "pong";
    size_t len = strlen(msg);
    memcpy(resp->payload, msg, len);
    resp->payload_len = (uint32_t)len;
    resp->status      = 0;
}

/*
 * handle_exec — run an arbitrary shell command via system(3).
 * payload: NUL-terminated command string.
 * response status: exit status from WEXITSTATUS, or 127 on fork failure.
 */
static void handle_exec(const ipc_cmd_t *cmd, ipc_resp_t *resp)
{
    char cmdstr[IPC_PAYLOAD_LEN + 1];
    safe_payload_copy((uint8_t *)cmdstr, cmd->payload, cmd->payload_len);

    fprintf(stderr, "[ipc-bridge] EXEC: %s\n", cmdstr);

    int rc = system(cmdstr);
    if (rc == -1) {
        resp->status = (uint32_t)errno;
    } else if (WIFEXITED(rc)) {
        resp->status = (uint32_t)WEXITSTATUS(rc);
    } else {
        resp->status = (uint32_t)rc;
    }
    resp->payload_len = 0;
}

/*
 * handle_write — write data to a guest file.
 * payload format: "<path>\0<data>" — path is NUL-terminated, data follows.
 * The path is extracted up to the first NUL; remaining bytes are written.
 */
static void handle_write(const ipc_cmd_t *cmd, ipc_resp_t *resp)
{
    if (cmd->payload_len == 0) {
        resp->status = EINVAL;
        return;
    }

    /* Locate the embedded NUL separator between path and data. */
    const uint8_t *payload = cmd->payload;
    uint32_t       total   = (cmd->payload_len < IPC_PAYLOAD_LEN)
                                 ? cmd->payload_len : IPC_PAYLOAD_LEN;

    /* Scan for the separator within the valid payload range. */
    uint32_t sep = total; /* pessimistic: no separator found */
    for (uint32_t i = 0; i < total; i++) {
        if (payload[i] == '\0') {
            sep = i;
            break;
        }
    }

    /* Build a safe, NUL-terminated path string. */
    char path[IPC_PAYLOAD_LEN + 1];
    size_t path_len = (sep < IPC_PAYLOAD_LEN) ? sep : IPC_PAYLOAD_LEN - 1;
    memcpy(path, payload, path_len);
    path[path_len] = '\0';

    if (path_len == 0) {
        resp->status = EINVAL;
        return;
    }

    /* Data starts immediately after the separator. */
    const uint8_t *data     = payload + sep + 1;
    uint32_t       data_len = (sep + 1 < total) ? (total - sep - 1) : 0;

    fprintf(stderr, "[ipc-bridge] WRITE: %s (%u bytes)\n", path, data_len);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        resp->status = (uint32_t)errno;
        return;
    }

    ssize_t written = 0;
    if (data_len > 0) {
        written = write(fd, data, data_len);
    }
    close(fd);

    if (written < 0) {
        resp->status = (uint32_t)errno;
    } else {
        resp->status = 0;
    }
    resp->payload_len = 0;
}

/*
 * handle_read — read a guest file and return its contents.
 * payload: NUL-terminated path.
 * response payload: file contents (clamped to IPC_PAYLOAD_LEN).
 * response status: bytes read on success, errno on failure.
 */
static void handle_read(const ipc_cmd_t *cmd, ipc_resp_t *resp)
{
    char path[IPC_PAYLOAD_LEN + 1];
    safe_payload_copy((uint8_t *)path, cmd->payload, cmd->payload_len);

    if (path[0] == '\0') {
        resp->status = EINVAL;
        return;
    }

    fprintf(stderr, "[ipc-bridge] READ: %s\n", path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        resp->status = (uint32_t)errno;
        resp->payload_len = 0;
        return;
    }

    ssize_t n = read(fd, resp->payload, IPC_PAYLOAD_LEN);
    close(fd);

    if (n < 0) {
        resp->status      = (uint32_t)errno;
        resp->payload_len = 0;
    } else {
        resp->status      = (uint32_t)n;
        resp->payload_len = (uint32_t)n;
    }
}

/*
 * handle_spawn — fork and exec payload as a detached background process.
 * payload: NUL-terminated command string passed to sh -c.
 * response status: child PID on success, errno on failure.
 */
static void handle_spawn(const ipc_cmd_t *cmd, ipc_resp_t *resp)
{
    char cmdstr[IPC_PAYLOAD_LEN + 1];
    safe_payload_copy((uint8_t *)cmdstr, cmd->payload, cmd->payload_len);

    fprintf(stderr, "[ipc-bridge] SPAWN: %s\n", cmdstr);

    pid_t pid = fork();
    if (pid < 0) {
        resp->status = (uint32_t)errno;
        return;
    }

    if (pid == 0) {
        /* Child: become a new session leader so it survives daemon exit. */
        setsid();

        /* Redirect stdio to /dev/null to fully detach. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }

        execl("/bin/sh", "sh", "-c", cmdstr, (char *)NULL);
        _exit(127);
    }

    /* Parent: do not wait — child is detached.  Return pid to caller. */
    resp->status      = 0;
    resp->payload_len = (uint32_t)snprintf((char *)resp->payload,
                                           IPC_PAYLOAD_LEN, "%d", (int)pid);
}

/*
 * handle_signal — send a signal to a guest process.
 * payload format: "<pid> <signal>" as a decimal ASCII string.
 * response status: 0 on success, errno on failure.
 */
static void handle_signal(const ipc_cmd_t *cmd, ipc_resp_t *resp)
{
    char buf[IPC_PAYLOAD_LEN + 1];
    safe_payload_copy((uint8_t *)buf, cmd->payload, cmd->payload_len);

    int pid_val = 0, sig_val = 0;
    if (sscanf(buf, "%d %d", &pid_val, &sig_val) != 2) {
        resp->status = EINVAL;
        return;
    }

    fprintf(stderr, "[ipc-bridge] SIGNAL: pid=%d sig=%d\n", pid_val, sig_val);

    if (kill((pid_t)pid_val, sig_val) < 0) {
        resp->status = (uint32_t)errno;
    } else {
        resp->status = 0;
    }
    resp->payload_len = 0;
}

/* ── Central dispatch ─────────────────────────────────────────────────────── */

/*
 * handle_cmd — dispatch a single command and populate the response.
 *
 * Always produces a valid response (even for unknown ops) so the seL4 side
 * never stalls waiting for an answer.
 */
static void handle_cmd(const ipc_cmd_t *cmd, ipc_resp_t *resp)
{
    /* Initialise response fields. */
    memset(resp, 0, sizeof(*resp));
    resp->seq = cmd->seq;

    switch (cmd->op) {
    case IPC_OP_PING:
        handle_ping(cmd, resp);
        break;
    case IPC_OP_EXEC:
        handle_exec(cmd, resp);
        break;
    case IPC_OP_WRITE:
        handle_write(cmd, resp);
        break;
    case IPC_OP_READ:
        handle_read(cmd, resp);
        break;
    case IPC_OP_SPAWN:
        handle_spawn(cmd, resp);
        break;
    case IPC_OP_SIGNAL:
        handle_signal(cmd, resp);
        break;
    default:
        fprintf(stderr, "[ipc-bridge] unknown op 0x%02x (seq=%u), NAK\n",
                cmd->op, cmd->seq);
        resp->status = ENOSYS;
        break;
    }
}

/* ── Poll loop ───────────────────────────────────────────────────────────── */

/*
 * ipc_daemon_run — main poll loop.
 *
 * Validates both ring magic values on entry.  On each iteration it drains all
 * pending commands from the cmd ring, dispatches each, and enqueues the
 * response into the resp ring.  Sleeps 1 ms between idle polls.
 *
 * Returns only when a signal sets g_quit.
 */
static int ipc_daemon_run(void *shmem)
{
    ipc_cmd_ring_t  *cmd_ring  = (ipc_cmd_ring_t  *)((uint8_t *)shmem
                                                      + IPC_CMD_RING_OFFSET);
    ipc_resp_ring_t *resp_ring = (ipc_resp_ring_t *)((uint8_t *)shmem
                                                     + IPC_RESP_RING_OFFSET);

    /* Validate magic — wait up to ~5 s for seL4 to initialise. */
    int retries = 0;
    while (!g_quit) {
        __sync_synchronize();
        if (cmd_ring->magic  == IPC_CMD_MAGIC &&
            resp_ring->magic == IPC_RESP_MAGIC) {
            break;
        }
        if (retries == 0) {
            fprintf(stderr, "[ipc-bridge] waiting for ring magic "
                    "(cmd=0x%08x resp=0x%08x)...\n",
                    cmd_ring->magic, resp_ring->magic);
        }
        if (++retries > 5000) {
            fprintf(stderr, "[ipc-bridge] timeout waiting for valid ring magic. "
                    "Aborting.\n");
            return 1;
        }
        sleep_ms(1);
    }

    if (g_quit)
        return 0;

    fprintf(stderr, "[ipc-bridge] ring magic OK. Entering poll loop.\n");

    while (!g_quit) {
        /* Reap any zombie children spawned by IPC_OP_SPAWN. */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;

        /* Memory barrier before reading producer index. */
        __sync_synchronize();

        uint32_t head = cmd_ring->head;
        uint32_t tail = cmd_ring->tail;

        if (head == tail) {
            /* Ring empty — sleep 1 ms to avoid busy-spinning. */
            sleep_ms(1);
            continue;
        }

        /* Drain all pending commands in this burst. */
        while (tail != head) {
            /* Bounds-check the slot index defensively. */
            uint32_t slot = tail % IPC_RING_DEPTH;

            /* Take a local copy so we can release the slot quickly. */
            ipc_cmd_t cmd_copy;
            memcpy(&cmd_copy, &cmd_ring->cmds[slot], sizeof(cmd_copy));

            /* Advance the consumer tail before executing (allows seL4 to
             * enqueue more commands while we're working). */
            __sync_synchronize();
            cmd_ring->tail = tail + 1;
            __sync_synchronize();

            fprintf(stderr, "[ipc-bridge] cmd seq=%u op=0x%02x vm=%u len=%u\n",
                    cmd_copy.seq, cmd_copy.op, cmd_copy.vm_slot,
                    cmd_copy.payload_len);

            /* Dispatch. */
            ipc_resp_t resp;
            handle_cmd(&cmd_copy, &resp);

            /* Enqueue the response.
             * If the response ring is full, stall until seL4 drains it.
             * This is a safety valve — in normal operation the ring never
             * fills because seL4 polls responses eagerly. */
            uint32_t rhead, rtail;
            do {
                __sync_synchronize();
                rhead = resp_ring->head;
                rtail = resp_ring->tail;
                if (rhead - rtail < IPC_RING_DEPTH)
                    break;
                fprintf(stderr, "[ipc-bridge] resp ring full, waiting...\n");
                sleep_ms(1);
            } while (!g_quit);

            if (g_quit)
                goto done;

            uint32_t rslot = rhead % IPC_RING_DEPTH;
            memcpy(&resp_ring->resps[rslot], &resp, sizeof(resp));
            __sync_synchronize();
            resp_ring->head = rhead + 1;
            __sync_synchronize();

            /* Refresh head in case seL4 enqueued more commands. */
            tail = tail + 1;
            head = cmd_ring->head;
        }
    }

done:
    fprintf(stderr, "[ipc-bridge] shutting down.\n");
    return 0;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--shmem-phys ADDR] [--shmem-size SIZE]\n"
            "  --shmem-phys ADDR   Physical base address of the IPC shmem MR\n"
            "                      (hex or decimal, default 0x%lx)\n"
            "  --shmem-size SIZE   Size of the region to map in bytes\n"
            "                      (hex or decimal, default 0x10000)\n"
            "  --help              Show this message\n",
            argv0, (unsigned long)IPC_SHMEM_BASE);
}

int main(int argc, char *argv[])
{
    unsigned long shmem_phys = IPC_SHMEM_BASE;
    unsigned long shmem_size = 0x10000UL; /* 64 KB */

    /* Argument parsing. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--shmem-phys") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: --shmem-phys requires an argument\n");
                return 1;
            }
            char *end;
            shmem_phys = strtoul(argv[i], &end, 0);
            if (*end != '\0') {
                fprintf(stderr, "error: invalid address '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--shmem-size") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "error: --shmem-size requires an argument\n");
                return 1;
            }
            char *end;
            shmem_size = strtoul(argv[i], &end, 0);
            if (*end != '\0' || shmem_size == 0) {
                fprintf(stderr, "error: invalid size '%s'\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    fprintf(stderr, "[ipc-bridge] phys=0x%lx size=0x%lx\n",
            shmem_phys, shmem_size);

    /* Validate that the ring offsets fit within the requested size. */
    if (IPC_RESP_RING_OFFSET + sizeof(ipc_resp_ring_t) > shmem_size) {
        fprintf(stderr, "[ipc-bridge] error: shmem_size 0x%lx is too small "
                "to hold both rings (need at least 0x%zx bytes)\n",
                shmem_size,
                (size_t)(IPC_RESP_RING_OFFSET + sizeof(ipc_resp_ring_t)));
        return 1;
    }

    /* Install signal handlers for clean shutdown. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Open /dev/mem and map the shared region. */
    int devmem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (devmem_fd < 0) {
        fprintf(stderr, "[ipc-bridge] open(/dev/mem): %s\n", strerror(errno));
        fprintf(stderr, "[ipc-bridge] hint: run as root, or load a UIO driver "
                "and pass the UIO device path via /dev/uioN instead.\n");
        return 1;
    }

    void *shmem = mmap(NULL, shmem_size,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED,
                       devmem_fd,
                       (off_t)shmem_phys);
    if (shmem == MAP_FAILED) {
        fprintf(stderr, "[ipc-bridge] mmap(0x%lx, 0x%lx): %s\n",
                shmem_phys, shmem_size, strerror(errno));
        close(devmem_fd);
        return 1;
    }

    fprintf(stderr, "[ipc-bridge] shmem mapped at %p\n", shmem);

    int rc = ipc_daemon_run(shmem);

    munmap(shmem, shmem_size);
    close(devmem_fd);
    return rc;
}
