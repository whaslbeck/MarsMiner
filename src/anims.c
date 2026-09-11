/* anims.c — decode RFM animation frames to PNG.  1:1 with render_anims.py
 * (+ walk_anims.py struct layout). Implements the fully-understood codecs:
 *   0x14 15bit raw, 0x15 15bit L-RLE, 0x00 8bit raw, 0x01 8bit L-RLE,
 *   0x16 64col dict-RLE, 0x17 32col dict-RLE.
 * Output: RGBA PNG with the 0x7C1F colour key transparent (extract.py --alpha).
 */
#include "marsminer.h"

#include <stdlib.h>
#include <string.h>

/* Animation:       u32 nframes@0x00, u32 palcount@0x10, ptr pal@0x14, ptr frames@0x1C
 * Animation_Frame: u32 type@0x00, u32 w@0x04, u32 h@0x08, ptr data@0x14  (0x18 bytes) */

/* sequential LE reader over a bounded window */
typedef struct {
    const uint8_t *d;
    size_t o, n;
    int oob;
} rdr;
static uint16_t r_u16(rdr *r) {
    if (r->o + 2 > r->n) {
        r->oob = 1;
        return 0;
    }
    uint16_t v = (uint16_t)(r->d[r->o] | r->d[r->o + 1] << 8);
    r->o += 2;
    return v;
}
static uint8_t r_u8(rdr *r) {
    if (r->o + 1 > r->n) {
        r->oob = 1;
        return 0;
    }
    return r->d[r->o++];
}

/* ---- palette (8-bit paths): RGB555 words indexed by palette index --------- */
static void build_palette(const mm_aspace *as, uint32_t anim_ptr, uint16_t *pal /*>=256*/) {
    memset(pal, 0, 256 * sizeof *pal);
    uint32_t palcount = mm_as_u32(as, anim_ptr + 0x10, 0);
    uint32_t pal_ptr = mm_as_u32(as, anim_ptr + 0x14, 0);
    size_t off, avail;
    const uint8_t *b = mm_region(as, pal_ptr, &off, &avail);
    if (!b)
        return;
    size_t p = off + 3; /* entry 0 skipped (transparent) */
    for (uint32_t i = 1; i < palcount && i < 256; i++) {
        if (p + 3 > off + avail)
            break;
        uint8_t r = b[p], g = b[p + 1], bl = b[p + 2];
        pal[i] = (uint16_t)(((r >> 1) << 10) | ((g >> 1) << 5) | (bl >> 1));
        p += 3;
    }
}

/* ---- pixel codecs -------------------------------------------------------- */
static void dec_15bit_raw(rdr *r, uint16_t *px, size_t n) {
    for (size_t i = 0; i < n && !r->oob; i++)
        px[i] = r_u16(r);
}
static void dec_15bit_lrle(rdr *r, uint16_t *px, size_t n) {
    size_t i = 0, guard = 0, gmax = n * 4;
    while (i < n && guard < gmax) {
        guard++;
        uint16_t ctrl = r_u16(r);
        if (r->oob)
            break;
        uint16_t count = ctrl & 0x7FFF;
        if (ctrl < 0x8000) { /* RUN */
            uint16_t pix = r_u16(r);
            if (r->oob)
                break;
            for (uint16_t k = 0; k < count; k++)
                if (i < n)
                    px[i++] = pix;
        } else { /* LITERAL */
            for (uint16_t k = 0; k < count; k++) {
                uint16_t pix = r_u16(r);
                if (r->oob)
                    break;
                if (i < n)
                    px[i++] = pix;
            }
        }
    }
}
static void dec_8bit_raw(rdr *r, uint16_t *px, size_t n, const uint16_t *pal) {
    for (size_t i = 0; i < n && !r->oob; i++)
        px[i] = pal[r_u8(r)];
}
static void dec_8bit_lrle(rdr *r, uint16_t *px, size_t n, const uint16_t *pal) {
    size_t i = 0, guard = 0, gmax = n * 4;
    while (i < n && guard < gmax) {
        guard++;
        uint16_t ctrl = r_u16(r);
        if (r->oob)
            break;
        uint16_t count = ctrl & 0x7FFF;
        if (ctrl < 0x8000) {
            uint16_t pix = pal[r_u8(r)];
            if (r->oob)
                break;
            for (uint16_t k = 0; k < count; k++)
                if (i < n)
                    px[i++] = pix;
        } else {
            for (uint16_t k = 0; k < count; k++) {
                uint16_t pix = pal[r_u8(r)];
                if (r->oob)
                    break;
                if (i < n)
                    px[i++] = pix;
            }
        }
    }
}

