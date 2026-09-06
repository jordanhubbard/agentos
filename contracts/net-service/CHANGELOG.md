# NetService contract changelog

## Version 2

- Document the established raw-device `MSG_NET_*` operation values.
- Add the shared-memory offset to the raw open reply.
- Add the one-way `NET_SVC_EVENT_RX_READY` event for asynchronous host RX.
- Reserve the lower half of the 2 MiB shared frame for guest-facing queues and
  place per-handle contract slots at offset `0x100000`.
- Specify that host transport MMIO and DMA are exclusively owned by `net_pd`.

## Version 1

- Initial vNIC and ACL operation table.
