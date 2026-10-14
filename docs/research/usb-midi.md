# USB MIDI on the FM-1 — JieLi stack implementation research

**fm1-ctrl / docs/research/usb-midi.md** — research in progress; appended section by section (timeout-safe).

Scope: how to implement USB MIDI **OUT** (device→host) and **IN** (host→device) on the FM-1's JieLi stack. Evidence = path:line in the four local firmware trees + AC79 SDK.

Physical-device fact (verified by John on the real FM-1): USB MIDI **in AND out both work** on the stock unit. The IN-path section therefore documents *how the rx pattern works / what to copy*, not *whether* it is possible.

Trees referenced:
- `/tmp/fm1-x0x` — x0x FM-1 fork (`firmware/src/app/main_fm1.c` has `usb_start()`/`usb_poll()`, USB ring buffer, "usb_poll never runs nested" note ~L138-145, SAFE MODE keeps USB alive ~L219-340)
- `/tmp/sloop-fork` — sloop
- `/tmp/melodee` — melodee
- `/tmp/bub-machine` — bubba-machine
- AC79 SDK — *path TBD*

Sections below: [1. OUT (device→host)](#1-out-devicehost) · [2. IN (host→device)](#2-in-hostdevice) · [3. USB descriptors](#3-usb-descriptors-per-tree) · [4. Port notes](#4-port-notes-for-firmwaremain_fm1c) · [5. Verdict / port plan](#5-verdict--minimal-port-plan)

---
