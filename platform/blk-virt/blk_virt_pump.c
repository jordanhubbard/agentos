/*
 * blk_virt pump — sDDF-shaped block request pump with no seL4 dependency.
 *
 * Each client posts READ/WRITE/FLUSH/BARRIER on its request queue.  The pump
 * serves them from a backend callback (blk_virt: the virtio_blk driver) or
 * from the selected RAM image, and posts responses.  The rings are shared
 * between PDs: indices are read as volatile and every hand-over is fenced.
 */

#include <platform/blk_virt_pump.h>

static void aos_bzero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0u) {
        *d++ = 0;
    }
}

static void aos_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0u) {
        *d++ = *s++;
    }
}

static void aos_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static uint32_t req_len(const aos_blk_req_queue_t *q)
{
    return *(volatile const uint32_t *)&q->tail -
           *(volatile const uint32_t *)&q->head;
}

static uint32_t resp_len(const aos_blk_resp_queue_t *q)
{
    return *(volatile const uint32_t *)&q->tail -
           *(volatile const uint32_t *)&q->head;
}

uint32_t aos_blk_queue_req_length(const aos_blk_req_queue_t *q)
{
    return q ? req_len(q) : 0u;
}

uint32_t aos_blk_queue_resp_length(const aos_blk_resp_queue_t *q)
{
    return q ? resp_len(q) : 0u;
}

int aos_blk_queue_req_valid(const aos_blk_req_queue_t *q, uint32_t capacity)
{
    return q && capacity != 0u && capacity <= AOS_BLK_QUEUE_CAPACITY &&
           req_len(q) <= capacity;
}

int aos_blk_queue_resp_valid(const aos_blk_resp_queue_t *q, uint32_t capacity)
{
    return q && capacity != 0u && capacity <= AOS_BLK_QUEUE_CAPACITY &&
           resp_len(q) <= capacity;
}

static void reset_req_queue(aos_blk_req_queue_t *q)
{
    uint32_t tail = *(volatile uint32_t *)&q->tail;

    aos_fence();
    *(volatile uint32_t *)&q->head = tail;
    aos_fence();
}

static void reset_resp_queue(aos_blk_resp_queue_t *q)
{
    uint32_t tail = *(volatile uint32_t *)&q->tail;

    aos_fence();
    *(volatile uint32_t *)&q->head = tail;
    aos_fence();
}

static int req_empty(const aos_blk_req_queue_t *q)
{
    return req_len(q) == 0u;
}

static int resp_full(const aos_blk_resp_queue_t *q, uint32_t capacity)
{
    return resp_len(q) >= capacity;
}

static int dequeue_req(aos_blk_req_queue_t *q, uint32_t capacity, aos_blk_req_t *out)
{
    if (!aos_blk_queue_req_valid(q, capacity) || req_empty(q)) {
        return -1;
    }
    aos_fence();
    *out = q->buffers[*(volatile uint32_t *)&q->head % capacity];
    aos_fence();
    (*(volatile uint32_t *)&q->head)++;
    return 0;
}

static int enqueue_resp(aos_blk_resp_queue_t *q, uint32_t capacity, aos_blk_resp_t r)
{
    if (!aos_blk_queue_resp_valid(q, capacity) || resp_full(q, capacity)) {
        return -1;
    }
    q->buffers[*(volatile uint32_t *)&q->tail % capacity] = r;
    aos_fence();
    (*(volatile uint32_t *)&q->tail)++;
    aos_fence();
    return 0;
}

void aos_blk_virt_reset(aos_blk_virt_t *v)
{
    if (!v) {
        return;
    }
    aos_bzero(v, (uint32_t)sizeof(*v));
}

bool aos_blk_virt_detach(aos_blk_virt_t *v)
{
    if (!v || v->num_clients > AOS_BLK_MAX_CLIENTS) return false;
    for (uint32_t i = 0; i < v->num_clients; i++) {
        const aos_blk_virt_client_t *c = &v->clients[i];
        if (!aos_blk_queue_req_valid(c->req, c->capacity) ||
            !aos_blk_queue_resp_valid(c->resp, c->capacity) ||
            req_len(c->req) != 0 || resp_len(c->resp) != 0) return false;
    }
    aos_blk_virt_reset(v);
    return true;
}

void aos_blk_storage_init(aos_blk_storage_info_t *info, uint32_t disk_blocks)
{
    static const char serial[] = "aos-ram0";
    uint32_t i;

    if (!info) {
        return;
    }
    aos_bzero(info, (uint32_t)sizeof(*info));
    for (i = 0; serial[i] != 0 && i < AOS_BLK_MAX_SERIAL; i++) {
        info->serial_number[i] = serial[i];
    }
    info->read_only = false;
    info->sector_size = (uint16_t)AOS_BLK_SECTOR_SIZE;
    info->block_size = 1u;
    info->queue_depth = (uint16_t)AOS_BLK_QUEUE_CAPACITY;
    info->capacity = (uint64_t)disk_blocks;
    info->ready = true;
}

int aos_blk_virt_add_client(aos_blk_virt_t *v, const aos_blk_virt_client_t *c)
{
    if (!v || !c || !c->info || !c->req || !c->resp || !c->data ||
        c->capacity == 0u || c->capacity > AOS_BLK_QUEUE_CAPACITY ||
        c->media_blocks == 0u || v->num_clients >= AOS_BLK_MAX_CLIENTS) {
        return -1;
    }
    v->clients[v->num_clients] = *c;
    v->num_clients++;
    return 0;
}

