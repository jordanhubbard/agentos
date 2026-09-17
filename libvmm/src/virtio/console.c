/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/vmm_caps.h>
#include <libvmm/guest.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/mmio.h>
#include <libvmm/virtio/console.h>
#include <libvmm/virtio/console_tx_ring.h>
#include <libvmm/virtio/gpa.h>
#include <sddf/serial/queue.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_CONSOLE

#if defined(DEBUG_CONSOLE)
#define LOG_CONSOLE(...) do{ printf("VIRTIO(CONSOLE): "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_CONSOLE(...) do{}while(0)
#endif

#define LOG_CONSOLE_ERR(...) do{ printf("VIRTIO(CONSOLE)|ERROR: "); printf(__VA_ARGS__); }while(0)

static inline struct virtio_console_device *device_state(struct virtio_device *dev)
{
    return (struct virtio_console_device *)dev->device_data;
}

static void virtio_console_features_print(uint32_t features)
{
    /* Dump the features given in a human-readable format */
    LOG_CONSOLE("Dumping features (0x%lx):\n", features);
    LOG_CONSOLE("feature VIRTIO_CONSOLE_F_SIZE set to %s\n",
                BIT_LOW(VIRTIO_CONSOLE_F_SIZE) & features ? "true" : "false");
    LOG_CONSOLE("feature VIRTIO_CONSOLE_F_MULTIPORT set to %s\n",
                BIT_LOW(VIRTIO_CONSOLE_F_MULTIPORT) & features ? "true" : "false");
    LOG_CONSOLE("feature VIRTIO_CONSOLE_F_EMERG_WRITE set to %s\n",
                BIT_LOW(VIRTIO_CONSOLE_F_EMERG_WRITE) & features ? "true" : "false");
}

static void virtio_console_reset(struct virtio_device *dev)
{
    device_state(dev)->tx_progress = (virtio_console_tx_state_t){0};
    device_state(dev)->rx_progress = (virtio_console_rx_state_t){0};
    device_state(dev)->tx_backpressure_reported = false;
    LOG_CONSOLE("operation: reset device\n");

    for (int i = 0; i < dev->num_vqs; i++) {
        virtio_queue_reset_guest_rings(&dev->vqs[i]);
    }
}

static bool virtio_console_get_device_features(struct virtio_device *dev, uint32_t *features)
{
    LOG_CONSOLE("operation: get device features\n");

    switch (dev->regs.DeviceFeaturesSel) {
    case 0:
        *features = 0;
        break;
    case 1:
        *features = BIT_HIGH(VIRTIO_F_VERSION_1);
        break;
    default:
        LOG_CONSOLE_ERR("driver sets DeviceFeaturesSel to 0x%x, which doesn't make sense\n", dev->regs.DeviceFeaturesSel);
        return false;
    }

    return true;
}

static bool virtio_console_set_driver_features(struct virtio_device *dev, uint32_t features)
{
    LOG_CONSOLE("operation: set driver features\n");
    virtio_console_features_print(features);

    bool success = false;

    switch (dev->regs.DriverFeaturesSel) {
    // feature bits 0 to 31
    case 0:
        /* We do not offer any features in the first 32-bit bits */
        success = (features == 0);
        break;
    // features bits 32 to 63
    case 1:
        success = (features == BIT_HIGH(VIRTIO_F_VERSION_1));
        break;
    default:
        LOG_CONSOLE_ERR("driver sets DriverFeaturesSel to 0x%x, which doesn't make sense\n", dev->regs.DriverFeaturesSel);
        return false;
    }

    if (success) {
        dev->features_happy = 1;
        LOG_CONSOLE("device is feature happy\n");
    }

    return success;
}

static bool virtio_console_get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *config)
{
    LOG_CONSOLE("operation: get device config\n");
    return false;
}

static bool virtio_console_set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t config)
{
    LOG_CONSOLE("operation: set device config\n");
    return false;
}

static uint32_t console_tx_copy(void *ctx, uint64_t address, uint32_t offset, uint32_t length)
{
    struct virtio_console_device *console = ctx;
    serial_queue_handle_t *queue = console->txq;
    uint32_t free = serial_queue_contiguous_free(queue);
    uint32_t count = length < free ? length : free;
    if (!count) return 0;
    if (virtio_copy_from_gpa(address, offset,
            queue->data_region + queue->queue->tail % queue->capacity, count) != 0)
        return UINT32_MAX;
    serial_update_shared_tail(queue, queue->queue->tail + count);
    return count;
}

bool virtio_console_handle_pending_tx(struct virtio_console_device *console)
{
    struct virtio_device *dev = &console->virtio_device;
    struct virtio_queue_handler *vq = &console->vqs[TX_QUEUE];
    if (!vq->ready) return true;
    if (console->tx_progress.failed) return false;
    virtio_console_tx_ring_result_t result = virtio_console_tx_ring_run(
        &vq->virtq, QUEUE_SIZE, &vq->last_idx, &console->tx_head,
        &console->tx_progress, console->txq->capacity, console_tx_copy, console);
    if (!result.valid) {
        LOG_CONSOLE_ERR("invalid transmit descriptor chain or ring\n");
        return false;
    }
    if (console->tx_progress.active && !console->tx_backpressure_reported &&
        serial_queue_full(console->txq, console->txq->queue->tail)) {
        printf("VIRTIO(CONSOLE): TX backpressure retained pending descriptor\n");
        console->tx_backpressure_reported = true;
    }
    if (result.bytes && console->tx_cap) vmm_notify(console->tx_cap);
    if (result.completed) {
        dev->regs.InterruptStatus |= BIT_LOW(0);
        return virq_inject(dev->virq);
    }
    return true;
}

