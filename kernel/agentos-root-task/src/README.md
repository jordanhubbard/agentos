# Root-task sources

This directory contains the root entry points, architecture descriptors,
capability and memory allocation, PD creation, and runtime support linked
into the root task. `ROOT_TASK_SRCS` in the parent Makefile enumerates its C
inputs; the architecture startup assembly supplies the entry point.

PD implementations live outside this directory:

- VMM implementation and adapters: `platform/guest-vmm/`.
- Boot services: `services/command-console/`, `services/vm-manager/`,
  `services/nameserver/`, `services/log-drain/`, `services/block-driver/`,
  and `services/fault-handler/`.
- Existing canonical drivers and virtualizers retain their service/platform
  directories.
- Shared PD entry and crypto support: `libs/pd-support/`.
- Fault-injection fixture: `tests/fault-inject/`.
- Historical PDs: `services/legacy-pds/`; see the museum rules in `docs/TCB.md`.

The source layout does not change the boot manifest or capability grants.
