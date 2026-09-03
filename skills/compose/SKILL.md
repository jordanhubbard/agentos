# compose

Build-time composition of agentOS. Runtime VibeOS CREATE of phantom guests
is not composition.

## Invariants

- A system is drivers + virtualizers + N VMM clients + optional native clients.
- Skills assemble topology; generators emit `system_desc` / SDF later.
- Do not compose museum PDs into a "complete OS".

## Helper

```sh
python3 skills/compose/scripts/compose_view.py
```