static bool virtio_console_handle_tx(struct virtio_device *dev)
{
    return virtio_console_handle_pending_tx(device_state(dev));
}

static void *console_rx_map(void *context, uint64_t address, uint32_t length)
{
    (void)context;
    return virtio_gpa_to_hva(address,length);
}

static void console_rx_take(void *context, void *destination, uint32_t length)
{
    serial_queue_handle_t *queue=context;
    uint8_t *out=destination;
    /* Input count was snapshotted; this is the sole consumer. A concurrent
     * producer can only append, so every dequeue in this bounded copy exists. */
    for (uint32_t i=0;i<length;i++) {
        char byte=0;
        (void)serial_dequeue(queue,&byte);
        out[i]=(uint8_t)byte;
    }
}

bool virtio_console_handle_rx(struct virtio_console_device *console)
{
    struct virtio_queue_handler *vq=&console->vqs[RX_QUEUE];
    if (!vq->ready) return true; /* retain input until buffers are available */
    uint32_t input=serial_queue_length_consumer(console->rxq);
    if (input>console->rxq->capacity) {
        console->rx_progress.failed=true;
        return false;
    }
    const virtio_console_rx_ops_t ops={console_rx_map,console_rx_take,console->rxq};
    virtio_console_rx_result_t result=virtio_console_rx_ring_run(
        &vq->virtq,&vq->last_idx,&console->rx_progress,input,&ops);
    bool irq=true;
    if (result.completed) {
        console->virtio_device.regs.InterruptStatus |= BIT_LOW(0);
        irq=virq_inject(console->virtio_device.virq);
    }
    if (!result.valid) LOG_CONSOLE_ERR("invalid receive descriptor chain or ring\n");
    return result.valid && irq;
}

virtio_device_funs_t functions = {
    .device_reset = virtio_console_reset,
    .get_device_features = virtio_console_get_device_features,
    .set_driver_features = virtio_console_set_driver_features,
    .get_device_config = virtio_console_get_device_config,
    .set_device_config = virtio_console_set_device_config,
    .queue_notify = virtio_console_handle_tx,
};

static struct virtio_device *virtio_console_init(struct virtio_console_device *console, virtio_transport_type_t type,
                                                 size_t virq, serial_queue_handle_t *rxq, serial_queue_handle_t *txq,
                                                 seL4_CPtr tx_cap)
{
    struct virtio_device *dev = &console->virtio_device;
    dev->regs.DeviceID = VIRTIO_DEVICE_ID_CONSOLE;
    dev->regs.VendorID = VIRTIO_MMIO_DEV_VENDOR_ID;
    dev->transport_type = type;
    dev->funs = &functions;
    dev->vqs = console->vqs;
    dev->num_vqs = VIRTIO_CONSOLE_NUM_VIRTQ;
    dev->virq = virq;
    dev->device_data = console;

    console->rxq = rxq;
    console->txq = txq;
    console->tx_cap = tx_cap;
    console->tx_progress = (virtio_console_tx_state_t){0};
    console->rx_progress = (virtio_console_rx_state_t){0};
    console->tx_backpressure_reported = false;

    return dev;
}

bool virtio_mmio_console_init(struct virtio_console_device *console, uintptr_t region_base, uintptr_t region_size,
                              size_t virq, serial_queue_handle_t *rxq, serial_queue_handle_t *txq, seL4_CPtr tx_cap)
{
    struct virtio_device *dev = virtio_console_init(console, VIRTIO_TRANSPORT_MMIO, virq, rxq, txq, tx_cap);

    return virtio_mmio_register_device(dev, region_base, region_size, virq);
}

bool virtio_pci_console_init(struct virtio_console_device *console, uint32_t dev_slot, size_t virq,
                             serial_queue_handle_t *rxq, serial_queue_handle_t *txq, seL4_CPtr tx_cap)
{
    struct virtio_device *dev = virtio_console_init(console, VIRTIO_TRANSPORT_PCI, virq, rxq, txq, tx_cap);

    dev->transport.pci.device_id = VIRTIO_PCI_CONSOLE_DEV_ID;
    dev->transport.pci.vendor_id = VIRTIO_PCI_VENDOR_ID;
    dev->transport.pci.device_class = PCI_CLASS_COMMUNICATION_OTHER;

    bool success = virtio_pci_alloc_dev_cfg_space(dev, dev_slot);
    assert(success);

    virtio_pci_alloc_memory_bar(dev, 0, VIRTIO_PCI_DEFAULT_BAR_SIZE);

    return virtio_pci_register_device(dev, virq);
}
