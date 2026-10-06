/* marsminer.h — ReRfM-MarsMiner: a self-contained C asset extractor for
 * Williams "Revenge from Mars" and its Pinball 2000 siblings.
 *
 * One binary that reads a machine's ROMs and writes a portable, ROM-free asset
 * tree: audio as FLAC, graphics as PNG, fonts as atlas + metrics, the ROM's own
 * tables as CSV. Nothing about a particular game is compiled in — the symbol
 * table, audio tables and resource descriptors all come out of the image you
 * hand it, which is what lets one binary serve more than one title.
 *
 * Provenance: several decoders below cite a Python script by name
 * (render_anims.py, extract_fonts.py, …). That is the reference implementation
 * this tool was written against and diffed byte-for-byte with; it belongs to the
 * parent ReRfM project, not to this repository. The names stay because they say
 * where each decoder's correctness claim comes from.
 *
 * External dependencies (only where a from-scratch implementation adds no value):
 *   - stb_image_write.h  (vendored, public domain) — PNG output
 *   - libFLAC            (system, BSD-3)            — lossless audio output
 *   - the ADSP-2105 DCS core (BSD-3 MAME core) — audio decode
 * Everything else (ROM prep, address space, anim/movie/font/table decoders) is
 * implemented here in plain C.
 */
#ifndef MARSMINER_H
#define MARSMINER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 *  byte buffer
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t len;
} mm_buf;

/* Load an entire file. On failure returns {NULL,0} and, if `required`, the
 * caller should treat it as fatal (mm_load_required does the die()). */
mm_buf mm_load(const char *path);
mm_buf mm_load_required(const char *path, const char *what);
int mm_write_file(const char *path, const void *data, size_t len);
void mm_buf_free(mm_buf *b);

/* Little-endian scalar reads with bounds checking (return 0 past the end). */
uint32_t mm_u32(const mm_buf *b, size_t off);
uint16_t mm_u16(const mm_buf *b, size_t off);
uint8_t mm_u8(const mm_buf *b, size_t off);

/* --------------------------------------------------------------------------
 *  address space — the BAR5 image-ROM windows + optional BAR3 flash
 *  (1:1 with walk_anims.py:AddrSpace)
 *
 *    0x14xxxxxx -> bank0   0x15xxxxxx -> bank1
 *    0x16xxxxxx -> bank2   0x17xxxxxx -> bank3   (each exactly 16 MiB)
 *    0x12xxxxxx -> BAR3 update flash (optional)
 *  The four banks are concatenated into one 64 MiB image so a blob that spills
 *  from one bank into the next resolves correctly.
 * -------------------------------------------------------------------------- */
#define MM_IMG_BASE 0x14000000u
#define MM_IMG_END 0x18000000u
#define MM_BANK_SZ (16u * 1024u * 1024u)
#define MM_GBASE 0x00100000u /* game.rom link base */

typedef struct {
    uint8_t *image; /* 64 MiB: bank0|bank1|bank2|bank3 */
    size_t image_len;
    int have[4];        /* which bank files were present */
    mm_buf flash;       /* BAR3 flash (may be {NULL,0}) */
    uint32_t flash_top; /* top byte selecting flash (0x12) */
} mm_aspace;

/* Build the address space from a banks directory (rfm_bank{0..3}.bin) and an
 * optional flash image. `flash` may be {NULL,0}. Returns 0 on success. */
int mm_aspace_init(mm_aspace *as, const char *banks_dir, mm_buf flash);
void mm_aspace_free(mm_aspace *as);

/* Resolve a guest pointer to a (base,offset) window. Returns the backing bytes
 * and sets *off; returns NULL if the pointer is outside every window. */
const uint8_t *mm_region(const mm_aspace *as, uint32_t ptr, size_t *off, size_t *avail);
/* Read a LE u32/u16 through the address space; returns `def` if unresolved. */
uint32_t mm_as_u32(const mm_aspace *as, uint32_t ptr, uint32_t def);

/* --------------------------------------------------------------------------
 *  symbol table  (1:1 with parse_symbols.py)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t addr;
    char *name; /* owned */
} mm_sym;

typedef struct {
    mm_sym *syms;
    size_t count;
} mm_symtab;

/* Parse a *_symbols.rom into a symbol table (sorted by address, as in the ROM). */
int mm_symbols_parse(mm_symtab *st, const mm_buf *rom);
void mm_symtab_free(mm_symtab *st);
/* Write the address,name CSV (1:1 with parse_symbols.py --csv). */
int mm_symbols_write_csv(const mm_symtab *st, const char *path);
/* Look up a symbol's address by exact name; returns 0 and sets *found=0 if absent. */
uint32_t mm_sym_addr(const mm_symtab *st, const char *name, int *found);

/* --------------------------------------------------------------------------
 *  options / context
 * -------------------------------------------------------------------------- */
