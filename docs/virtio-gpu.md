# Guest virtio-gpu backend

Graphics profiles can require exact captured pixels with a `host.test` action
`assert-frame-pixels`. Its `x` and `y` string arguments identify the first
pixel, and `rgb` contains 1 through 16 contiguous RGB hex triples in that row.
Coordinates and colors are validated before launch; frame dimensions and
every expected color are checked after immutable capture. The Debian GPU
profile requires the two colors written by its fb0 console probe at (0, 200),
and the combined graphics/input profile inherits that assertion. A wrong
pixel fails qualification even when the image is otherwise nonblack.
Successful frame receipts include the number of asserted pixels.

This work implements the unaccelerated 2D command engine and libvmm MMIO
registration entry point for the v0.4 guest graphics path. The protocol is
[VirtIO 1.2 section 5.7](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).
The `GUEST_GRAPHICS=1` AArch64 variant adds root queue grants and VMM
initialization. A GPU device flag in the guest profile adds the faulting DTB
window at `0x0a040000` with virtual INTID 54. `make test-guest-gpu` selects the
inherited Debian GPU profile, enables this variant and requires a successful
`modprobe virtio_gpu`, `/dev/dri/card0`, a write through `/dev/fb0`, and a
nonblack frame captured through CC before its authenticated SSH proof.
This boot qualification passed on Spark at `8fb8ba1`, including DRM/fb0,
the exact two profile-written pixels in a 1024 by 768 CC capture, and
authenticated SSH. The full OS gate also passed at that revision.
[The receipt](evidence/2026-09-17-spark/graphics-base-integration.json)
retains artifact identities and scope; physical scanout, combined input and
final release qualification remain separate requirements.
The MAC task remains `task_cefc0f77327d4245ab9feb132cd1eb57`.

The GPU profile retains Linux boot messages and bounded command/response logs
for qualification. The common MMIO transport reasserts uncleared interrupt
status after virtual-GIC acknowledgement: a completion arriving between the
driver's InterruptACK and GIC EOI must not lose its notification when vGIC
coalesces a pending interrupt. Host regression covers this ordering, partial
acknowledgement and stopping reinjection after all status bits are cleared.

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
packed through a fixed private 64 KiB buffer to the framebuffer request queue,
retaining the backing resource's row stride. A full 1024 by 768 transfer uses
48 queue writes rather than 768 individual row exchanges. The framebuffer PD
continues bounded pump passes while work exists and waits when idle; it does
not forfeit its MCS budget after each response. The kernel's scheduling budget
still bounds its CPU use. The full-frame host test checks every committed byte
and the bounded number of queue transactions.
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
client selection. The graphics image maps a separate observer page into CC
and the service, with a private snapshot arena mapped only into the service.
`MSG_CC_FRAME_CAPTURE` resolves public guest handles and relays capture/read/
release operations over that page.

At `1aee4b8`, the framebuffer service also pumps one observer request during
each display-response wait iteration. Guest client queues remain quiescent
during forwarding, so neither surface mutation nor detach can invalidate the
committed source. The observer continues using its separate immutable snapshot
and existing response backpressure.

The [September 19 Spark receipt](evidence/2026-09-19-spark/frame-observer-progress.json)
records the full OS gate, native display test, and qualified Debian graphics
guest. All 786,432 guest-frame pixels matched QEMU scanout. Alternating the
unchanged GUI client's 128-region-read benchmark between two retained guests
took 3.57/3.41 seconds on the candidate and 5.79 seconds on its baseline.
Sequential full-frame `agentctl` exports took 18.68 and 36.68 seconds,
respectively. These measurements show a partial throughput improvement;
170–180 ms p95 region-read delays persist. They do not establish interactive
desktop, native GUI rendering, or input-response acceptance.

The [CC refill follow-up](evidence/2026-09-19-spark/cc-refill-cadence.json)
qualifies runtime `3999e6d` with a 100 us CC budget per 1 ms period, retaining
the 10% CPU ceiling. The full OS gate and Debian graphics qualification passed;
the harness's complete pixel-checked capture took 3 seconds rather than 12.
Alternating production-client measurements took 1.58/1.46 seconds for 128
region reads versus 3.27 seconds on the preceding runtime. Status means fell
from 20.10 ms to 3.41/3.10 ms, and region-read p95 fell from 173 ms to
12–14 ms. Occasional 194–216 ms reads remain. This does not yet establish
interactive frame refresh or input-response acceptance; the retry limits also
remain iteration counts, not guarantees of identical elapsed recovery time.

`agentctl frame-capture GUEST_HANDLE OUTPUT.ppm` exports one coherent PPM
image over the existing privileged CC socket and prints JSON metadata. It
refuses to overwrite an existing file. The guest GPU test retains `.frame.ppm`
and `.frame.json` beside its serial log, including the captured artifact's
SHA-256. `make test-agentctl-frame-host` exercises the real CLI against the
observer service over CC frames: exact RGB conversion across chunks, snapshot
stability after live pixels change, malformed replies and existing-file
preservation. These host results do not substitute for the guest test.

The focused `make test-framebuffer` image also exports exact 40 by 40 images
from both native clients through the real observer service and CC transport.
Only this test image recognizes native capture handles `0xfb000000` and
`0xfb000001`; ordinary guest handles are rejected in that image. The isolation
matrix additionally probes both native clients' read/write access to the
observer page and private snapshot arena. The focused capture test passed on
Spark, as did all sixteen mapping-isolation cases at `3b00d93`. This native proof does
not establish guest DRM or physical scanout.

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
visible cursor. A physical driver remains required. No VMM receives a physical
device frame or IRQ.

`make test-virtio-gpu-host` exercises the command engine, direct ring handling,
the queue adapter and the actual framebuffer service together. It asserts
exact pixels, noncontiguous backing, transfer/flush ordering, cursor shape
commit, fences, queue wrap, malformed requests, capacity and resource reset.
`make test-host` includes this suite.
`make build GUEST_OS=debian-gpu GUEST_GRAPHICS=1` compiles the graphics variant
for AArch64. Compilation and host tests do not prove guest DRM enumeration or
physical scanout; that requires runtime evidence.
