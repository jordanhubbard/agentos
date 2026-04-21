# USBService — USB Host Controller Service Contract (PLANNED)

## Overview

USBService will broker access to USB host controller hardware (EHCI/XHCI via
MMIO) for all guest OSes and VMMs.  Individual USB devices are presented to
authorized clients as isolated logical channels.  Direct hardware access to
USB host controllers by guest OSes or VMMs will be prohibited by the device
abstraction policy once this service is implemented.

## Status

**PLANNED.**  No implementation exists yet.  This contract defines the intended
interface that the future USB protection domain must implement.

The USB PD does not appear in the current `tools/topology.yaml` and has no
assigned channel IDs.  When implemented it will be added as a passive PD
alongside the other device abstraction services (block-service, net-service,
timer-service, entropy-service, serial-mux).

## Intended Operations

| Opcode | Description |
|--------|-------------|
| `USB_SVC_OP_ENUMERATE`   | List attached USB devices |
| `USB_SVC_OP_OPEN`        | Open a device channel |
| `USB_SVC_OP_CLOSE`       | Close a device channel |
| `USB_SVC_OP_BULK_READ`   | Bulk IN transfer |
| `USB_SVC_OP_BULK_WRITE`  | Bulk OUT transfer |
| `USB_SVC_OP_CONTROL`     | Control transfer (setup + data) |
| `USB_SVC_OP_STATUS`      | Device and bus status |

## Source Files

- `contracts/usb-service/interface.h` — planned IPC contract
