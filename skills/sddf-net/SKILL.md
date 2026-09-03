# sDDF net

Network I/O in agentOS is sDDF-shaped: driver owns the NIC (or QEMU virtio-net
as a stand-in), `net_virt` muxes, clients hold queue caps.

## Invariants

- NIC MMIO/IRQ belong to the driver PD only.
- Clients (VMM virtio backends, native stacks) use free/active queue pairs.
- Buffers are offsets into a data region, not raw pointers across PDs.
- Guests do **not** DMA to the host NIC. The VMM emulates virtio-net.

## Forbidden

- `MSG_NET_SEND` of a full frame through IPC registers as the data path.
- Mapping QEMU virtio-net MMIO into a guest.
- Per-guest NICs that split QEMU buses instead of sharing `net_virt`.

## Helper

```sh
python3 skills/sddf-net/scripts/queue_layout.py
```