typedef struct {
    const char *roms_dir;    /* dir of paired chip ROMs (u100..u110)     */
    const char *chip_prefix; /* chip-filename prefix, e.g. "rfm" in rfm_u109.bin. NULL/"" =
                                work it out from the directory; bare uNNN dumps have none */
    const char *bundle_dir;  /* update bundle (*_game/_sf/_symbols/...)  */
    const char *out_dir;     /* asset output root                        */
    const char *work_dir;    /* intermediates (banks/flash/symbols)      */
    /* explicit DCS sound-ROM overrides (else derived from roms/bundle)  */
    const char *dcs_u109, *dcs_u110, *dcs_flash;
    const char *used_ids;    /* --used-ids: allow-list override; NULL = derive from the image */
    const char *sound_names; /* id->name CSV (optional)                          */
    int verbose;             /* -v count                                 */
    int force;               /* rebuild even if outputs exist            */
    unsigned steps;          /* bitmask MM_STEP_*                        */
    int all_sounds;          /* decode every id, not just the used list  */
    int no_dcs_check;        /* skip the DCS flash/sample consistency probe */
    int no_zip;              /* don't pack into a .zip (loose files only) */
    int keep_loose;          /* keep the loose asset tree alongside the .zip */
    int scan_images;         /* find anims by shape, not by symbol (chips-only dumps) */
    const char *dumps;       /* --dump: "SYM:LEN[,SYM:LEN...]" raw data -> tables/rom/SYM.bin */
} mm_opts;

enum {
    MM_STEP_PREPARE = 1u << 0,
    MM_STEP_SOUNDS = 1u << 1,
    MM_STEP_IMAGES = 1u << 2,
    MM_STEP_FONTS = 1u << 3,
    MM_STEP_MESSAGES = 1u << 4,
    MM_STEP_TABLES = 1u << 5,
    MM_STEP_MANIFEST = 1u << 6,
    MM_STEP_ALL = 0x7f,
};

/* Resolved intermediate inputs shared by the asset stages. */
typedef struct {
    mm_buf game_rom;  /* game.rom (link base 0x100000)            */
    size_t game_len;  /* bytes of it that came from the bundle — --scan-images appends its
                         own pointer table past this, and stages that bound-check against
                         "the end of the ROM" must use this, not game_rom.len          */
    mm_symtab symtab; /* parsed symbols                           */
    mm_aspace aspace; /* banks + flash                            */
    mm_buf flash;     /* assembled BAR3 flash (owns the bytes)    */
    char banks_dir[1024];
} mm_ctx;

/* --------------------------------------------------------------------------
 *  stages (each returns a count of assets produced, or -1 on error)
 * -------------------------------------------------------------------------- */
int mm_stage_prepare(const mm_opts *o, mm_ctx *c);   /* ROM prep */
long mm_stage_sounds(const mm_opts *o, mm_ctx *c);   /* -> FLAC  */
long mm_stage_images(const mm_opts *o, mm_ctx *c);   /* -> PNG   */
long mm_stage_fonts(const mm_opts *o, mm_ctx *c);    /* -> PNG+CSV */
long mm_stage_messages(const mm_opts *o, mm_ctx *c); /* -> CSV   */
long mm_stage_tables(const mm_opts *o, mm_ctx *c);   /* -> CSV   */

/* --------------------------------------------------------------------------
 *  DCS play set (used_ids.c) — the sound ids the loaded game can actually play,
 *  walked out of the image's own `*_acl` audio tables. Game-agnostic: no title
 *  is hard-coded and no precomputed list is shipped, so this also carries to
 *  another Pinball 2000 title. tests/used_ids_test.cpp pins the RfM result.
 * -------------------------------------------------------------------------- */
/* Collect the play set, ascending. Returns the id count (a return > max means
 * ids[] was too small — the first `max` were stored), or -1 if the image has no
 * usable acl tables. `n_acls` (may be NULL) receives the number of acls walked. */
long mm_used_ids_derive(const mm_ctx *c, uint16_t *ids, size_t max, long *n_acls);
/* Write the list in the decoder's DCS_USED_IDS format ("0xHEX" per line),
 * `header` (may be NULL) emitted verbatim first. Returns n, or -1. */
long mm_used_ids_write(const char *path, const uint16_t *ids, long n, const char *header);

/* ROM prep primitives (1:1 with the Python tools) — exposed for prepare + tests */
/* `prefix` pins the chip-filename prefix; NULL or "" resolves it from the directory, which
   also covers prefix-less dumps ("U100.ROM" as a chip reader writes them). */
int mm_deinterleave(const char *roms_dir, const char *out_dir, const char *prefix, int verbose);
int mm_assemble_flash(const char *bundle_dir, mm_buf *out); /* fills *out (owned) */
void mm_resolve_dcs_roms(const mm_opts *o, char u109[1024], char u110[1024], char flash[1024]);

