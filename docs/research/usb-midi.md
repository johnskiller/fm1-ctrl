# USB MIDI on the FM-1 — JieLi stack implementation research

**fm1-ctrl / docs/research/usb-midi.md** — research in progress; appended section by section (timeout-safe).

Scope: how to implement USB MIDI **OUT** (device→host) and **IN** (host→device) on the FM-1's JieLi stack. Evidence = `path:line` in the four local firmware trees + the AC79 SDK.

Physical-device fact (verified by John on the real FM-1): USB MIDI **in AND out both work** on the stock unit. The IN-path section documents *how the rx pattern works / what to copy*, not *whether* it is possible.

## Trees referenced

| tree | path | USB firmware files |
|---|---|---|
| x0x | `/tmp/fm1-x0x` | `firmware/src/usb.c` (1078 L), `firmware/hal/fm1_usb.h` (117 L), `firmware/src/app/main_fm1.c` |
| sloop | `/tmp/sloop-fork` | `firmware/src/usb.c` (1104 L), `firmware/hal/fm1_usb.h` |
| melodee | `/tmp/melodee` | `firmware/src/usb.c` (1002 L), `firmware/src/usb_audio.c` (303 L), `firmware/hal/fm1_usb.h` |
| bubba-machine | `/tmp/bub-machine` | `hal/fm1/fm1_usb_midi.c` (403 L), `hal/fm1/fm1_hw.h`, `hal/fm1/fm1_main.c` |
| AC79 SDK | `/tmp/fw-AC79_AIoT_SDK` | *paths TBD, see §3* |

All four trees' `firmware/hal/fm1_usb.h` are the same register-level HAL family (x0x copy verified: register map at `firmware/hal/fm1_usb.h:22-30`, no IRQ — "everything is polled" at `firmware/hal/fm1_usb.h:17`).

## 1. OUT (device→host)

### 1.1 x0x — the reference implementation

x0x `firmware/src/usb.c` is a self-contained, comment-dense USB-MIDI device on USB0, polled (no USB IRQ):

- **Threading model** — the whole SIE protocol runs in `usb_poll()` from the **TIMER5 ISR at 2 kHz** (`firmware/src/usb.c:954`; header comment `firmware/src/usb.c:3-6` says: "All SIE traffic happens in usb_poll(), called from the TIMER5 ISR at 2 kHz, so INDEX is never shared and no USB IRQ is needed"). The app produces MIDI from the audio ISR via rings; `main_fm1.c:138-147` shows the call site — "the consumer side of its ring, and usb_poll never runs nested" at `main_fm1.c:138`, `usb_poll()` at `main_fm1.c:145`, then `uart_midi_poll()` (TRS jack) at `main_fm1.c:147` — same context, same queue.
- **Buffers** — EP buffers 64 B, aligned: `ep0buf/ep1tx/ep1rx` at `firmware/src/usb.c:29-31`. MIDI rings of 4-byte USB-MIDI event packets: `#define MQ 64u; midi_in_q[MQ], midi_out_q[MQ]` at `firmware/src/usb.c:128-131`.
- **OUT path (app → host)** — producer `midi_out_event(uint32_t pkt)` at `firmware/src/usb.c:133-140`, called **from the audio ISR**: checks `usb.config`, writes one 4-byte USB-MIDI event packet into `midi_out_q`, `RING_PUBLISH()`, advances `mo_w`. Consumer `ep1_tx()` at `firmware/src/usb.c:699-745`: drains `midi_out_q` into `ep1tx` (max 64 B/packet), then `fm1_usb_ep_send(1, ep1tx, n)` at `:742` and sets TxPktRdy (`S_TXCSR1 |= 0x01`) at `:743`. `ep1_tx()` is called every poll once configured (`firmware/src/usb.c:1027-1028`). Double-checks the previous packet still pending (`csr & 0x01u` → return, `:711-712`); ring reads bracketed by `RING_PUBLISH()` because the audio ISR can preempt (`:730-738`).
- **IN path (host → app)** — `ep1_rx()` at `firmware/src/usb.c:622-651` (see §2).

### 1.2 Endpoint wiring (x0x, applies to all trees)

- `ep1_config()` at `firmware/src/usb.c:312-352`: EP1 bulk **BIDI** — `fm1_usb_ep_txbuf(1, ep1tx)` / `fm1_usb_ep_rxbuf(1, ep1rx)` (`:314-315`), `S_TXMAXP/S_RXMAXP = 0xFF` (64 B), `S_RXCSR1 = 0x90`, RX interrupt-enable `S_INTRRX1E = 0x02`, `fm1_usb_ep_enable(1u<<1)` (`:316-324`). CDC adds EP2/EP3 (`#if FELUCCA_CDC`, `:325-343`); UAC adds isochronous EP4 IN (`:344-351`).
- Called from EP0 `SET_CONFIGURATION` handler (`case 0x0009`, `firmware/src/usb.c:462-474` → `ep1_config()` at `:467`).

### 1.3 The HAL the driver sits on (`hal/fm1_usb.h`)

Every USB register access in the driver goes through this header (`firmware/hal/fm1_usb.h:3-17`):

- Register map: `FM1_USB_CON0 0x11800`, `CON1 0x11804`, per-EP DMA address registers `0x11808-0x1183C` (`firmware/hal/fm1_usb.h:22-30`).
- `fm1_usb_reset()` / `fm1_usb_attach(ep0)` / `fm1_usb_off()` — bring-up: USB clock = PLL48M, D+ pull-up = attach (`firmware/hal/fm1_usb.h:32-62`).
- **SIE access through CON1** — MUSB byte map, only while `CON0.bit2` (USB_NRST) set: `fm1_usb_sie_on/_wr_start/_rd_start/_done/_data` (`firmware/hal/fm1_usb.h:64-77`). Driver wrappers with timeout + "SIE dead" fail-safe: `sie_wr`/`sie_rd` at `firmware/src/usb.c:254-282`.
- EP DMA: `fm1_usb_ep0_send`, `fm1_usb_ep_send(ep,p,n)` (with `csync` before DMA start), `fm1_usb_rx_sync()` (`ssync` before reading an RX buffer) (`firmware/hal/fm1_usb.h:95-107`).
- `fm1_usb_ep_enable(eps)` = `CON0 &= ~(eps << 19)` (bit per EP) (`firmware/hal/fm1_usb.h:109`); `fm1_usb_sof_take()` (`:111-117`).
- No IRQ: everything polled (`firmware/hal/fm1_usb.h:17`).

### 1.4 Boot chain (x0x)

`usb_start()` at `firmware/src/usb.c:1044-1058`: `fm1_usb_reset()` → 25 ms delay → `fm1_usb_attach(ep0buf)` → enable suspend/resume/reset + EP0 TX interrupts (polled) → `fm1_usb_ep_enable(0x1Fu)` → `usb.up = usb.timeouts < 3u`. Called at `main_fm1.c:329` (normal boot) and `main_fm1.c:240` (safe mode). `usb_retry(now_ms)` at `firmware/src/usb.c:1063-1071` re-runs it 1×/s from the main loop so late-plugged cables still enumerate (called at `main_fm1.c:349`). Safe mode (`main_fm1.c:216-255`) keeps `usb_start()`/`usb_retry()` alive with no audio engine.

*(§2 IN, §3 descriptors, §4 port notes, §5 verdict to follow.)*
