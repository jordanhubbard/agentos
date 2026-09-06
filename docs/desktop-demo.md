# Ubuntu Desktop Proof

Status: **experimental, not yet release-qualified**. The deterministic RFB
verifier and Make entry points are present. The live gate must pass on the
release revision before agentOS claims desktop-workload support.

This is the fastest desktop proof because it reuses the Ubuntu live guest,
agentOS-owned virtio net/block/console path, and key-only SSH. It does not wait
for `framebuffer_pd` or virtio-gpu.

## Run it

```bash
make setup
make demo-desktop
```

The target:

1. boots the Ubuntu ARM64 live guest;
2. proves the agentOS virtio net, block, and console path;
3. provisions an ephemeral SSH key and key-only `sshd`;
4. installs TigerVNC, Openbox, xterm, and D-Bus support into the live overlay;
5. starts VNC on guest loopback only;
6. forwards host `127.0.0.1:15901` through authenticated SSH to guest
   `127.0.0.1:5901`;
7. negotiates RFB 3.8, requests raw encoding, and records framebuffer
   dimensions, byte count, desktop name, and FNV-1a checksum;
8. keeps QEMU and the tunnel alive for an external viewer.

Open the printed endpoint with a host viewer:

```bash
vncviewer 127.0.0.1:15901
```

Press Enter in the original terminal to stop the VNC tunnel, guest, and QEMU.
`make demo-clean` removes generated keys, sockets, and logs while preserving
cached guest media.

For automation:

```bash
make demo-desktop-test
```

This runs the same proof and exits after the first bounded raw framebuffer
update. Override the total budget only when diagnosing a slow mirror:

```bash
make demo-desktop-test DESKTOP_TEST_TIMEOUT=5400
```

## Requirements and limits

- Internet access from the Ubuntu guest is currently required to install the
  lightweight desktop packages into its live overlay.
- The guest filesystem changes are ephemeral because the staged Ubuntu media
  is read-only.
- The VNC server accepts security type `None` only on guest loopback. It is
  reachable solely through the key-authenticated SSH tunnel; it is never
  forwarded directly by QEMU.
- Port `15901` must be available on the host.
- The proof intentionally uses Openbox instead of GNOME to stay inside the
  current 1 GiB Ubuntu guest allocation.
- A received RFB frame proves a graphical userspace workload and network
  transport. It does not prove hardware acceleration, virtual HDMI,
  framebuffer export, keyboard injection, pointer injection, or virtio-gpu.
  Those are 0.3 roadmap outcomes in [`ROADMAP.md`](ROADMAP.md).

## Evidence

A qualifying run ends with a message containing:

```text
Ubuntu desktop RFB <width>x<height> bytes=<count> fnv1a64=<checksum>
```

The checksum is evidence that a bounded raw framebuffer payload crossed the
RFB protocol. It is not an image-quality assertion. Release evidence must
retain the command, source revision, serial log, dimensions, byte count, and
checksum.
