/*
 * Emulated-virtio GPA copy helpers. Translation is a hook so platform/
 * can bounds-check against guest RAM without forking every virtio device.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <libvmm/virtio/virtq.h>
#include <libvmm/virtio/gpa.h>

static void *virtio_gpa_identity(uint64_t gpa, size_t len)
{
    if (len != 0u && (gpa + (uint64_t)len) < gpa) {
        return NULL;
    }
    return (void *)(uintptr_t)gpa;
}

static virtio_gpa_translate_fn g_translate = virtio_gpa_identity;

void virtio_gpa_set_translate(virtio_gpa_translate_fn fn)
{
    g_translate = (fn != NULL) ? fn : virtio_gpa_identity;
}

void *virtio_gpa_to_hva(uint64_t gpa, size_t len)
{
    return g_translate(gpa, len);
}

static int gpa_add_off(uint64_t gpa, size_t off, uint64_t *out)
{
    if (off != 0u && gpa > (UINT64_MAX - (uint64_t)off)) {
        return -1;
    }
    *out = gpa + (uint64_t)off;
    return 0;
}

int virtio_copy_from_gpa(uint64_t gpa, size_t off, void *dst, size_t len)
{
    uint64_t start;
    const void *src;

    if (len == 0u) {
        return 0;
    }
    if (dst == NULL || gpa_add_off(gpa, off, &start) != 0) {
        return -1;
    }
    src = virtio_gpa_to_hva(start, len);
    if (src == NULL) {
        return -1;
    }
    memcpy(dst, src, len);
    return 0;
}

int virtio_copy_to_gpa(uint64_t gpa, size_t off, const void *src, size_t len)
{
    uint64_t start;
    void *dst;

    if (len == 0u) {
        return 0;
    }
    if (src == NULL || gpa_add_off(gpa, off, &start) != 0) {
        return -1;
    }
    dst = virtio_gpa_to_hva(start, len);
    if (dst == NULL) {
        return -1;
    }
    memcpy(dst, src, len);
    return 0;
}

bool virtio_queue_map_guest_rings(struct virtq *virtq)
{
    uint64_t desc_gpa;
    uint64_t avail_gpa;
    uint64_t used_gpa;
    void *desc;
    void *avail;
    void *used;
    size_t desc_len;
    size_t avail_len;
    size_t used_len;

    if (virtq == NULL || virtq->num == 0u || virtq->num > 32768u) {
        return false;
    }

    desc_gpa = (uint64_t)(uintptr_t)virtq->desc;
    avail_gpa = (uint64_t)(uintptr_t)virtq->avail;
    used_gpa = (uint64_t)(uintptr_t)virtq->used;

    desc_len = (size_t)virtq->num * sizeof(struct virtq_desc);
    /* flags + idx + ring[num] + used_event */
    avail_len = sizeof(uint16_t) * (3u + (size_t)virtq->num);
    /* flags + idx + used_event + used_elem[num] */
    used_len = sizeof(uint16_t) * 3u
               + sizeof(struct virtq_used_elem) * (size_t)virtq->num;

    desc = virtio_gpa_to_hva(desc_gpa, desc_len);
    avail = virtio_gpa_to_hva(avail_gpa, avail_len);
    used = virtio_gpa_to_hva(used_gpa, used_len);
    if (desc == NULL || avail == NULL || used == NULL) {
        return false;
    }

    virtq->desc = (struct virtq_desc *)desc;
    virtq->avail = (struct virtq_avail *)avail;
    virtq->used = (struct virtq_used *)used;
    return true;
}