/* --scan-images (scan.c): find anim structures in the image banks by their shape and
 * synthesise the game.rom pointer + `scan_*_ptr` symbol each walker expects, so a
 * chips-only dump (no update bundle, hence no symbol table) still yields its graphics.
 * Appends to c->game_rom and c->symtab; returns how many structures it added. */
long mm_scan_images(const mm_opts *o, mm_ctx *c);

/* asset decoders */
long mm_render_anims(const mm_opts *o, mm_ctx *c, const char *out_images);
long mm_render_movies(const mm_opts *o, mm_ctx *c, const char *out_images);
long mm_extract_fonts(const mm_opts *o, mm_ctx *c, const char *out_fonts);
long mm_extract_messages(const mm_opts *o, mm_ctx *c, const char *out_csv);
/* adjustments.csv (and, with out_enums_csv, adjustment_enums.csv) from the X_enode /
   _X / _d_X symbol triples -- see adjustments.c. Returns the row count, -1 on error. */
long mm_extract_adjustments(const mm_opts *o, mm_ctx *c, const char *out_csv,
                            const char *out_enums_csv);

/* --------------------------------------------------------------------------
 *  version identification
 * -------------------------------------------------------------------------- */
typedef struct {
    char game_version[32]; /* e.g. "1.80" from the game.rom / bundle name */
    char flash_md5[33];
    char u109_md5[33];
    char u110_md5[33];
    int dcs_consistent; /* -1 unknown, 0 mismatch, 1 self-consistent   */
} mm_idinfo;

void mm_identify(const mm_opts *o, mm_ctx *c, mm_idinfo *id);

/* --------------------------------------------------------------------------
 *  ZIP container (zip.c) — STORE-method writer, dependency-free
 * -------------------------------------------------------------------------- */
typedef struct mm_zip mm_zip;
mm_zip *mm_zip_create(const char *path);
int mm_zip_add_file(mm_zip *z, const char *fs_path, const char *arc);
int mm_zip_finish(mm_zip *z);
/* Pack every file under `dir` into `zip_path` with dir-relative archive names.
 * Returns the number of files added, or -1 on error. */
long mm_zip_pack_dir(const char *dir, const char *zip_path);
/* Recursively delete a directory tree. */
int mm_rmtree(const char *dir);
/* 1 if `dir` contains only names an extraction run writes — the guard to check before
 * pointing mm_rmtree at a directory the user named. */
int mm_is_asset_tree(const char *dir);

/* --------------------------------------------------------------------------
 *  shared asset helpers (assets_common.c)
 * -------------------------------------------------------------------------- */
/* Read through the game ROM (link base 0x100000) first, then the address space
 * (banks/flash) — 1:1 with the Python tools' rd_u32/rd_bytes. `def` on miss. */
uint32_t mm_rd_u32(const mm_buf *grom, const mm_aspace *as, uint32_t addr, uint32_t def);
/* Return a pointer to `n` readable bytes at guest `addr`, or NULL. */
const uint8_t *mm_rd_bytes(const mm_buf *grom, const mm_aspace *as, uint32_t addr, size_t n);

/* Spectral flatness (Wiener entropy) of `n` samples — the DCS consistency metric
 * (clean music/voiced ~0.03..0.18, mismatched white noise ~0.7..0.85). spectral.c */
double mm_spectral_flatness(const double *x, int n);

/* RGB555 -> packed 8-bit. TRANSPARENT (0x7C1F) key handled by the RGBA path. */
#define MM_TRANSPARENT 0x7C1F
void mm_rgb555_to_rgb888(const uint16_t *px, size_t n, uint8_t *out /*n*3*/);
void mm_rgb555_to_rgba(const uint16_t *px, size_t n, uint8_t *out /*n*4*/);

/* PNG output via vendored stb_image_write (png.c). Returns 0 on success. */
int mm_write_png(const char *path, int w, int h, int comp, const uint8_t *pixels);

/* Python-csv-compatible row writer (QUOTE_MINIMAL, lineterminator "\r\n").
 * Fields joined by ',', quoted only when they contain , " CR or LF; embedded
 * quotes doubled. `fields` is an array of `n` NUL-terminated strings. */
void mm_csv_row(FILE *f, const char *const *fields, int n);

/* --------------------------------------------------------------------------
 *  logging / util
 * -------------------------------------------------------------------------- */
extern int mm_verbosity;
void mm_log(int level, const char *fmt, ...); /* prints if mm_verbosity>=level */
void mm_info(const char *fmt, ...);           /* always (stdout) */
void mm_warn(const char *fmt, ...);           /* always (stderr) */
void mm_die(const char *fmt, ...);            /* prints + exit(1) */
int mm_mkdir_p(const char *path);
int mm_path_exists(const char *path);
void mm_md5_hex(const void *data, size_t len, char out[33]);
const char *mm_human_size(uint64_t bytes, char buf[32]);

#endif /* MARSMINER_H */