/* ---- dict-RLE (0x16/0x17) ------------------------------------------------ */
/* Bit reader: per-byte LSB first. */
typedef struct {
    const uint8_t *d;
    size_t o, n;
    int bit;
} bitr;
static int b_get(bitr *b) {
    if (b->o >= b->n)
        return 0;
    int v = (b->d[b->o] >> b->bit) & 1;
    if (++b->bit == 8) {
        b->bit = 0;
        b->o++;
    }
    return v;
}

static void load_dict(const mm_aspace *as, uint32_t anim_ptr, int n_singles,
                      uint16_t *singles /*n_singles*/, uint16_t *pairs /*2*npairs*/, int *npairs) {
    *npairs = 0;
    memset(singles, 0, (size_t)n_singles * sizeof *singles);
    uint32_t pal_ptr = mm_as_u32(as, anim_ptr + 0x14, 0);
    size_t off, avail;
    const uint8_t *b = mm_region(as, pal_ptr, &off, &avail);
    if (!b)
        return;
    int total = (n_singles == 32) ? 288 : 192; /* u16 words */
    uint16_t words[288];
    for (int i = 0; i < total; i++) {
        size_t p = off + (size_t)i * 2;
        words[i] = (p + 2 <= off + avail) ? (uint16_t)(b[p] | b[p + 1] << 8) : 0;
    }
    for (int i = 0; i < n_singles; i++)
        singles[i] = words[i];
    int j = n_singles, k = 0;
    while (j + 1 < total) {
        pairs[k * 2] = words[j];
        pairs[k * 2 + 1] = words[j + 1];
        j += 2;
        k++;
    }
    *npairs = k;
}

static int emit_pair(uint16_t *px, size_t row, int x, int w, const uint16_t *pairs, int npairs,
                     int pidx, int bit) {
    uint16_t p0 = 0, p1 = 0;
    if (pidx < npairs) {
        p0 = pairs[pidx * 2];
        p1 = pairs[pidx * 2 + 1];
    }
    uint16_t a = bit ? p1 : p0;
    uint16_t bb = bit ? p0 : p1;
    if (x < w)
        px[row + x++] = a;
    if (x < w)
        px[row + x++] = bb;
    return x;
}

static void dec_32col_dictrle(const uint8_t *data, size_t off, size_t avail, int w, int h,
                              const uint16_t *singles, const uint16_t *pairs, int npairs,
                              uint16_t *px) {
    if (avail < 2)
        return;
    uint16_t header = (uint16_t)(data[off] | data[off + 1] << 8);
    size_t op = off + header;
    bitr bits = {data, off + 2, off + avail, 0};
    for (int y = 0; y < h; y++) {
        int x = 0;
        size_t row = (size_t)y * w;
        int guard = 0, gmax = w * 6;
        while (x < w && guard < gmax) {
            guard++;
            if (op >= off + avail)
                return;
            uint8_t b0 = data[op++];
            if (b0 < 0x80) {
                int cat = b0 & 0xE0, idx = b0 & 0x1F;
                if (cat == 0x00) {
                    if (x < w)
                        px[row + x++] = singles[idx];
                } else if (cat == 0x20) {
                    for (int k = 0; k < 2; k++)
                        if (x < w)
                            px[row + x++] = singles[idx];
                } else if (cat == 0x40) {
                    for (int k = 0; k < idx + 3; k++)
                        if (x < w)
                            px[row + x++] = singles[0];
                } else {
                    if (op >= off + avail)
                        return;
                    int nn = data[op++] + 3;
                    for (int k = 0; k < nn; k++)
                        if (x < w)
                            px[row + x++] = singles[idx];
                }
            } else {
                x = emit_pair(px, row, x, w, pairs, npairs, b0 & 0x7F, b_get(&bits));
            }
        }
    }
}

