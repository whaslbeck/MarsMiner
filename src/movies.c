/* movies.c — decode RFM "Movie" assets (delta-compressed frames) to PNG.
 * 1:1 with render_movies.py (mirrors the ROM decoder FUN_002d53d0).
 *
 * Movie struct: +0x00 nframes, +0x08 w, +0x0c h, +0x1c descriptor array.
 * Descriptor (0x18 bytes): +0x00 type (0x1e = movie/delta frame), +0x14 data ptr.
 * Frame codec: DXT1/S3TC-like over 4x4 blocks, continuous LSB-first bit stream;
 * per block a 2-bit mode: 1 SKIP, 2 SOLID(15b colour), 0 4COL, 3 2COL.
 * Output: RGB PNG (opaque), one frameNNN.png per frame; SKIP carries prior frame.
 */
#include "marsminer.h"

#include <stdlib.h>
#include <string.h>

/* continuous LSB-first bit reader with a 25+-bit accumulator (as the ROM does) */
typedef struct {
    const uint8_t *d;
    size_t o, n;
    uint32_t acc;
    int bits;
    int oob;
} mbits;
static uint32_t mb_get(mbits *b, int nbits) {
    while (b->bits < nbits) {
        if (b->o >= b->n) {
            b->oob = 1;
            return 0;
        }
        b->acc |= (uint32_t)b->d[b->o++] << b->bits;
        b->bits += 8;
    }
    uint32_t v = b->acc & ((1u << nbits) - 1);
    b->acc >>= nbits;
    b->bits -= nbits;
    return v;
}

static inline int blend(int a, int b) {
    return (2 * a + b) / 3;
}

/* decode one 0x1e frame into px (flat w*h RGB555, carried across frames). */
static int decode_frame(const uint8_t *data, size_t off, size_t avail, int w, int h, uint16_t *px) {
    if (w % 4 || h % 4)
        return -1;
    mbits br = {data, off, off + avail, 0, 0, 0};
    for (int by = 0; by < h; by += 4) {
        for (int bx = 0; bx < w; bx += 4) {
            int mode = mb_get(&br, 2);
            if (br.oob)
                return -1;
            if (mode == 1)
                continue;    /* SKIP */
            if (mode == 2) { /* SOLID */
                uint16_t c = (uint16_t)mb_get(&br, 15);
                if (br.oob)
                    return -1;
                for (int r = 0; r < 4; r++) {
                    size_t row = (size_t)(by + r) * w + bx;
                    px[row] = c;
                    px[row + 1] = c;
                    px[row + 2] = c;
                    px[row + 3] = c;
                }
                continue;
            }
            /* endpoints e0.R,e1.R,e0.G,e1.G,e0.B,e1.B (5 bits each) */
            int e0r = mb_get(&br, 5), e1r = mb_get(&br, 5);
            int e0g = mb_get(&br, 5), e1g = mb_get(&br, 5);
            int e0b = mb_get(&br, 5), e1b = mb_get(&br, 5);
            if (br.oob)
                return -1;
            uint16_t c0 = (uint16_t)((e0r << 10) | (e0g << 5) | e0b);
            uint16_t c1 = (uint16_t)((e1r << 10) | (e1g << 5) | e1b);
            uint16_t pal[4];
            int nbits;
            if (mode == 0) { /* 4-colour interpolated */
                uint16_t c_1 =
                    (uint16_t)((blend(e0r, e1r) << 10) | (blend(e0g, e1g) << 5) | blend(e0b, e1b));
                uint16_t c_2 =
                    (uint16_t)((blend(e1r, e0r) << 10) | (blend(e1g, e0g) << 5) | blend(e1b, e0b));
                pal[0] = c0;
                pal[1] = c_1;
                pal[2] = c_2;
                pal[3] = c1;
                nbits = 2;
            } else { /* mode 3: 2-colour */
                pal[0] = c0;
                pal[1] = c1;
                nbits = 1;
            }
            for (int r = 0; r < 4; r++) {
                size_t row = (size_t)(by + r) * w + bx;
                for (int k = 0; k < 4; k++) {
                    uint32_t idx = mb_get(&br, nbits);
                    if (br.oob)
                        return -1;
                    px[row + k] = pal[idx];
                }
            }
        }
    }
    return 0;
}

long mm_render_movies(const mm_opts *o, mm_ctx *c, const char *out_images) {
    const mm_symtab *st = &c->symtab;
    const mm_aspace *as = &c->aspace;
    const mm_buf *grom = &c->game_rom;
    long ok_frames = 0;
    (void)o;

    for (size_t si = 0; si < st->count; si++) {
        const char *name = st->syms[si].name;
        size_t nl = strlen(name);
        if (!(strncmp(name, "movie_", 6) == 0 && nl > 4 && strcmp(name + nl - 4, "_ptr") == 0))
            continue;

        uint32_t gaddr = st->syms[si].addr;
        if (gaddr < MM_GBASE || gaddr - MM_GBASE + 4 > grom->len)
            continue;
        uint32_t movie = mm_u32(grom, gaddr - MM_GBASE);

        uint32_t nframes = mm_as_u32(as, movie, 0);
        uint32_t w = mm_as_u32(as, movie + 0x08, 0);
        uint32_t h = mm_as_u32(as, movie + 0x0c, 0);
        uint32_t desc = mm_as_u32(as, movie + 0x1c, 0);
        if (!nframes || !desc || !(nframes < 100000) || !(w > 0 && w <= 1024) ||
            !(h > 0 && h <= 512))
            continue;
        if (w % 4 || h % 4)
            continue;
        size_t doff, davail;
        const uint8_t *db = mm_region(as, desc, &doff, &davail);
        if (!db)
            continue;

        char outdir[1100];
        snprintf(outdir, sizeof outdir, "%s/%s", out_images, name);
        int made_dir = 0;

        size_t npx = (size_t)w * h;
        uint16_t *px = (uint16_t *)calloc(npx, sizeof *px); /* frame 0 from black */
        uint8_t *rgb = (uint8_t *)malloc(npx * 3);
        if (!px || !rgb) {
            free(px);
            free(rgb);
            continue;
        }

        for (uint32_t fi = 0; fi < nframes; fi++) {
            size_t d = doff + (size_t)fi * 0x18;
            if (d + 0x18 > doff + davail)
                break; /* descriptor array truncated */
            uint32_t ftype = (uint32_t)(db[d] | db[d + 1] << 8 | db[d + 2] << 16 | db[d + 3] << 24);
            uint32_t dptr = (uint32_t)(db[d + 0x14] | db[d + 0x15] << 8 | db[d + 0x16] << 16 |
                                       db[d + 0x17] << 24);
            size_t foff, favail;
            const uint8_t *fb = mm_region(as, dptr, &foff, &favail);
            if (!fb || ftype != 0x1e)
                break;
            if (decode_frame(fb, foff, favail, (int)w, (int)h, px) != 0)
                break;

            if (!made_dir) {
                mm_mkdir_p(outdir);
                made_dir = 1;
            }
            mm_rgb555_to_rgb888(px, npx, rgb);
            char path[1200];
            snprintf(path, sizeof path, "%s/frame%03u.png", outdir, fi);
            mm_write_png(path, (int)w, (int)h, 3, rgb);
            ok_frames++;
        }
        free(px);
        free(rgb);
    }
    return ok_frames;
}
