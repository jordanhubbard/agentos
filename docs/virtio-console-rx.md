# Console receive ownership

The canonical libvmm console validates a receive chain before consuming any
input for that head. It snapshots the published availability count, rejects
counts exceeding the queue size, checks every descriptor index and writable
flag, bounds chain traversal, and translates every complete buffer span.
Indirect descriptors are not negotiated. A private snapshot of the translated
spans drives the subsequent copy; descriptor metadata is not reread during it.

Input remains queued until receive buffers exist. The pump consumes at most
the snapshotted ingress length and examines at most the queue's published
heads. A private used index determines completion placement, including 16-bit
wrap. A rejected chain consumes no input and publishes no completion for that
head. Earlier valid completions in the same batch still receive an interrupt.
The failure is latched until the device resets.

MMIO queue sizes and addresses cannot change after QueueReady maps the rings.
Device reset clears the mapped queue state before reconfiguration. Shared GPA
mapping also checks descriptor, available-ring and used-ring alignment before
converting addresses into typed host pointers.

`make test-virtio-console-rx-host test-virtio-mmio-core-host` exercises the
actual receive helper and register/GPA implementation under ASan/UBSan,
including chained delivery, input retention, invalid metadata, completion
ownership, index wrap and rejection of mapped queue reconfiguration.
`make gate` qualifies live guest console input through the canonical service
path as well as the shared network/block register path. These checks do not
claim an Intel console composition or complete v0.4 device qualification.

The full Spark gate passed at `50b8930`, including live guest console input.
The [receipt](evidence/2026-09-17-spark/virtio-console-rx.json) retains exact
artifact hashes and verification limits.