static void dec_64col_dictrle(const uint8_t *data, size_t off, size_t avail, int w, int h,
                              const uint16_t *singles, const uint16_t *pairs, int npairs,
                              uint16_t *px) {
    if (avail < 2)
        return;
    uint16_t header = (uint16_t)(data[off] | data[off + 1] << 8);
    size_t op = off + header;
    bitr bits = {data, off + 2, off + avail, 0};
    for (int y = 0; y < h; y++) {
        int x = 0;
        size_t row = (size_t)y * w;
        int guard = 0, gmax = w * 6;
        while (x < w && guard < gmax) {
            guard++;
            if (op >= off + avail)
                return;
            uint8_t b0 = data[op++];
            int cat = b0 & 0xC0, idx = b0 & 0x3F;
            if (cat == 0x00) {
                if (x < w)
                    px[row + x++] = singles[idx];
            } else if (cat == 0x40) {
                for (int k = 0; k < 2; k++)
                    if (x < w)
                        px[row + x++] = singles[idx];
            } else if (cat == 0x80) {
                x = emit_pair(px, row, x, w, pairs, npairs, idx, b_get(&bits));
            } else {
                if (op >= off + avail)
                    return;
                int nn = data[op++] + 3;
                for (int k = 0; k < nn; k++)
                    if (x < w)
                        px[row + x++] = singles[idx];
            }
        }
    }
}

/* ---- frame type ids ------------------------------------------------------ */
enum {
    T_8RAW = 0x00,
    T_8LRLE = 0x01,
    T_15RAW = 0x14,
    T_15LRLE = 0x15,
    T_64DICT = 0x16,
    T_32DICT = 0x17
};

static void write_frame_png(const char *dir, int fi, int w, int h, const uint16_t *px) {
    char path[1200];
    snprintf(path, sizeof path, "%s/frame%03d.png", dir, fi);
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba)
        return;
    mm_rgb555_to_rgba(px, (size_t)w * h, rgba);
    mm_write_png(path, w, h, 4, rgba);
    free(rgba);
}

