# MarsMiner

A single-binary asset extractor for Williams *Pinball 2000* machines. Primary built for
*Revenge from Mars* and should work also for SWE1 (but not tested yet)
It reads the ROM images from a machine you own and writes a
portable, ROM-free asset tree: audio as FLAC, graphics as PNG, fonts as atlas + metrics, the
ROM's own tables as CSV.

```sh
make
./marsminer --roms <chipdir> --bundle <updatedir> --out assets -v
```

That produces `assets.zip`, a plain STORE-method container (the contents are already
PNG/FLAC-compressed) that any tool can open.

Nothing about a particular game is compiled in. The symbol table, the audio tables and the
resource descriptors all come out of the image you hand it - including *which* sounds the game
can actually play, which is walked out of its own audio tables at extract time rather than
shipped as a list. That is what lets one binary serve more than one title.

## ROMs and what this tool produces

This program does not contain, download, or help you find any ROM. You supply the images; the
machine they came from should be yours. What it writes - audio, graphics, fonts, tables - is
derived from those ROMs and belongs to the rights holders of the original game, so **do not
redistribute the output**. It is meant to be generated locally and consumed locally.

The code is MIT (`LICENSE`); the bundled MAME ADSP-2105 core is BSD-3-Clause. See
[THIRD-PARTY.md](THIRD-PARTY.md) for the full inventory and `LICENSES/` for the texts.

## Inputs

```
--roms   DIR   paired mask-ROM chips:  <game>_u100..u110.{rom,bin}  or  U100..U110.ROM
--bundle DIR   flash update bundle:    pin2000_*_game/_sf/_symbols/_im_flsh0/_bootdata.rom
```

The bundle supplies `game.rom` + `symbols.rom` (every stage needs them) and the DCS sound flash
(`*_sf.rom`). The chips supply the image banks (u100–u107) and the raw DCS sample ROMs
(u109/u110).

Chip filenames resolve in both forms they come in: `<prefix>_uNNN.{rom,bin}` as the ROM sets
use it, and the bare `UNNN.ROM` a chip reader writes — letter and extension in either case,
nothing to rename or symlink. The prefix is taken from the files themselves; if a directory
holds more than one set, the most complete one wins and the others are named in a warning, so
`--chip-prefix` is only needed to override that choice.

