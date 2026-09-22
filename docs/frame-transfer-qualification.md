# Varied frame transfer qualification

`make host-frame-pattern guest-frame-pattern` builds the same C pattern
generator for the host and the AArch64 Linux guest. Patterns are `tiles`,
`gradient`, and deterministic `noise`, with tightly packed 1024×768 XRGB8888
bytes. Generate an exclusive new reference file with:

```
build/tmp/host-frame-pattern generate tiles /tmp/tiles.raw
```

Copy `build/tmp/guest-frame-pattern-aarch64` to a qualified Linux guest using
its authenticated SSH connection. Run `frame-pattern display tiles 180` in
the guest. The fixture requires active text VT1 and the exact framebuffer
geometry and RGB layout. It switches that VT to graphics mode to prevent
console redraws, saves the original framebuffer, writes the pattern, and
holds it for at most the requested 1–300 seconds. Normal completion or handled
SIGINT, SIGTERM, and SIGHUP restore the framebuffer and text mode. SIGKILL
cannot perform cleanup. Check the `FRAME_PATTERN_RESTORED` result and exit code.

`FRAME_PATTERN_WRITTEN` reports the fixture PID and completed fbdev write;
it is not a display fence. Verify the observed pixels against the reference.
In the paired GUI checkout, use:

```
FRAME_REFERENCE=/tmp/tiles.raw make benchmark-frame-transfer CC_PD_SOCK=/path/to/cc.sock BENCH_GUEST_HANDLE=0 BENCH_ORDER=raw-first
```

The production CC client compares both complete transfers with that reference,
in addition to raw/packed equality. Any mismatch fails the measurement.
Repeat with `BENCH_ORDER=packed-first` to reverse the order. Keep each fixture
active throughout its measurements; terminate only its reported PID when done.
Restore text mode before proceeding to the next scene.

These are deterministic raster fixtures, not desktop application acceptance.
Record both timings and exact pixel agreement, including incompressible
fallback. Native rendering, pointer delivery, input latency and cold-boot
stability require separate evidence.
