/* node web/fm1-shim.test.cjs - device MIDI -> a Felucca-family emu page's panel */

const assert = require('assert');
const sent = [];
globalThis.send = (m) => sent.push(m);
const shim = require('./fm1-shim.js');
shim.setSink(globalThis.send);
const feed = (b) => shim._onMidi({ data: Uint8Array.from(b) });

feed([0x90, 53, 100]); feed([0x90, 60, 100]);
assert.deepStrictEqual(sent[sent.length-1], { type: 'keys', mask: 0b10000001 });
console.log('ok: device keys -> page keys mask');

feed([0x80, 60, 0]);
assert.strictEqual(sent[sent.length-1].mask, 0b1);
console.log('ok: release clears the key bit');

feed([0xB0, 20, 0x41]);                        // encoder 0, +1 detent
assert.deepStrictEqual(sent[sent.length-1], { type: 'enc', role: 0, n: 1 });
console.log('ok: relative CC -> page enc');

feed([0xB0, 27, 64]);                          // master 7-bit
assert.strictEqual(sent[sent.length-1].type, 'master');
assert.strictEqual(sent[sent.length-1].value, Math.round(64*1023/127));
console.log('ok: CC 27 -> master');

shim.state.btnMask = 0;
feed([0xB0, 40, 127]);                         // button FX down (default map)
assert.strictEqual(sent[sent.length-1].mask, 1);
feed([0xB0, 53, 127]);                         // OCT+ (btn 13)
assert.strictEqual(sent[sent.length-1].mask, (1<<13)|1);
feed([0xB0, 40, 0]);
assert.strictEqual(sent[sent.length-1].mask, 1<<13);
console.log('ok: buttons as CC -> page buttons mask');

// button-as-note mode (RemoteFM1 order): matrix 2 = FX
sent.length = 0; shim.state.btnMask = 0;
feed([0x90, 2, 127]);
assert.strictEqual(sent[sent.length-1].mask, 1<<2);
console.log('ok: button-as-note -> page buttons mask');

// map frame
const f = shim._mapRequest({ enc: [30,31,32,33,34,35,36], btn: Array.from({length:14},(_,i)=>60+i), btnLed0: 74 });
assert.strictEqual(f[0], 0xF0); assert.strictEqual(f[f.length-1], 0xF7);
assert.strictEqual(f[1], 0x7D); assert.strictEqual(f[2], 0x43); assert.strictEqual(f[3], 0x54); assert.strictEqual(f[4], 0x01);
assert.strictEqual(f.length, 31);   // F0 + hdr4 + enc + btn + led0 + flags + F7
assert.strictEqual(f[f.length-2], 0);          // flags: CC mode, no mirror
console.log('ok: map frame shape', f.length, 'bytes');
console.log('SHIM TESTS DONE');
