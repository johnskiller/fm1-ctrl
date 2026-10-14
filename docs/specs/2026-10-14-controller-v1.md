# FM-1 控制器固件 spec (v1)

**日期**: 2026-10-14 · **状态**: draft → review
**前置**: idea 6 讨论（thread 1559828341044027412）· **流水线**: spec → host sim → shim → firmware（Polly 验证过的路线）

## 1. 问题与定位

每个 FM-1 固件都带 web 模拟器，但只能鼠标操作（27 个小按钮 + 拖旋钮）。
FM-1 硬件本身是极好的输入设备却没东西可玩。**翻转：让物理 FM-1 成为任意
web/emu 的输入设备**，emu 的状态（步骤点亮、参数值）经 Web MIDI 回传驱动
LED/屏幕。

- 生态中最薄的固件：无 DSP、无存储、无音序器，只有 scan → USB MIDI out
  + 反馈 → LED/屏幕。估 1–2 天。
- 每个 emu 只需一个 shim（通用 JS + 一份 JSON map，~半天）。
- 是 DriftSeq（idea 3）的骨架：keyscan/LED/USB-MIDI 层直接复用。
- 兼作 idea 4 的 MIDI-in→audio 延迟测试台。

## 2. v1 范围（In scope）

真实面板拓扑（Felucca 系 panel.c，CTL-1 校准表证实）：

| 控件 | 数量 | v1 输出 |
|---|---|---|
| 琴键 | 27（下排 C3..B3 = note 48..61，上排 C4..A4 = note 60..72） | note on/off，velocity 固定 100（fm1-velocity 已证明 ADC+DRIVE 可读压力，后续升级） |
| 编码器 | 7（SELECT/ALGORITHM/PRESETS/K1-K4） | 相对 CC 64±n（melodee 默认 map = CC 20..26）；push+turn → ch2 二层 |
| 按钮 | 14（FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT− OCT+） | CC 40..53，按下 127 / 释放 0（map 可改） |
| LED 回传 | 5×7 网格 + 键位 | emu → MIDI → LED（idea 1 grid 复用） |
| 屏幕回传 | v1 最小化：连接状态 + 当前 map 名；值显示走 LED（v1.1 可选） |
| USB | MIDI in+out 双向（**实机已验证**，2026-10-14 John） |

**与 CTL-1 的差异（John 实机对照 2026-10-14）**：CTL-1 只有 Play 页 K1-K4 发 CC、
PLAY/REC 发事件、PRESET 发 ProgChange；SELECT/ALGORITHM 和多数命令按钮**无任何
MIDI 输出**（只改它自己的页面状态）。fm1-ctrl 的承诺：**每个控件都发事件，
无一遗漏** — 这是本项目的存在理由。

**Out of scope (not v1)**: sequencer/存储/GM/节拍器、屏幕显示 emu 参数值、
MIDI DIN 转发、多 map 切换 UI（v1 固定编译进一份 map）。

## 3. 架构

```
firmware/
  main.c        init + 主循环
  keyscan.c     27 键矩阵扫描 → note on/off (去抖、旋转编码器 quadrature)
  encoders.c    8× encoder push/turn → CC
  leds.c        LED 网格 + 键位 LED → 接收自 MIDI in 的状态回显
  midi_io.c     USB MIDI in/out, 运行时读 map 表
  map.c         编译进固件的 JSON map 解析结果 (常量表)
host/
  sim.c         host UI 模拟器: 同一 keyscan/midi_io 代码, 屏幕/LED 用 SDL/终端渲染
web/
  shim.js       通用 shim: 读 map JSON, WebMIDI 双向桥接 emu 页面
  maps/*.json   每 emu 一份 map
```

设计原则（沿 melodee/felucca 惯例）: **pure C, host-tested** — keyscan/led/midi
逻辑在 host sim 里跑通再上板。

## 4. 按键映射约定 (v1 内置 map)

- 27 键 = 2 行 × 13/14：上排白键 C4 起 13 键，下排 C3 起 14 键（与 idea 1
  grid 的 black-keys-as-pitch-row 兼容，后续 DriftSeq 复用同一布局）。
- 编码器 1–8 → **CC 20–27**（修订 2026-10-14：采用 melodee emu 的原生约定 —
  其页面 `midiKnob()` 以 CC 20–26 为旋钮角色、CC 27 为 MASTER，且 abs 模式
  天然消化相对编码器 64±n。默认 map = melodee 兼容，零 shim 直连）。
- 按钮: PLAY=note 36, STOP=note 37, REC=note 38, OCT- / OCT+ = CC 102/103,
  PAGE = CC 104。

精确的 note/CC 分配以 `docs/map-convention.md` 为准；该文件是这份 spec 的
机器可读投影，冲突时以 convention 为准。

## 5. 接口约定

**固件 → emu (USB MIDI out)**: note on/off (27 键)、CC (8 enc + 按钮)、
无 clock/sysex (v1)。

**emu → 固件 (USB MIDI in)**:
- CC 0–119 任意值 → LED 网格按约定点亮（具体 bit 映射见 convention）；
- note on/off → 键位 LED 回显；
- 其余忽略。

## 6. 验收标准 (v1 done = )

1. Host sim：模拟 27 键 + 8 enc + 按钮，产生与固件一致的 MIDI 字节流（单测比对）。
2. melodee emu 端到端：物理 FM-1 按键 → emu 出声；emu 播放 → FM-1 LED 跟随。
3. 延迟：按键→emu 发声 < 25 ms（WebMIDI 典型值，实测记录）。
4. 固件大小：XIP slot 内有余量（生态惯例：报告 headroom 而非绝对值）。

## 7. 风险

- ~~**WebMIDI 仅 Chromium**~~ — 可接受（emu 用户已是 wasm/AudioWorklet Chrome 用户）。
- 35 个 emu 分裂 → JSON map convention 是收敛点；maps 放 hub 可静态托管。
- ~~**FM-1 USB MIDI in 是否真的能收 host 数据**~~ — **已解除（2026-10-14 John 实机验证）**：
  USB MIDI in/out 双向均正常工作。剩余工作只是移植时抄对 SDK 的 init/rx 模式
  （见 docs/research/usb-midi.md）。
