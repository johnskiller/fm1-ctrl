
import fs from 'fs';
(async () => {
  const bytes = fs.readFileSync(new URL('../../build/emu/fm1ctrl.wasm', import.meta.url));
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const M = instance.exports;
  const A = (c, m) => { if (!c) { console.error('FAIL:', m); process.exitCode = 1; } else console.log('ok:', m); };
  M.web_boot();
  // 1. key F3 (idx 0) down -> after ticks, expect 90 35 64 out (note 53, vel 100)
  M.web_key(0, 1);
  for (let i = 0; i < 4; i++) M.web_tick();
  const got = [];
  let v;
  while ((v = M.web_out_get()) >= 0) got.push([v & 255, (v>>8)&255, (v>>16)&255]);
  A(got.length === 1 && got[0][0]===0x90 && got[0][1]===53 && got[0][2]===100,
    'key F3 -> ' + JSON.stringify(got.map(g=>g.map(x=>x.toString(16)))));
  M.web_key(0, 0);
  for (let i = 0; i < 4; i++) M.web_tick();
  got.length = 0; while ((v = M.web_out_get()) >= 0) got.push([v & 255, (v>>8)&255, (v>>16)&255]);
  A(got.length === 1 && got[0][0]===0x80 && got[0][1]===53 && got[0][2]===0,
    'key F3 release -> ' + JSON.stringify(got.map(g=>g.map(x=>x.toString(16)))));
  // 2. button FX (0) -> B0 28 7F / B0 28 00 (CC 40)
  M.web_button(0, 1);
  for (let i = 0; i < 4; i++) M.web_tick();
  M.web_button(0, 0);
  for (let i = 0; i < 4; i++) M.web_tick();
  got.length = 0; while ((v = M.web_out_get()) >= 0) got.push([v & 255, (v>>8)&255, (v>>16)&255]);
  A(got.length === 2 && got[0][0]===0xB0 && got[0][1]===40 && got[0][2]===127 &&
    got[1][1]===40 && got[1][2]===0,
    'button FX -> ' + JSON.stringify(got.map(g=>g.map(x=>x.toString(16)))));
  // 3. encoder SELECT +2 steps -> B0 14 41 twice (CC 20, 64+1)
  M.web_enc(0, 2);
  for (let i = 0; i < 20; i++) M.web_tick();   // 2 steps * 4 transitions, plenty of ticks
  got.length = 0; while ((v = M.web_out_get()) >= 0) got.push([v & 255, (v>>8)&255, (v>>16)&255]);
  A(got.length === 2 && got.every(g => g[0]===0xB0 && g[1]===20 && g[2]===65),
    'encoder +2 -> ' + JSON.stringify(got.map(g=>g.map(x=>x.toString(16)))));
  // 4. MIDI-in CC 3 = 0x2A -> grid row 3 reads back 0x2A
  M.web_midi_in(0xB0, 3, 0x2A);
  A(M.web_grid_row(3) === 0x2A, 'midi-in CC3 -> grid row3 = 0x' + M.web_grid_row(3).toString(16));
  // 5. MIDI-in note 60 on -> key LED idx 7 (60-53)
  M.web_midi_in(0x90, 60, 100);
  A((M.web_lit_keys() >> 7) & 1, 'midi-in note60 -> key LED 7 lit');
  // 6. button-LED feedback: CC 54 (FX) on -> web_btn_leds bit0; off clears
  M.web_midi_in(0xB0, 54, 0x7F); M.web_tick();
  A(M.web_btn_leds() === 1, 'CC 54 lights FX LED');
  M.web_midi_in(0xB0, 54, 0); M.web_tick();
  A(M.web_btn_leds() === 0, 'CC 54 value 0 clears FX LED');
  console.log('WASM EMU TESTS DONE');
})().catch(e => { console.error(e); process.exit(1); });
