# Third-party code bundled with ReRfM-MarsMiner

Everything outside `vendor/` is original to this project and MIT-licensed (`LICENSE`).
Inside `vendor/` sits the bundled MAME core plus the project-original harness that hosts it;
that harness stays BSD-3-Clause, as its file headers say. This file is the inventory of what
is bundled, under which license, and what was changed.

| Component | Where | License | Text |
|---|---|---|---|
| ADSP-2100/2105 CPU core (MAME) | `vendor/dcs/` | BSD-3-Clause | `LICENSES/BSD-3-Clause.txt` |
| `stb_image_write.h` v1.16 | `vendor/stb_image_write.h` | public domain **or** MIT, at your option | in the file itself |
| libFLAC | *not bundled* — system library, linked at build time | BSD-3-Clause | with your distribution |

---

## MAME — ADSP-2100/2105 CPU core

Copyright Aaron Giles and the MAME team, BSD-3-Clause. Upstream:
<https://github.com/mamedev/mame>, `src/devices/cpu/adsp2100/`.

The core is bundled so that MAME's **instruction logic compiles here unchanged** — the DCS
audio in a Pinball 2000 machine is produced by a real ADSP-2105 executing the sound ROM's
own code, so decoding it authentically means running that code, not reimplementing a codec.
What we replace is only the surrounding MAME device framework: address spaces become
callback dispatch, devcb callbacks become plain globals (there is exactly one DSP instance
in an offline tool), and the debugger / save-state / state-registration surface becomes
no-ops. That stand-in is `vendor/dcs/emu_shim.h`, which is ours.

### Files, and exactly how they differ from upstream

| File | Upstream name | Modified? |
|---|---|---|
| `vendor/dcs/adsp2105.h` | `adsp2100.h` | **no** — byte-identical |
| `vendor/dcs/2100ops.hxx` | `2100ops.hxx` | **no** — byte-identical |
| `vendor/dcs/2100dasm.h` | `2100dasm.h` | **no** — byte-identical |
| `vendor/dcs/2100dasm.cpp` | `2100dasm.cpp` | **yes** — one line: `#include "emu.h"` → `#include "emu_shim.h"` |
| `vendor/dcs/adsp2105.cpp` | `adsp2100.cpp` | **yes** — framework scaffolding only (27 diff lines); no instruction semantics touched |

### Upstream revision

⚠ **The MAME revision these files were taken from was not recorded** when they entered the
project (2026-07-12). Rather than guess it, the copies are pinned by content hash, which is
what actually lets anyone verify them against an upstream checkout:

```
ea20a735d4f5a8747eb163588c02d8ddd3d282682f4654dd278934368e59468f  adsp2105.cpp
1cfbfc60573a283e9622cc8cf8bed27ed4fc44b8ecf23887eb38024ff049eca2  adsp2105.h
6986d6b1c56598a7f136ddf46604e3d56ad395d42a9471acedfae8c6ee3b8b14  2100ops.hxx
c45a92b64e7518937878d4fdf94e9604ece0ca3ee4af3fe51deb1c06b47dc969  2100dasm.cpp
4e343fe1898c077e3aac060a0f35db2de669632d31ba44cabf3496915e261317  2100dasm.h
```

The three unmodified files can be matched against upstream directly; that identifies the
revision. Record it here once it is known.

## Original to this project, but BSD-3-Clause

`vendor/dcs/emu_shim.h`, `dcs2_p2k.cpp/.h`, `dcs2_export.cpp`, `dcs2_extract.cpp`,
`flac_encode.cpp/.h` are written for this project, not copied from MAME — but they are
licensed BSD-3-Clause (as their `// license:` headers state), matching the code they host
rather than the MIT of the rest of the repository.

They sit next to the vendored core because they exist only to host it: `dcs2_p2k.cpp` is the
Pinball 2000 DCS-2 board model (SDRC memory map, 28F800 flash boot), written against the
mechanism described by MAME's `src/mame/shared/dcs.cpp` (same BSD-3 license) rather than
copied from it. `emu_shim.h` is the framework stand-in described above.