long mm_render_anims(const mm_opts *o, mm_ctx *c, const char *out_images) {
    const mm_symtab *st = &c->symtab;
    const mm_aspace *as = &c->aspace;
    const mm_buf *grom = &c->game_rom;
    long ok = 0;
    (void)o;

    for (size_t si = 0; si < st->count; si++) {
        const char *name = st->syms[si].name;
        size_t nl = strlen(name);
        /* Any *_ptr symbol is a candidate; the structural checks below decide. Selecting on the
         * "anim_"/"pict_" name prefixes instead silently skipped every image the ROM happens to
         * name differently — the seven menu_background_* service-menu backdrops (the Bally and
         * Williams logos and scripts, the playfield map, the lamp grid, the switch grid),
         * image_cookie_ptr and system_video_test_align_ptr. A name is not a type.
         *
         * Nothing is taken on trust: the pointer must land in a mapped region, nframes must be
         * sane, the frame table must be mapped, every frame's type must be one of the six known
         * codecs and its dimensions must be plausible. A non-image pointer (Game_ptr,
         * _impure_ptr, no_resource_alloc_head_ptr, system_boot_data_ptr) fails those and is
         * dropped without writing anything. */
        if (!(nl > 4 && strcmp(name + nl - 4, "_ptr") == 0))
            continue;

        uint32_t gaddr = st->syms[si].addr;
        if (gaddr < MM_GBASE || gaddr - MM_GBASE + 4 > grom->len)
            continue;
        uint32_t anim_ptr = mm_u32(grom, gaddr - MM_GBASE);
        size_t aoff, aavail;
        if (!mm_region(as, anim_ptr, &aoff, &aavail))
            continue;

        uint32_t nframes = mm_as_u32(as, anim_ptr, 0);
        uint32_t frames_ptr = mm_as_u32(as, anim_ptr + 0x1C, 0);
        if (!nframes || !(nframes < 100000))
            continue;
        size_t foff, favail;
        const uint8_t *fb = mm_region(as, frames_ptr, &foff, &favail);
        if (!fb)
            continue;

        uint16_t pal[256];
        int pal_built = 0;
        char outdir[1100];
        snprintf(outdir, sizeof outdir, "%s/%s", out_images, name);
        int made_dir = 0;

        for (uint32_t fi = 0; fi < nframes; fi++) {
            size_t base = foff + (size_t)fi * 0x18;
            if (base + 0x18 > foff + favail)
                break;
            uint32_t t =
                (uint32_t)(fb[base] | fb[base + 1] << 8 | fb[base + 2] << 16 | fb[base + 3] << 24);
            uint32_t w = (uint32_t)(fb[base + 4] | fb[base + 5] << 8 | fb[base + 6] << 16 |
                                    fb[base + 7] << 24);
            uint32_t h = (uint32_t)(fb[base + 8] | fb[base + 9] << 8 | fb[base + 10] << 16 |
                                    fb[base + 11] << 24);
            uint32_t dptr = (uint32_t)(fb[base + 0x14] | fb[base + 0x15] << 8 |
                                       fb[base + 0x16] << 16 | fb[base + 0x17] << 24);

            if (t != T_8RAW && t != T_8LRLE && t != T_15RAW && t != T_15LRLE && t != T_64DICT &&
                t != T_32DICT)
                continue;
            if (!(w > 0 && w <= 2048 && h > 0 && h <= 2048))
                continue;

            size_t doff, davail;
            const uint8_t *db = mm_region(as, dptr, &doff, &davail);
            if (!db)
                continue;

            size_t npx = (size_t)w * h;
            uint16_t *px = (uint16_t *)calloc(npx, sizeof *px);
            if (!px)
                continue;

            if (t == T_15RAW) {
                rdr r = {db, doff, doff + davail, 0};
                dec_15bit_raw(&r, px, npx);
            } else if (t == T_15LRLE) {
                rdr r = {db, doff, doff + davail, 0};
                dec_15bit_lrle(&r, px, npx);
            } else if (t == T_32DICT || t == T_64DICT) {
                int nsing = (t == T_32DICT) ? 32 : 64;
                uint16_t singles[64], pairs[288];
                int npairs = 0;
                load_dict(as, anim_ptr, nsing, singles, pairs, &npairs);
                if (t == T_32DICT)
                    dec_32col_dictrle(db, doff, davail, w, h, singles, pairs, npairs, px);
                else
                    dec_64col_dictrle(db, doff, davail, w, h, singles, pairs, npairs, px);
            } else { /* 8-bit paths */
                if (!pal_built) {
                    build_palette(as, anim_ptr, pal);
                    pal_built = 1;
                }
                rdr r = {db, doff, doff + davail, 0};
                if (t == T_8RAW)
                    dec_8bit_raw(&r, px, npx, pal);
                else
                    dec_8bit_lrle(&r, px, npx, pal);
            }

            if (!made_dir) {
                mm_mkdir_p(outdir);
                made_dir = 1;
            }
            write_frame_png(outdir, (int)fi, (int)w, (int)h, px);
            free(px);
            ok++;
        }
    }
    return ok;
}
