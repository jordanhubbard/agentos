# Graphics and input quiescence

After stopping vCPUs and returning from the current synchronous device
callback, `aos_vmm_virtio_input_quiesce()` retires both input devices' guest
rings and clears a held event. It leaves the input virtualizer's source queues
untouched. Late source notifications and guest queue kicks cannot consume more
events or write guest buffers. Guest reset does not reopen delivery.

`aos_vmm_virtio_gpu_quiesce()` stops new GPU requests before releasing cursor,
scanout and surface resources through the existing framebuffer adapter.
It returns false if a service operation fails. Retry cleanup with RAM and
service capabilities still present; successful releases are not repeated, and
failed resources remain available to retry. Only a true result permits the
caller to proceed with reclamation. Guest reset cannot reopen queue admission.

The platform adapters reject repeated initialization, including after a partial
initialization attempt. Reusable guest slots need an explicit lifecycle for
device registration and service attachment; these APIs do not supply one.

`make test-input-host` verifies that quiescence and subsequent reset leave both
clients' source queues unconsumed and cause no IRQ or guest-ring access.
`make test-virtio-gpu-host` now links the actual GPU device wrapper as well as
the command engine and framebuffer service. It exercises failed surface
cleanup, successful retry, restored service capacity, and rejected queue kicks
after both cleanup and reset. Existing exact framebuffer-pixel assertions
remain in that test.

These are teardown prerequisites. Production lifecycle wiring, target cleanup,
guest recreation and complete capability reclamation still require acceptance.
