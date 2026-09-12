/* scan.c — --scan-images: find the anim structures by shape instead of by name.
 *
 * Every other stage here is symbol-driven: the walkers start at a `*_ptr` symbol from
 * the update bundle's symbols.rom, read the pointer word out of game.rom and follow it
 * into the image ROMs. A chips-only dump has no bundle — no game.rom, no symbol table —
 * and therefore extracts nothing at all, even though its image banks are intact and in
 * exactly the same format. That is the case this covers.
 *
 * The scan walks the image window looking for the anim header's own shape and then
 * synthesises the two things the walkers need: a pointer word appended to game.rom and a
 * `scan_<addr>_<w>x<h>_<n>f_ptr` symbol addressing it. mm_render_anims then runs
 * unchanged — this file decides *what* to render, never *how*.
 *
 * Three kinds of resource are found, each by its own shape (all little-endian):
 *
 *   anim    +0x00 u32 nframes
 *           +0x1C u32 frames_ptr -> nframes * 0x18-byte frame descriptors:
 *                                     +0x00 type  +0x04 w  +0x08 h
 *                                     +0x0C, +0x10 zero    +0x14 frame data pointer
 *           type is one of the six anim codecs (0x00/0x01/0x14/0x15/0x16/0x17).
 *
 *   movie   the same header and the same 0x18-byte descriptors, but every type is 0x1e
 *           (the delta/S3TC codec) and the dimensions are the movie decoder's: w <= 1024,
 *           h <= 512, both multiples of 4, and matching the header's own w/h at +0x08/+0x0C.
 *           That single type value is what separates a movie from an anim.
 *
 *   font    +0x00 u32 glyph count   +0x10 u32 default advance
 *           +0x14 u32 char array -> count * 0x1C-byte glyphs:
 *                                     +0x00 u32 code  +0x04 i32 xoff  +0x08 i32 yoff
 *                                     +0x0C u32 w     +0x10 u32 h     +0x14 i32 advance
 *                                     +0x18 bitmap pointer (w*h bytes, one per pixel)
 *           The codes ascend across the array, which is the constraint that carries this one.
 *
 * Headers are not aligned — RfM's anim_beam_ptr resolves to 0x150312b7 — so the walk is
 * bytewise, with per-kind byte prefilters carrying the cost.
 *
 * What keeps the false-positive rate down is that the checks are structural and mutually
 * constraining: a candidate must have a plausible frame count, a frames_ptr that lands in
 * mapped ROM, and then *every* one of its first frames must carry one of the six known
 * codec ids, plausible dimensions, two zero reserved words and a data pointer that is
 * itself mapped. Calibration on RfM, whose shipped symbol table gives an independent
 * answer, is in README.md.
 *
 * Names are the one thing a dump without a symbol table cannot give back: the output is
 * addressed, not named.
 */
#include "marsminer.h"

#include <stdlib.h>
#include <string.h>

#define SCAN_MAX_FRAMES 10000u /* nframes sanity bound (RfM's largest anim: 610)  */
#define SCAN_PROBE 4u          /* frame descriptors validated per candidate       */
#define SCAN_MIN_DIM 2u
#define SCAN_MAX_DIM 2048u /* same bound mm_render_anims applies per frame     */
#define SCAN_MOVIE_TYPE 0x1eu
#define SCAN_MOVIE_MAX_W 1024u /* the movie decoder's own limits (movies.c)       */
#define SCAN_MOVIE_MAX_H 512u
#define SCAN_FONT_MAX_GLYPHS 512u /* fonts.c accepts < 1024; a real font is < 128 */
#define SCAN_FONT_PROBE 6u
/* Wider than fonts.c's own per-glyph bound of 64: a glyph outside that is skipped by the
   extractor, not rejected, and one such glyph must not disqualify a whole font — RfM's
   font_serp_33 has a 73x26 one. The bitmap still has to map w*h bytes, which is what keeps
   an implausible size from passing. */
#define SCAN_FONT_MAX_DIM 256u
#define SCAN_FONT_MAX_OFF 64
#define SCAN_FONT_MAX_ADV 128

typedef enum { SK_ANIM, SK_MOVIE, SK_FONT } scankind;

