# Guest virtio-gpu backend

This work implements the unaccelerated 2D command engine and libvmm MMIO
registration entry point for the v0.4 guest graphics path. The protocol is
[VirtIO 1.2 section 5.7](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).
The `GUEST_GRAPHICS=1` AArch64 variant adds root queue grants and VMM
initialization. A GPU device flag in the guest profile adds the faulting DTB
window at `0x0a040000` with virtual INTID 54. `make test-guest-gpu` selects the
inherited Debian GPU profile, enables this variant and requires a successful
`modprobe virtio_gpu` plus `/dev/dri/card0` before its authenticated SSH proof.
This boot qualification is in progress; captured guest frames are not yet proven.
The MAC task remains `task_cefc0f77327d4245ab9feb132cd1eb57`.

`virtio_gpu_2d_execute` consumes a private command snapshot. It supports
display information, resource create/unref, attach/detach backing, rectangular
transfers, scanout selection and flush. The initial bounds are four resources,
1024 backing entries per resource and a 1024 by 768 display. B8G8R8A8 and
B8G8R8X8 resources use four-byte pixels; acceleration, blobs, capsets, EDID and
indirect descriptors are not advertised. The cursor queue selects or moves a
64 by 64 cursor surface. Cursor shape selection commits its transferred
pixels without requiring the guest to issue a resource flush.

The command engine retains guest resource IDs separately from private
framebuffer handles. Backing lists are validated before replacement and copied
into private state. Transfers read scatter lists through the platform GPA
translator; backing addresses are never treated as host pointers. Rows are
copied through a fixed private buffer to the framebuffer request queue.
Completion fences are echoed only after synchronous backend completion.
Backend failures return errors and resource handles remain available for
cleanup retries. Reset releases backend resources rather than merely clearing
the resource table.

The framebuffer service now retains validated scanout selection, and
`framebuffer_observer.h` defines a separate read-only capture queue. A capture
copies the selected committed rectangle into observer-private storage and
returns a fresh cookie; bounded reads retain a coherent image across later
flips and surface destruction. A private client mask limits which guests an
observer can capture. The pump permits only one outstanding response so read
payloads cannot be overwritten before consumption. Host tests cover a complete
1024 by 768 image, crop selection, chunk ownership, stale cookies and denied
client selection. Observer root grants and CC/external API integration remain
unfinished; this is not yet a target capture proof.

`virtio_gpu_control_run` and `virtio_gpu_cursor_run` consume direct split
virtqueues. They snapshot descriptors, bound chains and request sizes, require
read buffers before write buffers, and validate writable response space before
executing a control command. Ring occupancy and indices use fixed private
limits. Malformed chains stop the queue until reset. The MMIO adapter reports
`DEVICE_NEEDS_RESET` and injects the configured virtual interrupt on failure.

`aos_gpu_framebuffer_t` requires a serialized queue exchange callback. It must
retain payload ownership until the matching response returns, using persistent
notifications on target. Scanout and cursor selections are private virtual
display metadata; they do not operate physical GPU hardware or compose a
visible cursor. A physical driver and an authorized external export path still
need to consume that state. No VMM receives a physical device frame or IRQ.

`make test-virtio-gpu-host` exercises the command engine, direct ring handling,
the queue adapter and the actual framebuffer service together. It asserts
exact pixels, noncontiguous backing, transfer/flush ordering, cursor shape
commit, fences, queue wrap, malformed requests, capacity and resource reset.
`make test-host` includes this suite.
`make build GUEST_OS=debian-gpu GUEST_GRAPHICS=1` compiles the graphics variant
for AArch64. Compilation and host tests do not prove guest DRM enumeration or
physical scanout; that requires runtime evidence.
