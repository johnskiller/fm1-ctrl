/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-ctrl shim: drive a Felucca-family web emulator from the real FM-1.
 *
 * Drop this into a page (or load it from the console) while a firmware's own web
 * emulator is running. It takes the FM-1's plain MIDI — keys as notes, buttons as
 * CC (or notes), encoders as relative CC — and calls the emulator's own input
 * functions, so the *firmware's* UI reacts as if you had clicked its on-screen
 * panel. Nothing is reimplemented here; the shim only translates.
 *
 * Built for the Felucca / Melodee / Sloop emu pages, which share one shape
 * (verified against web/emu/index.html in Felucca and Melodee):
 *
 *   send({type:"buttons", mask})      bit per matrix id
 *   send({type:"keys",    mask})      bit per note key 0..26
 *   send({type:"enc",     role, n})   role = knob role, n = detents
 *   send({type:"master",  value})     0..1023
 *
 * Those are exactly the messages the page's own on-screen panel produces, so the
 * firmware sees a genuine panel, not synthetic MIDI. If the page exposes `send`
 * globally (the emu pages do, inside a module scope — see installShim below for
 * the hook), the shim drives it directly.
 *
 * Usage in a page: <script src="fm1-shim.js"></script> then on the console
 *   fm1Shim.connect()          // pick the FM-1 MIDI ports, start bridging
 *   fm1Shim.map({ ... })       // optional: push a map over SysEx to the device
 *   fm1Shim.status()
 *
 * Usage with the wasm emu (our own page): the page already feeds the firmware's C
 * directly, so the shim is only needed for a *third-party* emu page.
 */
