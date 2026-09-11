/* fonts.c — extract the RFM bitmap DMD fonts.  1:1 with extract_fonts.py
 *
 *  font_<name>_data_ptr -> FontData: count@0x00, spacing@0x10, Character[]@0x14
 *  Character (0x1c): asc@0x00, xoff@0x04(i32), yoff@0x08(i32), w@0x0c(u32),
 *                    h@0x10(u32), adv@0x14(i32), bitmap ptr@0x18 (w*h bytes,
 *                    1 byte/pixel colour index; 0 = transparent).
 *  Output: RGBA atlas PNG (index 1 -> white, index>=2 -> grey 128) + metrics CSV.
 */
#include "marsminer.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t asc;
    int32_t xoff, yoff, adv;
    uint32_t w, h;
    const uint8_t *px; /* w*h bytes (borrowed from grom/bank image) */
} glyph;

static int32_t rd_i32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
}
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Returns glyph count written (>0), or 0 if the font is unresolvable/empty. */
static int extract_one(const char *name, uint32_t ptr, mm_ctx *c, const char *outdir) {
    const mm_buf *grom = &c->game_rom;
    const mm_aspace *as = &c->aspace;

    /* fontdata resolvable? */
    if (!mm_rd_bytes(grom, as, ptr, 4))
        return 0;
    uint32_t fontdata = mm_rd_u32(grom, as, ptr, 0);
    if (!mm_rd_bytes(grom, as, fontdata, 4))
        return 0;
    uint32_t count = mm_rd_u32(grom, as, fontdata, 0);
    uint32_t default_adv = mm_rd_u32(grom, as, fontdata + 0x10, 0);
    uint32_t char_arr = mm_rd_u32(grom, as, fontdata + 0x14, 0);
    if (!(count > 0 && count < 1024))
        return 0;
    if (!mm_rd_bytes(grom, as, char_arr, 0x1c))
        return 0;

    glyph *g = (glyph *)calloc(count, sizeof *g);
    if (!g)
        return 0;
    uint32_t ng = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *hdr = mm_rd_bytes(grom, as, char_arr + i * 0x1c, 0x1c);
        if (!hdr) {
            free(g);
            return 0;
        }
        uint8_t asc = hdr[0];
        int32_t xoff = rd_i32(hdr + 0x04);
        int32_t yoff = rd_i32(hdr + 0x08);
        uint32_t w = rd_u32le(hdr + 0x0c);
        uint32_t h = rd_u32le(hdr + 0x10);
        int32_t adv = rd_i32(hdr + 0x14);
        uint32_t bmp = rd_u32le(hdr + 0x18);
        if (adv == 0)
            adv = (int32_t)(default_adv ? default_adv : 1);
        if (!(w > 0 && w <= 64 && h > 0 && h <= 64))
            continue;
        const uint8_t *px = mm_rd_bytes(grom, as, bmp, (size_t)w * h);
        if (!px)
            continue;
        g[ng].asc = asc;
        g[ng].xoff = xoff;
        g[ng].yoff = yoff;
        g[ng].adv = adv;
        g[ng].w = w;
        g[ng].h = h;
        g[ng].px = px;
        ng++;
    }
    if (ng == 0) {
        free(g);
        return 0;
    }

    /* atlas dimensions */
    const int pad = 1;
    uint32_t atlas_h = 0, atlas_w = 0;
    for (uint32_t i = 0; i < ng; i++) {
        if (g[i].h > atlas_h)
            atlas_h = g[i].h;
        atlas_w += g[i].w + pad;
    }
    uint8_t *rgba = (uint8_t *)calloc((size_t)atlas_w * atlas_h, 4);
    if (!rgba) {
        free(g);
        return 0;
    }

    mm_mkdir_p(outdir);
    char csvp[1200], pngp[1200];
    snprintf(pngp, sizeof pngp, "%s/font_%s.png", outdir, name);
    snprintf(csvp, sizeof csvp, "%s/font_%s.csv", outdir, name);
    FILE *csv = fopen(csvp, "wb");
    if (csv) {
        const char *hdr[] = {"ascii", "char", "x", "y", "w", "h", "xoff", "yoff", "adv"};
        mm_csv_row(csv, hdr, 9);
    }

    uint32_t x = 0;
    for (uint32_t i = 0; i < ng; i++) {
        for (uint32_t gy = 0; gy < g[i].h; gy++) {
            for (uint32_t gx = 0; gx < g[i].w; gx++) {
                uint8_t ci = g[i].px[gy * g[i].w + gx];
                if (ci == 0)
                    continue;
                size_t o = ((size_t)gy * atlas_w + (x + gx)) * 4;
                uint8_t v = (ci == 1) ? 255 : 128;
                rgba[o] = v;
                rgba[o + 1] = v;
                rgba[o + 2] = v;
                rgba[o + 3] = 255;
            }
        }
        if (csv) {
            char sasc[16], sx[16], sy[16], sw[16], sh[16], sxo[16], syo[16], sadv[16], sch[8];
            snprintf(sasc, sizeof sasc, "%u", g[i].asc);
            snprintf(sx, sizeof sx, "%u", x);
            snprintf(sy, sizeof sy, "%d", 0);
            snprintf(sw, sizeof sw, "%u", g[i].w);
            snprintf(sh, sizeof sh, "%u", g[i].h);
            snprintf(sxo, sizeof sxo, "%d", g[i].xoff);
            snprintf(syo, sizeof syo, "%d", g[i].yoff);
            snprintf(sadv, sizeof sadv, "%d", g[i].adv);
            if (g[i].asc >= 32 && g[i].asc < 127) {
                sch[0] = (char)g[i].asc;
                sch[1] = '\0';
            } else
                sch[0] = '\0';
            const char *row[] = {sasc, sch, sx, sy, sw, sh, sxo, syo, sadv};
            mm_csv_row(csv, row, 9);
        }
        x += g[i].w + pad;
    }
    if (csv)
        fclose(csv);
    mm_write_png(pngp, (int)atlas_w, (int)atlas_h, 4, rgba);
    free(rgba);
    int result = (int)ng;
    free(g);
    return result;
}

long mm_extract_fonts(const mm_opts *o, mm_ctx *c, const char *out_fonts) {
    const mm_symtab *st = &c->symtab;
    long ok = 0;
    (void)o;
    for (size_t si = 0; si < st->count; si++) {
        const char *n = st->syms[si].name;
        size_t nl = strlen(n);
        const char *suf = "_data_ptr";
        size_t sl = strlen(suf);
        if (strncmp(n, "font_", 5) != 0)
            continue;
        if (nl <= 5 + sl || strcmp(n + nl - sl, suf) != 0)
            continue;
        /* base name = between "font_" and "_data_ptr" */
        char base[256];
        size_t blen = nl - 5 - sl;
        if (blen >= sizeof base)
            continue;
        memcpy(base, n + 5, blen);
        base[blen] = '\0';
        int g = extract_one(base, st->syms[si].addr, c, out_fonts);
        if (g > 0) {
            ok++;
            mm_log(1, "  font %-16s %3d glyphs", base, g);
        }
    }
    return ok;
}
