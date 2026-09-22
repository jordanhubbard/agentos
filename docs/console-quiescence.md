# Guest console quiescence

After stopping guest vCPUs, call `aos_vmm_virtio_console_quiesce()` before
reclaiming guest RAM. It immediately rejects new input and queue notifications,
but returns false while published TX descriptors still reference guest memory.
Continue calling `aos_vmm_virtio_console_drain_tx()` and retry quiescence so
backpressured output can finish copying into the bounded device-local FIFO.
Invalid descriptors keep quiescence unsuccessful; callers must not reclaim RAM
on that path.

Success retires the guest rings. Copied output remains available to drain
without accessing guest RAM. Repeated calls succeed without touching those
rings, and guest reset cannot reopen queue processing. Reset during the drain
does not discard a retained descriptor. Calling quiescence before initialization
is a no-op. This does not detach serial_virt, flush its shared queues to an
observer, or provide a reusable guest slot.

`make test-x86-console-host` exercises the actual console backend through the
x86 MMIO/IOAPIC adapter. A transmit larger than the 64 KiB local FIFO stays
pending across quiescence and reset. The test drains it and verifies every byte,
then marks all guest RAM `PROT_NONE` before draining the copied remainder and
delivering late input, faults and queue notifications. Guest feature/queue
renegotiation cannot undo quiescence. This complements the existing exact
console RX/TX and backpressure tests.

Production lifecycle wiring and target destroy/recreate qualification remain
outstanding. The host result is a device-lifetime prerequisite, not complete
resource reclamation evidence.