(function (global) {
  "use strict";

  // ---- Felucca-family panel constants (web/emu/worklet.js: KNOB_CC, MASTER_CC) ----
  var KNOB_CC = 20, MASTER_CC = 27, NOTE_KEY0 = 53;

  // RemoteFM1 PROTOCOL section 4 matrix order: 0 = OCT-, 1 = OCT+, 2..13 = FX..REC.
  // Most pages keep their own label order; the shim translates by name.
  var MATRIX_LABELS = ["OCT-", "OCT+", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO",
                       "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"];

  var state = {
    midi: null, inPort: null, outPort: null, sink: null, log: [],
    lastEnc: [], btnMask: 0, keyMask: 0, master: -1,
  };

  function log(s) {
    state.log.push(s);
    if (state.log.length > 200) state.log.shift();
    if (typeof state.onlog === "function") state.onlog(s);
  }

  /* The emu page's message sink. The emu pages keep `send` inside a module
   * closure; they expose it through the AudioWorklet node's port, so we use the
   * same channel the on-screen panel uses when we cannot reach `send` directly. */
  function findSink() {
    if (typeof global.send === "function") return global.send;
    if (global.emu && typeof global.emu.send === "function") return global.emu.send;
    if (global.fm1 && typeof global.fm1.send === "function") return global.fm1.send;
    return null;
  }

  function push(msg) {
    var sink = state.sink;
    if (!sink) { log("no sink: the page must expose send() (see installShim)"); return; }
    try { sink(msg); } catch (e) { log("sink error: " + e.message); }
  }

  // ---- MIDI in: plain device messages -> the page's panel messages ----
  function onMidi(ev) {
    var d = ev.data;
    if (!d || d.length < 2) return;
    var st = d[0] & 0xF0, a = d[1], b = d.length > 2 ? d[2] : 0;

    if (st === 0x90 || st === 0x80) {                  // key notes (and button notes)
      if (a >= NOTE_KEY0 && a <= NOTE_KEY0 + 26) {
        var k = a - NOTE_KEY0, bit = 1 << k, on = st === 0x90 && b > 0;
        if (on) state.keyMask |= bit; else state.keyMask &= ~bit;
        push({ type: "keys", mask: state.keyMask });
        log((on ? "key down " : "key up   ") + k);
      } else if (a < 14) {                             // button-as-note mode
        var bm = 1 << a;
        if (st === 0x90 && b > 0) state.btnMask |= bm; else state.btnMask &= ~bm;
        push({ type: "buttons", mask: state.btnMask });
        log("button " + (MATRIX_LABELS[a] || a) + (st === 0x90 && b ? " down" : " up"));
      }
      return;
    }

    if (st === 0xB0) {
      if (a >= KNOB_CC && a <= KNOB_CC + 6) {          // encoders: relative, 64+n
        var role = a - KNOB_CC, n = b - 64;
        if (!n) return;
        state.lastEnc[role] = n;
        push({ type: "enc", role: role, n: n });
        log("encoder " + role + " " + (n > 0 ? "+" : "") + n);
        return;
      }
      if (a === MASTER_CC) {                            // master, 7-bit fallback
        state.master = Math.round(b * 1023 / 127);
        push({ type: "master", value: state.master });
        log("master " + state.master);
        return;
      }
      if (a === 7 || a === 39) {                        // 14-bit master pair (CC 7/39)
        var msb = (state.master >> 7) & 0x7F, lsb = state.master & 0x7F;
        if (a === 7) msb = b; else lsb = b;
        state.master = Math.round(((msb << 7) | lsb) * 1023 / 16383);
        push({ type: "master", value: state.master });
        log("master 14-bit " + state.master);
        return;
      }
      if (a >= 40 && a <= 53) {                         // buttons as CC (default map)
        var i = a - 40, bit2 = 1 << i;
        if (b) state.btnMask |= bit2; else state.btnMask &= ~bit2;
        push({ type: "buttons", mask: state.btnMask });
        log("button " + (MATRIX_LABELS[i] || i) + (b ? " down" : " up"));
      }
    }
  }

  // ---- map over SysEx: F0 7D 43 54 01 <enc...> <btn...> <led0> <flags> F7 ----
  function mapRequest(m) {
    var body = [0x7D, 0x43, 0x54, 0x01];
    var enc = m.enc || [], btn = m.btn || [];
    body.push(7); for (var i = 0; i < 7; i++) body.push(enc[i] === undefined ? 20 + i : enc[i]);
    body.push(14); for (var j = 0; j < 14; j++) body.push(btn[j] === undefined ? 40 + j : btn[j]);
    body.push(m.btnLed0 === undefined ? 54 : m.btnLed0);
    body.push((m.mirror ? 1 : 0) | (m.buttonsAsNotes ? 2 : 0));
    body.push(0xF7);
    return [0xF0].concat(body);
  }

  var api = {
    KNOB_CC: KNOB_CC, MASTER_CC: MASTER_CC, MATRIX_LABELS: MATRIX_LABELS,
    state: state,

    /* Connect: request MIDI, bind the first input that looks like the FM-1. */
    connect: async function (opts) {
      opts = opts || {};
      if (!navigator.requestMIDIAccess) throw new Error("WebMIDI unavailable (Chrome/Edge)");
      state.midi = await navigator.requestMIDIAccess({ sysex: true });
      var pick = function (ports, re, fallbackFirst) {
        for (var p of ports.values()) if (re.test(p.name || "")) return p;
        if (fallbackFirst) for (var q of ports.values()) return q;
        return null;
      };
      state.inPort = pick(state.midi.inputs, /fm-1|smk|felucca|melodee|sloop|ctrl/i, true);
      state.outPort = pick(state.midi.outputs, /fm-1|smk|felucca|melodee|sloop|ctrl/i, true);
      if (!state.inPort) throw new Error("no MIDI input");
      state.inPort.onmidimessage = onMidi;
      state.sink = findSink();
      if (!state.sink) log("connected to " + state.inPort.name + ", but the page exposes no send()");
      else log("connected: in " + state.inPort.name + (state.outPort ? ", out " + state.outPort.name : ""));
      return api.status();
    },

    /* Push a map to the device over SysEx (CC numbers per encoder/button). */
    map: function (m) {
      if (!state.outPort) throw new Error("no MIDI output");
      var req = mapRequest(m || {});
      state.outPort.send(req);
      log("map sent: " + req.length + " bytes");
      return req;
    },

    /* Ask the device for its current map (it answers F0 7D 43 54 01 ...). */
    query: function () {
      if (!state.outPort) throw new Error("no MIDI output");
      state.outPort.send([0xF0, 0x7D, 0x43, 0x54, 0x02, 0xF7]);
    },

    status: function () {
      return {
        input: state.inPort && state.inPort.name,
        output: state.outPort && state.outPort.name,
        sink: !!state.sink, keys: state.keyMask, buttons: state.btnMask,
        master: state.master, log: state.log.slice(-10),
      };
    },

    /* If the page keeps `send` in a closure, expose it: the emu pages create their
     * AudioWorklet node in switchOn(); patch it there, or set
     *   fm1Shim.state.sink = (msg) => node.port.postMessage(msg)
     * once the page has started. */
    setSink: function (fn) { state.sink = fn; log("sink installed"); },

    /* test seam: the same handler the real port uses */
    _onMidi: function (ev) { return onMidi(ev); },
    _mapRequest: function (m) { return mapRequest(m); },
  };

  global.fm1Shim = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
