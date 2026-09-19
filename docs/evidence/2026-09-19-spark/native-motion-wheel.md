# Native motion/wheel probe: acceptance remains incomplete

Fixture source `19f9809` adds `input-probe --gui-pointer`. It requires F12
press/release plus exact ordered pointer values X=17, Y=-9, WHEEL=+1,
BTN_LEFT down/up. Adjacent events may share a packet; empty SYN packets,
wrong values/order, missing final SYN and trailing input are rejected.
Host tests exercise every grouping of these transitions and invalid values.
`make guest-input-probe test-host` passed.

Probe SHA-256:
`3f20a0d9ac57e4bbb70d562807eded7b51349ec78e5f3109a622dffe19bca6b8`.
The retained AArch64 guest and GUI are unchanged from the native-live-input
receipt: GUI runtime `ae8bc44`, packed server runtime `a9391b0`.

Native pointer capture and live display remained active during both attempts.
After authenticated SSH printed READY, host X11 events were injected through
the actual native GUI using xdotool. A brief F12 keypress and relative motion
(17,-9) preceded wheel-up clicks and a left click.

* One wheel-up click: probe exited 1 with
  `unexpected device 1 event 2: 1/272/1`. X/Y were accepted; the left-button
  press arrived before the expected wheel event.
* Capture was released and reacquired to clear wheel remainders. Three
  wheel-up clicks spaced 100 ms apart: probe exited 1 with
  `unexpected device 1 event 3: 2/8/1`. X/Y and one WHEEL=+1 were accepted,
  then another WHEEL=+1 arrived before the expected button press.

These observations establish native relative motion and wheel delivery, but
do not pass the specified complete sequence. They do not yet establish the
individual WebKit delta values, exact total wheel output, or a transport
defect. The GUI accumulates pixel-mode wheel deltas in units of 100 pixels;
native source-event telemetry is needed to explain this mapping before
changing normalization or claiming one host detent equals one guest detent.

The passing exact key/button qualification remains separate. Precise input
latency and abrupt-disconnect release remain unproven. Artifacts are retained
under `/home/jkh/.local/share/agentos-evidence/2026-09-19-native-motion-wheel/`.