typedef struct {
    uint32_t addr, count, w, h; /* count = frames, or glyphs for a font */
    scankind kind;
} scanhit;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int frame_type_known(uint32_t t) {
    /* T_8RAW, T_8LRLE, T_15RAW, T_15LRLE, T_64DICT, T_32DICT (anims.c) */
    return t == 0x00 || t == 0x01 || t == 0x14 || t == 0x15 || t == 0x16 || t == 0x17;
}

/* Resolve `ptr` to at least `need` mapped bytes, using the renderer's own rules (image
   banks or, when a bundle supplied one, the BAR3 flash). NULL if it does not resolve. */
static const uint8_t *mapped(const mm_aspace *as, uint32_t ptr, size_t need, size_t *off) {
    size_t o, avail;
    const uint8_t *b = mm_region(as, ptr, &o, &avail);
    if (!b || avail < need)
        return NULL;
    if (off)
        *off = o;
    return b;
}

/* Signed 32-bit read (the font metrics are signed). */
static int32_t rds32(const uint8_t *p) {
    return (int32_t)rd32(p);
}

/* Does an anim or movie header live at `addr`? The two share a layout and differ only in the
   frame type, so one walk serves both. On success fills the out params from the first frame. */
static int header_shape_ok(const mm_aspace *as, uint32_t addr, int movie, uint32_t *nf_out,
                           uint32_t *w_out, uint32_t *h_out) {
    size_t ho;
    const uint8_t *hbase = mapped(as, addr, 0x20, &ho);
    if (!hbase)
        return 0;
    const uint8_t *h = hbase + ho;

    uint32_t nf = rd32(h);
    if (nf < 1 || nf >= SCAN_MAX_FRAMES)
        return 0;

    uint32_t frames_ptr = rd32(h + 0x1C);
    uint32_t probe = nf < SCAN_PROBE ? nf : SCAN_PROBE;
    size_t fo;
    const uint8_t *fbase = mapped(as, frames_ptr, (size_t)probe * 0x18, &fo);
    if (!fbase)
        return 0;

    for (uint32_t i = 0; i < probe; i++) {
        const uint8_t *e = fbase + fo + (size_t)i * 0x18;
        uint32_t t = rd32(e), w = rd32(e + 4), fh = rd32(e + 8);
        if (movie) {
            /* One codec, and mm_render_movies' own dimension rules — including that the
               descriptor repeats the header's w/h, which no accidental match manages. */
            if (t != SCAN_MOVIE_TYPE)
                return 0;
            if (w > SCAN_MOVIE_MAX_W || fh > SCAN_MOVIE_MAX_H || (w % 4) || (fh % 4))
                return 0;
            if (w != rd32(h + 0x08) || fh != rd32(h + 0x0C))
                return 0;
        } else {
            if (!frame_type_known(t))
                return 0;
        }
        if (w < SCAN_MIN_DIM || w > SCAN_MAX_DIM || fh < SCAN_MIN_DIM || fh > SCAN_MAX_DIM)
            return 0;
        if (rd32(e + 0x0C) || rd32(e + 0x10)) /* reserved words are zero in every real frame */
            return 0;
        if (!mapped(as, rd32(e + 0x14), 1, NULL)) /* frame data must resolve */
            return 0;
        if (i == 0) {
            *w_out = w;
            *h_out = fh;
        }
    }
    *nf_out = nf;
    return 1;
}

/* Does a FontData live at `addr`? The glyph array is what decides: every probed entry must
   carry an ascending character code, metrics inside the extractor's own bounds, and a bitmap
   of exactly w*h mapped bytes. */
