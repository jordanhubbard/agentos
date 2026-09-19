# Native GUI input during live display

On Spark, the retained packed-read AArch64 guest accepted native GUI F12 and
captured left-button transitions while live display updates continued.
The guest evdev probe printed:

```
AGENTOS_INPUT_READY
AGENTOS_GUI_INPUT_PASS keyboard=4 pointer=4
```

The SSH process exited zero. This requires exactly F12 down, SYN, F12 up, SYN
and BTN_LEFT down, SYN, BTN_LEFT up, SYN on the discovered canonical devices,
followed by no trailing input for 200 ms. Event values, order, and packet
boundaries are checked; SYN values are unspecified by Linux. The probe grabs
both devices and releases them on exit.

Fixture source: `1fcebf2`; guest probe SHA-256:
`b6db70cc8b58e16987c623d6bc232ce901b1c01a828eaefb6b1841f8f4d5cdf8`.
GUI source: `ae8bc4457b73a639dd2793347f2b1516dd0f0548`; native SHA-256:
`907842d460fdc763ed4cbacc28d01c67ee0a88915a7432eee8a12ba915bfa3ca`.
Server runtime remains the packed-frame receipt's `a9391b0` image; this test
changes only the guest fixture and its host checker, not the OS image.

Procedure: start live display, capture the pointer using the native button,
start `/tmp/agentos-native-input-probe --gui` over authenticated SSH, wait for
READY, then send a brief F12 keypress. Wait one second, press the stationary
left button, wait one second, and release it. Host X11 input was injected with
xdotool into the actual focused native window. No browser mock or CC CLI
substituted for the GUI path. The passing screenshot shows live capture in
progress at 82%, last completed sequence 4209, 1.9 seconds per displayed
capture, with keyboard focus and pointer capture active.

Two earlier attempts were rejected and are retained:

* An immediate keypress and quick click coalesced the pointer down/up into one
  GUI batch. The probe expected a SYN between them and rejected the up event.
  The GUI batches queued transitions and appends one SYN; the result does not
  establish an input-delivery defect.
* Holding F12 while inspecting a screenshot generated host autorepeat. The
  exact checker rejected value 2. Repeats after its exit reached the login
  console; no Enter was sent. This is not the passing test's event sequence.

After the passing run, pointer capture and live display were stopped, and the
native application closed normally with exit zero. `make guest-input-probe`
and `make test-host` passed. The initial host compile failed because the
checker call sites needed the new GUI-mode argument; the corrected checker
tests all three modes, including rejection of wrong codes, types, values,
ordering and extra events.

Artifacts, screenshots and checksums are retained externally in
`/home/jkh/.local/share/agentos-evidence/2026-09-19-native-live-input/`.
This proves key/button delivery during live transfer. It does not measure
precise input latency, prove native relative motion/wheel delivery, qualify
abrupt-disconnect releases, or establish final-release integration.
