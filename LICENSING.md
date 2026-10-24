# Licensing

**fm1-ctrl is MPL-2.0**, file by file: every source file carries an
`SPDX-License-Identifier: MPL-2.0` header. If you change one of these files and
distribute it, publish your change to that file under MPL-2.0. Your own files may use
any licence.

## Why MPL-2.0 and not GPL-3.0

This repository began as a thin controller over a **vendored copy** of the FM-1 HAL
and USB stack taken from [x0x](https://github.com/charlesvestal/fm1-x0x), which is
GPL-3.0-only. That vendoring is gone: the platform is now
[fm1-base](https://github.com/charlesvestal/fm1-base), which is MPL-2.0 and carries
[felucca-bsp](https://github.com/hugelton/felucca-bsp) as a submodule, also MPL-2.0.

The same files that were GPL-3.0-only in our old `firmware/hal/` and
`firmware/src/{usb,lcd,gfx,ota}.c` exist in `bsp/` with
`SPDX-License-Identifier: MPL-2.0` and the same copyright holders (Leo Kuroshita /
Hügelton Instruments). The relicensing is the authors' own: see
[hugelton/felucca-bsp#4](https://github.com/hugelton/felucca-bsp/issues/4), recorded in
fm1-base's `LICENSING.md`. We did not relicense anything ourselves — we took the copy
the authors already publish under MPL-2.0 and dropped the GPL copy.

MPL-2.0 is also what makes Bluetooth and Wi-Fi possible: their JieLi AC79 libraries are
binary and may be linked from MPL-2.0 code.

## Where the code comes from

| Code | Origin | Licence |
| --- | --- | --- |
| `app/` — this project's own firmware | johnskiller | MPL-2.0 |
| `tools/` — this project's own scripts | johnskiller | MPL-2.0 |
| `web/` — the shim and its tests | johnskiller | MPL-2.0 |
| `platform/` (fm1-base, a submodule or a checkout) | Charles Vestal | MPL-2.0 |
| `platform/bsp/` (felucca-bsp, a submodule) | Leo Kuroshita / Hügelton Instruments, Charles Vestal | MPL-2.0 |
| Leo Kuroshita's code inside the platform | originally Felucca, GPL-3.0-only there | MPL-2.0 here, by the author's permission (`felucca-bsp#4`) |
| `platform/ble/` — the Bluetooth shim | Charles Vestal, repeating parts of the JieLi AC79 SDK | MPL-2.0 AND Apache-2.0, per file |

No GPL-3.0 code is required to build, use or redistribute this firmware. The old
GPL-3.0 tree is in the git history before the `ble-port` merge and in release tags
before v0.2; nothing in the current tree depends on it.

## What this means in practice

- A firmware built from this repository is MPL-2.0. It may be shipped, sold, or
  modified; only changes to the MPL files must be published.
- The JieLi Bluetooth and Wi-Fi libraries are linked as binaries under Apache-2.0;
  they do not make this project GPL.
- If you copy a file of ours into a GPL project, that is allowed (MPL-2.0 is
  GPL-compatible in that direction). The reverse is not.