static int font_shape_ok(const mm_aspace *as, uint32_t addr, uint32_t *count_out) {
    size_t ho;
    const uint8_t *hbase = mapped(as, addr, 0x18, &ho);
    if (!hbase)
        return 0;
    const uint8_t *h = hbase + ho;

    uint32_t count = rd32(h);
    if (count < 2 || count > SCAN_FONT_MAX_GLYPHS)
        return 0;

    uint32_t chars = rd32(h + 0x14);
    size_t co;
    const uint8_t *cbase = mapped(as, chars, (size_t)count * 0x1C, &co);
    if (!cbase)
        return 0;

    uint32_t probe = count < SCAN_FONT_PROBE ? count : SCAN_FONT_PROBE;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < probe; i++) {
        const uint8_t *e = cbase + co + (size_t)i * 0x1C;
        uint32_t code = rd32(e), w = rd32(e + 0x0C), gh = rd32(e + 0x10), bmp = rd32(e + 0x18);
        int32_t xoff = rds32(e + 0x04), yoff = rds32(e + 0x08), adv = rds32(e + 0x14);
        if (code == 0 || code > 0xFFFFu)
            return 0;
        if (i && code <= prev) /* the array is ordered by character code */
            return 0;
        prev = code;
        if (w < 1 || w > SCAN_FONT_MAX_DIM || gh < 1 || gh > SCAN_FONT_MAX_DIM)
            return 0;
        if (xoff < -SCAN_FONT_MAX_OFF || xoff > SCAN_FONT_MAX_OFF || yoff < -SCAN_FONT_MAX_OFF ||
            yoff > SCAN_FONT_MAX_OFF || adv < -SCAN_FONT_MAX_ADV || adv > SCAN_FONT_MAX_ADV)
            return 0;
        if (!mapped(as, bmp, (size_t)w * gh, NULL))
            return 0;
    }
    *count_out = count;
    return 1;
}

/* The anim pointers the shipped symbols already reach. Only non-empty when a bundle was
   given, which is exactly when --scan-images is an addition rather than the whole input. */
static uint32_t *known_anim_ptrs(const mm_ctx *c, size_t *n_out) {
    *n_out = 0;
    if (!c->game_rom.data || !c->symtab.count)
        return NULL;
    uint32_t *k = (uint32_t *)malloc(c->symtab.count * sizeof *k);
    if (!k)
        return NULL;
    size_t n = 0;
    for (size_t i = 0; i < c->symtab.count; i++) {
        const char *nm = c->symtab.syms[i].name;
        size_t nl = strlen(nm);
        if (!(nl > 4 && strcmp(nm + nl - 4, "_ptr") == 0))
            continue;
        uint32_t a = c->symtab.syms[i].addr;
        if (a < MM_GBASE || (size_t)(a - MM_GBASE) + 4 > c->game_rom.len)
            continue;
        k[n++] = mm_u32(&c->game_rom, a - MM_GBASE);
    }
    *n_out = n;
    return k;
}