void aos_blk_client_set_media(aos_blk_virt_client_t *c, uint32_t media_blocks)
{
    if (!c) {
        return;
    }
    c->media_blocks = media_blocks;
}

void aos_blk_client_set_ram_disk(aos_blk_virt_client_t *c, uint8_t *disk,
                                 uint32_t disk_blocks)
{
    if (!c) {
        return;
    }
    c->disk = disk;
    c->disk_blocks = disk_blocks;
    c->media_blocks = disk_blocks;
}

void aos_blk_virt_set_backend(aos_blk_virt_t *v, aos_blk_backend_fn backend,
                              void *ctx)
{
    if (!v) {
        return;
    }
    v->backend = backend;
    v->backend_ctx = ctx;
}

static int request_valid(const aos_blk_virt_client_t *c, const aos_blk_req_t *req)
{
    uint32_t nbytes;

    switch (req->code) {
    case AOS_BLK_REQ_READ:
    case AOS_BLK_REQ_WRITE:
        if (req->count == 0u || c->media_blocks == 0u ||
            req->block_number > c->media_blocks ||
            req->count > c->media_blocks - req->block_number) {
            return 0;
        }
        nbytes = (uint32_t)req->count * AOS_BLK_TRANSFER_SIZE;
        return req->io_or_offset <= AOS_BLK_DATA_BYTES &&
               nbytes <= AOS_BLK_DATA_BYTES - req->io_or_offset;
    case AOS_BLK_REQ_FLUSH:
    case AOS_BLK_REQ_BARRIER:
        return req->io_or_offset == 0u && req->block_number == 0u &&
               req->count == 0u;
    default:
        /* Queue ABI v4 has no DISCARD request.  Do not pass a future
         * discard-shaped code to a backend that cannot implement it. */
        return 0;
    }
}

static aos_blk_resp_status_t serve_rw(aos_blk_virt_client_t *c,
                                      const aos_blk_req_t *req, int writing)
{
    uint32_t nbytes = (uint32_t)req->count * AOS_BLK_TRANSFER_SIZE;
    uint64_t disk_offset = req->block_number * AOS_BLK_TRANSFER_SIZE;

    if (!c->disk || c->disk_blocks < c->media_blocks) {
        return AOS_BLK_RESP_ERR_NO_DEVICE;
    }

    if (writing) {
        aos_copy(c->disk + disk_offset, c->data + (uint32_t)req->io_or_offset,
                 nbytes);
    } else {
        aos_copy(c->data + (uint32_t)req->io_or_offset, c->disk + disk_offset,
                 nbytes);
    }
    return AOS_BLK_RESP_OK;
}

uint32_t aos_blk_virt_pump(aos_blk_virt_t *v)
{
    uint32_t completed = 0;
    uint32_t i;

    if (!v || v->num_clients == 0u) {
        return 0;
    }

    for (i = 0; i < v->num_clients; i++) {
        aos_blk_virt_client_t *c = &v->clients[i];
        uint32_t budget;

        if (!c->info || !c->req || !c->resp || !c->data ||
            c->capacity == 0u || c->capacity > AOS_BLK_QUEUE_CAPACITY) {
            continue;
        }
        if (!aos_blk_queue_resp_valid(c->resp, c->capacity)) {
            reset_resp_queue(c->resp);
            continue;
        }
        if (!aos_blk_queue_req_valid(c->req, c->capacity)) {
            reset_req_queue(c->req);
            continue;
        }

        budget = c->capacity;
        while (budget-- > 0u && !req_empty(c->req)) {
            aos_blk_req_t req;
            aos_blk_resp_t resp;

            if (resp_full(c->resp, c->capacity)) {
                break;
            }
            if (dequeue_req(c->req, c->capacity, &req) != 0) {
                break;
            }

            resp.id = req.id;
            resp.success_count = 0;
            resp.status = AOS_BLK_RESP_ERR_UNSPEC;

            if (!request_valid(c, &req)) {
                resp.status = AOS_BLK_RESP_ERR_INVALID_PARAM;
            } else if (req.code == AOS_BLK_REQ_WRITE && c->info->read_only) {
                resp.status = AOS_BLK_RESP_ERR_IO;
            } else if (v->backend) {
                resp.status = v->backend(v->backend_ctx, c, &req);
                if (resp.status == AOS_BLK_RESP_OK &&
                    (req.code == AOS_BLK_REQ_READ ||
                     req.code == AOS_BLK_REQ_WRITE)) {
                    resp.success_count = req.count;
                }
            } else {
                switch (req.code) {
                case AOS_BLK_REQ_READ:
                    resp.status = serve_rw(c, &req, 0);
                    if (resp.status == AOS_BLK_RESP_OK) {
                        resp.success_count = req.count;
                    }
                    break;
                case AOS_BLK_REQ_WRITE:
                    resp.status = serve_rw(c, &req, 1);
                    if (resp.status == AOS_BLK_RESP_OK) {
                        resp.success_count = req.count;
                    }
                    break;
                case AOS_BLK_REQ_FLUSH:
                case AOS_BLK_REQ_BARRIER:
                    /* RAM flush is an ordering acknowledgement, not durability. */
                    resp.status = AOS_BLK_RESP_OK;
                    break;
                default:
                    resp.status = AOS_BLK_RESP_ERR_INVALID_PARAM;
                    break;
                }
            }

            if (enqueue_resp(c->resp, c->capacity, resp) != 0) {
                break;
            }
            completed++;
        }
    }

    return completed;
}
