# ipc_bridge_daemon

The Linux-side counterpart to the agentOS seL4 IPC bridge: it maps the
shared-memory command/response rings (physical address `0x4000000`) via
`/dev/mem`, polls for commands enqueued by native seL4 protection domains,
executes them (ping, file read/write, shell exec, process spawn/signal), and
writes responses back into the ring.

## Build

```sh
make
# Cross-compile for AArch64:
make CC=aarch64-linux-gnu-gcc
```

## Running inside the Linux guest

```sh
# Default physical address and size (must be root):
sudo ./ipc_bridge_daemon

# Override if the seL4 MR is mapped elsewhere:
sudo ./ipc_bridge_daemon --shmem-phys 0x4000000 --shmem-size 0x10000

# Run as a background service (logs go to the journal):
sudo ./ipc_bridge_daemon &
```