long mm_scan_images(const mm_opts *o, mm_ctx *c) {
    const mm_aspace *as = &c->aspace;
    if (!as->image || as->image_len < 0x20) {
        mm_warn("--scan-images: no image banks loaded — nothing to scan");
        return 0;
    }

    size_t nknown = 0;
    uint32_t *known = known_anim_ptrs(c, &nknown);

    scanhit *hits = NULL;
    size_t nhits = 0, cap = 0;
    long dups = 0;

    const uint8_t *img = as->image;
    const size_t n = as->image_len;
    /* Does a byte select a window that exists? A table pointer's top byte must. */
    #define TOP_OK(t) (((t) >= (MM_IMG_BASE >> 24) && (t) < (MM_IMG_END >> 24)) || (t) == as->flash_top)
    for (size_t off = 0; off + 0x20 <= n; off++) {
        /* Prefilter before any structural walk: the leading count is a small positive number
           (its two high bytes zero) and the table pointer's top byte selects a real window.
           Three byte tests per candidate kind is what makes a bytewise 64 MiB scan cheap. */
        if (img[off + 2] || img[off + 3])
            continue;
        uint16_t lead = (uint16_t)(img[off] | img[off + 1] << 8);
        if (!lead)
            continue;

        uint32_t addr = (uint32_t)(MM_IMG_BASE + off);
        uint32_t cnt = 0, w = 0, h = 0;
        scankind kind;
        if (lead < SCAN_MAX_FRAMES && TOP_OK(img[off + 0x1F]) &&
            header_shape_ok(as, addr, 0, &cnt, &w, &h))
            kind = SK_ANIM;
        else if (lead < SCAN_MAX_FRAMES && TOP_OK(img[off + 0x1F]) &&
                 header_shape_ok(as, addr, 1, &cnt, &w, &h))
            kind = SK_MOVIE;
        else if (lead <= SCAN_FONT_MAX_GLYPHS && TOP_OK(img[off + 0x17]) &&
                 font_shape_ok(as, addr, &cnt))
            kind = SK_FONT;
        else
            continue;

        int dup = 0;
        for (size_t k = 0; k < nknown; k++)
            if (known[k] == addr) {
                dup = 1;
                break;
            }
        if (dup) {
            dups++;
            continue;
        }

        if (nhits == cap) {
            size_t ncap = cap ? cap * 2 : 128;
            scanhit *nh = (scanhit *)realloc(hits, ncap * sizeof *nh);
            if (!nh) {
                free(hits);
                free(known);
                mm_warn("--scan-images: out of memory");
                return -1;
            }
            hits = nh;
            cap = ncap;
        }
        hits[nhits].addr = addr;
        hits[nhits].count = cnt;
        hits[nhits].w = w;
        hits[nhits].h = h;
        hits[nhits].kind = kind;
        nhits++;
    }
    #undef TOP_OK
    free(known);

    if (!nhits) {
        free(hits);
        mm_info("scan: no image structures found in the banks");
        return 0;
    }

    /* Append the pointer table to game.rom. c->game_len keeps the bundle's own extent, so
       stages that bound-check against "the end of the ROM" do not see this table. */
    size_t old = c->game_rom.len;
    size_t pad = (4u - (old & 3u)) & 3u; /* keep the synthetic addresses 4-byte aligned */
    size_t need = old + pad + nhits * 4;
    uint8_t *g = (uint8_t *)realloc(c->game_rom.data, need);
    if (!g) {
        free(hits);
        mm_warn("--scan-images: out of memory");
        return -1;
    }
    memset(g + old, 0, pad);
    for (size_t i = 0; i < nhits; i++) {
        uint8_t *p = g + old + pad + i * 4;
        p[0] = (uint8_t)hits[i].addr;
        p[1] = (uint8_t)(hits[i].addr >> 8);
        p[2] = (uint8_t)(hits[i].addr >> 16);
        p[3] = (uint8_t)(hits[i].addr >> 24);
    }
    c->game_rom.data = g;
    c->game_rom.len = need;

    /* Append the matching symbols. */
    mm_sym *syms = (mm_sym *)realloc(c->symtab.syms, (c->symtab.count + nhits) * sizeof *syms);
    if (!syms) {
        free(hits);
        mm_warn("--scan-images: out of memory");
        return -1;
    }
    c->symtab.syms = syms;
    long added = 0, frames = 0, n_anim = 0, n_movie = 0, n_font = 0;
    for (size_t i = 0; i < nhits; i++) {
        /* The names carry the prefix and suffix each walker selects on: mm_render_anims takes
           any *_ptr, mm_render_movies only movie_*_ptr, mm_extract_fonts only
           font_*_data_ptr. The address in the middle is all the identity a dump without a
           symbol table can offer. */
        char nm[128];
        if (hits[i].kind == SK_FONT)
            snprintf(nm, sizeof nm, "font_scan_%08x_%ug_data_ptr", hits[i].addr, hits[i].count);
        else
            snprintf(nm, sizeof nm, "%sscan_%08x_%ux%u_%uf_ptr",
                     hits[i].kind == SK_MOVIE ? "movie_" : "", hits[i].addr, hits[i].w, hits[i].h,
                     hits[i].count);
        char *dup = (char *)malloc(strlen(nm) + 1);
        if (!dup)
            break;
        strcpy(dup, nm);
        syms[c->symtab.count].addr = (uint32_t)(MM_GBASE + old + pad + i * 4);
        syms[c->symtab.count].name = dup;
        c->symtab.count++;
        added++;
        if (hits[i].kind == SK_FONT) {
            n_font++;
            mm_log(2, "  scan: 0x%08x  font, %u glyph(s)", hits[i].addr, hits[i].count);
        } else {
            frames += hits[i].count;
            if (hits[i].kind == SK_MOVIE)
                n_movie++;
            else
                n_anim++;
            mm_log(2, "  scan: 0x%08x  %-5s %ux%u  %u frame(s)", hits[i].addr,
                   hits[i].kind == SK_MOVIE ? "movie" : "anim", hits[i].w, hits[i].h,
                   hits[i].count);
        }
    }
    free(hits);

    mm_log(1, "scan: %ld unnamed structure(s) — %ld anim(s), %ld movie(s), %ld font(s); "
              "%ld frame(s)",
           added, n_anim, n_movie, n_font, frames);
    if (dups)
        mm_log(1, "scan: %ld further structure(s) already reachable from the symbol table", dups);
    (void)o;
    return added;
}
