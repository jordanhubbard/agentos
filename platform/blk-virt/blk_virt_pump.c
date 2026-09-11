/*
 * blk_virt pump — sDDF-shaped block request pump with no seL4 dependency.
 *
 * Each client posts READ/WRITE/FLUSH/BARRIER on its request queue.  The pump
 * serves them from a backend callback (blk_virt: the virtio_blk driver) or
 * from a RAM image, and posts responses.  The rings are shared between PDs:
 * indices are read as volatile and every hand-over is fenced.
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

static int req_empty(const aos_blk_req_queue_t *q)
{
    return req_len(q) == 0u;
}

static int resp_full(const aos_blk_resp_queue_t *q, uint32_t capacity)
{
    return resp_len(q) == capacity;
}

static int dequeue_req(aos_blk_req_queue_t *q, uint32_t capacity, aos_blk_req_t *out)
{
    if (req_empty(q)) {
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
    if (resp_full(q, capacity)) {
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
    if (!v || !c || v->num_clients >= AOS_BLK_MAX_CLIENTS) {
        return -1;
    }
    v->clients[v->num_clients] = *c;
    v->num_clients++;
    return 0;
}

void aos_blk_virt_set_disk(aos_blk_virt_t *v, uint8_t *disk, uint32_t disk_blocks)
{
    if (!v) {
        return;
    }
    v->disk = disk;
    v->disk_blocks = disk_blocks;
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

static aos_blk_resp_status_t serve_rw(aos_blk_virt_t *v, aos_blk_virt_client_t *c,
                                      const aos_blk_req_t *req, int writing)
{
    uint32_t nbytes;
    uint64_t data_end;

    if (!v->disk || req->count == 0u) {
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }
    if ((uint64_t)req->block_number + (uint64_t)req->count > (uint64_t)v->disk_blocks) {
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }
    nbytes = (uint32_t)req->count * AOS_BLK_TRANSFER_SIZE;
    data_end = req->io_or_offset + (uint64_t)nbytes;
    if (data_end > (uint64_t)AOS_BLK_DATA_BYTES) {
        return AOS_BLK_RESP_ERR_INVALID_PARAM;
    }

    if (writing) {
        aos_copy(v->disk + ((uint32_t)req->block_number * AOS_BLK_TRANSFER_SIZE),
                 c->data + (uint32_t)req->io_or_offset, nbytes);
    } else {
        aos_copy(c->data + (uint32_t)req->io_or_offset,
                 v->disk + ((uint32_t)req->block_number * AOS_BLK_TRANSFER_SIZE), nbytes);
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

        while (!req_empty(c->req)) {
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

            if (v->backend) {
                resp.status = v->backend(v->backend_ctx, c, &req);
                if (resp.status == AOS_BLK_RESP_OK &&
                    (req.code == AOS_BLK_REQ_READ ||
                     req.code == AOS_BLK_REQ_WRITE)) {
                    resp.success_count = req.count;
                }
                if (enqueue_resp(c->resp, c->capacity, resp) != 0) {
                    break;
                }
                completed++;
                continue;
            }

            switch (req.code) {
            case AOS_BLK_REQ_READ:
                resp.status = serve_rw(v, c, &req, 0);
                if (resp.status == AOS_BLK_RESP_OK) {
                    resp.success_count = req.count;
                }
                break;
            case AOS_BLK_REQ_WRITE:
                resp.status = serve_rw(v, c, &req, 1);
                if (resp.status == AOS_BLK_RESP_OK) {
                    resp.success_count = req.count;
                }
                break;
            case AOS_BLK_REQ_FLUSH:
            case AOS_BLK_REQ_BARRIER:
                resp.status = AOS_BLK_RESP_OK;
                break;
            default:
                resp.status = AOS_BLK_RESP_ERR_INVALID_PARAM;
                break;
            }

            if (enqueue_resp(c->resp, c->capacity, resp) != 0) {
                break;
            }
            completed++;
        }
    }

    return completed;
}