A dump that is only the chips has no bundle, and therefore no symbol table — see
[chips-only dumps](#chips-only-dumps---scan-images) below.

## What it extracts

| Stage | Output |
|---|---|
| `sounds` | `sounds/*.flac` + `index.csv` — decoded by emulating the sound board, see below |
| `images` | `images/<name>/frameNNN.png` — animations and the delta-coded movie sequences |
| `fonts` | `fonts/font_*.png` + `*.csv` — glyph atlas plus per-glyph metrics |
| `messages` | `tables/messages.csv` — the ROM's message strings |
| `tables` | `tables/symbols.csv`, `adjustments.csv` |

Before any of that, ROM prep runs in-process: the paired mask ROMs are deinterleaved into 16 MiB
image banks, the update flash is assembled, and the symbol table is parsed.

## Chips-only dumps: `--scan-images`

Every stage is symbol-driven: it starts at a `*_ptr` symbol from the bundle's `symbols.rom`,
reads the pointer out of `game.rom` and follows it into the image ROMs. A dump taken off the
mask ROMs alone has neither file, so it extracts nothing — even though the image banks are
intact and in the usual format.

`--scan-images` finds the graphics without them, by shape rather than by name, and synthesises
the pointer word and symbol each walker expects. The decoders themselves are untouched. Three
kinds are recognised:

| | shape it is found by |
|---|---|
| anim | `nframes` at +0x00, `frames_ptr` at +0x1C, then 0x18-byte frame descriptors whose type is one of the six anim codecs, with plausible dimensions, two zero reserved words and a mapped data pointer |
| movie | the same header and descriptors, but every type is `0x1e` and the dimensions obey the movie decoder: ≤ 1024×512, both multiples of 4, and equal to the header's own w/h |
| font | glyph count at +0x00, character array at +0x14, then 0x1C-byte glyphs whose character codes **ascend**, with metrics inside the extractor's bounds and a bitmap of exactly w×h mapped bytes |

Without `--bundle` this is the only mode that can work, so it turns itself on — a chips-only
dump needs no flags beyond the directory:

```sh
./marsminer --roms <chipdir> --out assets -v
```

`sounds` is then skipped with a one-line notice rather than failing the run, since the DCS
sound flash lives in the bundle. (Name a stage in `--only` and it is never skipped: if you ask
for `--only sounds` without a flash, the missing input is an error, which is what you want to
hear.) **The output is addressed, not named** — a dump without a symbol table cannot give the
names back, so output lands in `images/scan_<addr>_<w>x<h>_<n>f_ptr/`,
`images/movie_scan_<addr>_…/` and `fonts/font_scan_<addr>_<n>g.{png,csv}`. `messages` and
`adjustments` stay empty; they live in `game.rom`.

How much the shape alone is worth is measurable, because RfM ships a symbol table that gives
an independent answer. Over RfM's image banks the scan finds 513 structures — 482 anims, 20
movies, 11 fonts — and 499 of them are exactly the ones the symbols reach (skipped as
duplicates, so nothing is extracted twice). Recall is complete where it can be checked: all 20
`movie_*_ptr` and all 4 image-ROM `font_*_data_ptr` are among them.

The other 14 are real assets the shipped table does not name, and not one is a false positive:
7 anims (the service-mode test pattern and the switch-matrix grids) and 7 fonts — which turn
out to be the mask ROMs' own copies of the seven `font_system_*` fonts that the update flash
supersedes. Three of those decode byte-identically to the flash version; the others are the
pre-update artwork. So `--scan-images` earns its place alongside a bundle too: it picks up
what no symbol points at.

Only the image window is scanned, not the update flash — everything in the flash already has
a symbol pointing at it, so there is nothing there for a scan to add.

## DCS audio: the ROM sets must match

The `sounds` stage does not implement a codec. It emulates the sound board's ADSP-2105 and its
SDRC and lets the sound ROM's own code render the PCM, which is then encoded to FLAC. That is
what makes the output authentic - and it is also why the ROM set matters:

**Audio decodes correctly only when the sound flash and the u109/u110 sample ROMs come from the
same build.** The flash holds the descriptor table that addresses the samples. A mixed set —
flash from one build, samples from another - decodes to white noise that the stream's group-KILL
truncates into ~0.03 s fragments. That is not a decoder fault; there is no address scrambling to
undo, the two halves simply disagree.

A dump taken from **one physical sound board** (flash + u109 + u110 read off the same card) is
self-consistent and decodes everything. That is the intended input.

MarsMiner checks this for you rather than trusting it. Before the full run it decodes a few
reference music ids with the stream-KILL disabled and measures spectral flatness: a consistent
set lands around 0.03–0.18, a mismatched one around 0.7–0.85, i.e. noise. On noise it prints
**DCS FLASH/SAMPLE MISMATCH** and aborts instead of writing garbage. `--no-dcs-check` skips the
probe if you know better.

By default the samples come from `--roms` and the flash from `--bundle`. If those are not from
the same board, point it at a consistent set:

```sh
./marsminer ... --dcs-u109 S2.ROM --dcs-u110 S3.ROM --dcs-flash SF.ROM
```

## Options

```
--out DIR         asset output root             (default: assets)
--work DIR        intermediates (banks/…)       (default: work)
--only LIST       subset: sounds,images,fonts,messages,tables
--force           rebuild even if outputs exist
--loose           keep the loose tree next to the .zip
--no-zip          loose tree only, no container
--scan-images     find anims, movies and fonts by structure instead of by symbol. Implied
                  when there is no --bundle; with one, it adds what no symbol points at
--all-sounds      decode every DCS id, not just the ones the game can play
--used-ids F      allow-list override          (default: derived from the image)
--names F         id->name CSV                 (optional; names the FLACs)
--dump LIST       SYM:LEN[,SYM:LEN...] — copy LEN raw bytes at symbol SYM to
                  tables/rom/SYM.bin (tables stage), for data tables an engine reads
                  as-is. The caller names them; MarsMiner knows no title.
--chip-prefix P   pin the <prefix> in <prefix>_uNNN (default: read off the files)
--dcs-u109/u110/flash F   raw DCS sample/flash ROMs
--no-dcs-check    skip the flash/sample consistency probe
-v, -vv           verbosity
-h, --help
```

Exit status is 0 only if every requested stage succeeded; a failed stage exits 1 and no
container is written, so a script can tell the difference. `--out` and `--work` are refused if
they point inside a ROM directory, and the loose tree is only deleted after packing when it
holds nothing but files a run wrote.

## Build

```sh
make            # -> ./marsminer
make test       # unit tests (see below)
```

Requires a C11 + C++20 toolchain and **libFLAC** (`apt install libflac-dev`, `brew install flac`)
- the only external dependency. PNG output uses the vendored `stb_image_write.h`, so no zlib.
Everything else - ROM prep, address space, the anim/movie/font/table decoders, the ZIP writer —
is implemented here in plain C.

Developed on Linux; it should build on macOS as-is. Windows needs a POSIX layer: the code uses
`dirent.h`, `unistd.h` and `setenv`.

## Tests

```sh
make test                    # looks for ROMs in ./roms
make test ROMS=/path/to/roms
```

- `core_test` - executes a hand-decoded ADSP-2105 instruction and reads the register back.
- `dcs2_test` - boots the real DCS-2 board from the ROMs and checks the DSP converges on its
  documented post-boot state.
- `used_ids_test` - runs the play-set walk over the real image and compares it against
  `tests/baseline/rfm_dcs_used_ids.txt`, after verifying the ROM md5s that baseline names.

The two ROM-reading tests **skip themselves** when no ROMs are present, so `make test` passes in
a fresh checkout. With ROMs, they expect the layout `<roms>/chips/` and `<roms>/update_0180/`.

```sh
make golden                  # ~1.5 min, needs ROMs
```

`make golden` is the regression net for the decoders the unit tests do not reach. It re-extracts
images, fonts and tables and compares whole-stage content hashes against
`tests/baseline/rfm_asset_hashes.txt` - after checking the `game.rom` md5 that baseline names, so
it cannot silently pass on a different ROM version. Audio is deliberately not hashed: decoding
the full play set takes far too long for a regression run, and it has its own guards (the board
boot test, the flash/sample mismatch probe, and the pinned play set).

CI builds and runs `make test` on Linux and macOS. It holds no ROMs and never should, so
`make golden` is a local step.

## Layout

```
src/                     the extractor (C) + DCS glue (C++)
vendor/dcs/              ADSP-2105 + SDRC DCS-2 decoder (MAME core + this project's harness)
vendor/stb_image_write.h PNG writer
tests/                   unit tests; tests/baseline/ holds expected results
```

There is no `data/` directory, and that is deliberate: the extractor carries no per-title
tables. Game-specific expectations exist only under `tests/baseline/`, where each file names the
exact game and ROM version it was derived from - consumed by `make test`, never by the
extractor.


