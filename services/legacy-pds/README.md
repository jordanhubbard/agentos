# Historical PD implementations

These sources were moved out of the root task directory without extending
their behavior. Their presence here does not grant TCB status or make them
part of the default boot image. `docs/TCB.md` and the system descriptor define
the actual boot set and authority boundaries.

The root-task Makefile still builds historical images for compatibility.
Do not add device frames, IRQs, or new protocol operations to museum PDs.
New platform features belong in the canonical driver, virtualizer, VMM, or
native-client paths described in `CLAUDE.md`.
