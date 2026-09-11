/* assets_common.c — shared helpers for the asset decoders:
 *   - address reads that prefer the game ROM then the bank/flash image
 *   - RGB555 -> RGB888/RGBA expansion (matching the Python _LUT)
 *   - Python-csv-compatible field escaping
 */
#include "marsminer.h"

#include <string.h>

/* ---- address reads (1:1 with fonts/anims rd_u32 / rd_bytes) -------------- */
uint32_t mm_rd_u32(const mm_buf *grom, const mm_aspace *as, uint32_t addr, uint32_t def) {
    if (addr >= MM_GBASE && addr < MM_GBASE + grom->len) {
        size_t o = addr - MM_GBASE;
        if (o + 4 <= grom->len) {
            const uint8_t *p = grom->data + o;
            return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                   (uint32_t)p[3] << 24;
        }
        return def;
    }
    return mm_as_u32(as, addr, def);
}

const uint8_t *mm_rd_bytes(const mm_buf *grom, const mm_aspace *as, uint32_t addr, size_t n) {
    if (addr >= MM_GBASE && addr < MM_GBASE + grom->len) {
        size_t o = addr - MM_GBASE;
        if (o + n <= grom->len)
            return grom->data + o;
        return NULL;
    }
    size_t off, avail;
    const uint8_t *b = mm_region(as, addr, &off, &avail);
    if (!b || avail < n)
        return NULL;
    return b + off;
}

/* ---- RGB555 expansion ---------------------------------------------------- */
/* r5 -> r8 = (r<<3)|(r>>2), same as the Python _LUT. */
static inline void expand555(uint16_t p, uint8_t *r, uint8_t *g, uint8_t *b) {
    unsigned rr = (p >> 10) & 0x1F, gg = (p >> 5) & 0x1F, bb = p & 0x1F;
    *r = (uint8_t)((rr << 3) | (rr >> 2));
    *g = (uint8_t)((gg << 3) | (gg >> 2));
    *b = (uint8_t)((bb << 3) | (bb >> 2));
}

void mm_rgb555_to_rgb888(const uint16_t *px, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n; i++) {
        uint8_t r, g, b;
        expand555(px[i] & 0x7FFF, &r, &g, &b);
        out[i * 3] = r;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = b;
    }
}

void mm_rgb555_to_rgba(const uint16_t *px, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n; i++) {
        if (px[i] == MM_TRANSPARENT) {
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = out[i * 4 + 3] = 0;
            continue;
        }
        uint8_t r, g, b;
        expand555(px[i] & 0x7FFF, &r, &g, &b);
        out[i * 4] = r;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = b;
        out[i * 4 + 3] = 255;
    }
}

/* ---- Python-csv-compatible row writer (QUOTE_MINIMAL) -------------------- */
static int csv_need_quote(const char *s) {
    for (const char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
            return 1;
    return 0;
}

static void csv_field(FILE *f, const char *s) {
    if (!s)
        s = "";
    if (csv_need_quote(s)) {
        fputc('"', f);
        for (const char *p = s; *p; p++) {
            if (*p == '"')
                fputc('"', f);
            fputc(*p, f);
        }
        fputc('"', f);
    } else {
        fputs(s, f);
    }
}

void mm_csv_row(FILE *f, const char *const *fields, int n) {
    for (int i = 0; i < n; i++) {
        if (i)
            fputc(',', f);
        csv_field(f, fields[i]);
    }
    fputs("\r\n", f); /* Python csv default lineterminator */
}
