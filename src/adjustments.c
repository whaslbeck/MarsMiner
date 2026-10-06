/* adjustments.c — extract the operator adjustments from game.rom.
 *
 * A Pinball 2000 adjustment is three symbols with one name X:
 *   _X        the factory default value; field width = the gap to _d_X
 * (1|2|4|8) _d_X      the descriptor {min, max, step, disp_list} in that width
 * (a 64-bit value carries them as pairs, so its list sits at +0x18 instead of
 * +0x0c) X_enode   the ResourceManager node: +0x00 the label message (four
 * language pointers: English, German, French, Spanish), +0x04 the descriptor,
 * +0x08 the category-mask bit, +0x0c the rank the menu sorts by Nothing here
 * knows a title: the names are whatever the game calls them (adj_*, ad_*, RTC_*
 * ... all alike), and every one with the three symbols is a row.
 *
 * adjustments.csv adjustment,default,min,max,step,width_bits,category,rank,
 *                       label_en,label_de,label_fr,label_es
 * adjustment_enums.csv  adjustment,value,en,de,fr,es — a value's words where
 * the descriptor's disp_list points at a message per value. A list a game
 * builds at boot (the file holds zeros there) is not in the file and so not
 * here.
 *
 * The width comes from the ROM, so all four sizes count. Accepting only 2 and 4
 * silently dropped nine adjustments: adj_current_volume (u8, 0..31) and the
 * eight 64-bit SCORE thresholds (adj_score_level_1..4,
 * adj_replay_pct_{score,magnitude,fixed_boost,auto_boost}), whose maxima are
 * 10-20 billion and do not fit in 32 bits. A gap of 20 is the two
 * Resource<String> adjustments (network_login/password), correctly left out of
 * a numeric table.
 */
#include "marsminer.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name; /* owned: the X of X_enode */
    uint32_t enode;
} ent;

static int cmp_name(const void *a, const void *b) {
    return strcmp(((const ent *)a)->name, ((const ent *)b)->name);
}

/* little-endian unsigned read of 1..8 bytes */
static uint64_t rdu(const mm_buf *rom, uint32_t va, int w) {
    size_t o = va - MM_GBASE;
    if (o + (size_t)w > rom->len)
        return 0;
    uint64_t v = 0;
    for (int i = w - 1; i >= 0; i--)
        v = (v << 8) | rom->data[o + (size_t)i];
    return v;
}

/* a NUL-terminated string at a guest address, or "" when the address is not in
   the image or the text is not plain (the label of an untranslated language is
   often a pointer to ""). */
static const char *cstr(const mm_buf *rom, uint32_t va, uint32_t end, char *buf, size_t n) {
    buf[0] = 0;
    if (va < MM_GBASE || va >= end)
        return buf;
    size_t o = va - MM_GBASE, k = 0;
    while (o + k < rom->len && rom->data[o + k] && k + 1 < n) {
        unsigned char ch = rom->data[o + k];
        if (ch < 9 || (ch > 13 && ch < 32))
            return buf; /* not text */
        buf[k++] = (char)ch;
    }
    buf[k] = 0;
    return buf;
}

/* the four words of a message object into four strings (missing languages stay
 * "") */
static void message4(const mm_buf *rom, uint32_t msg, uint32_t end, char out[4][128]) {
    for (int l = 0; l < 4; l++) {
        out[l][0] = 0;
        if (msg >= MM_GBASE && msg + 16 <= end)
            cstr(rom, (uint32_t)rdu(rom, msg + 4 * (uint32_t)l, 4), end, out[l], 128);
    }
}

long mm_extract_adjustments(const mm_opts *o, mm_ctx *c, const char *out_csv,
                            const char *out_enums_csv) {
    (void)o;
    const mm_symtab *st = &c->symtab;
    const mm_buf *rom = &c->game_rom;
    /* The bundle's game.rom ends here. Not rom->len: --scan-images appends a
       pointer table past it, which must not widen the address range this accepts.
     */
    uint32_t end = (uint32_t)(MM_GBASE + (c->game_len ? c->game_len : rom->len));

    ent *rows = malloc(st->count * sizeof *rows);
    if (!rows)
        return -1;
    size_t nr = 0;
    for (size_t i = 0; i < st->count; i++) {
        const char *nm = st->syms[i].name;
        size_t l = strlen(nm);
        if (l <= 6 || strcmp(nm + l - 6, "_enode") != 0)
            continue;
        char *k = malloc(l - 5);
        if (!k)
            continue;
        memcpy(k, nm, l - 6);
        k[l - 6] = '\0';
        rows[nr].name = k;
        rows[nr].enode = st->syms[i].addr;
        nr++;
    }
    qsort(rows, nr, sizeof *rows, cmp_name);

    FILE *f = fopen(out_csv, "wb");
    FILE *fe = out_enums_csv ? fopen(out_enums_csv, "wb") : NULL;
    if (!f) {
        for (size_t i = 0; i < nr; i++)
            free(rows[i].name);
        free(rows);
        if (fe)
            fclose(fe);
        return -1;
    }
    const char *hdr[] = {"adjustment", "default", "min",      "max",      "step",     "width_bits",
                         "category",   "rank",    "label_en", "label_de", "label_fr", "label_es"};
    mm_csv_row(f, hdr, 12);
    if (fe) {
        const char *ehdr[] = {"adjustment", "value", "en", "de", "fr", "es"};
        mm_csv_row(fe, ehdr, 6);
    }

    long written = 0, enums = 0;
    char vname[300], dname[300];
    for (size_t i = 0; i < nr; i++) {
        const char *x = rows[i].name;
        snprintf(vname, sizeof vname, "_%s", x);
        snprintf(dname, sizeof dname, "_d_%s", x);
        int fv = 0, fd = 0;
        uint32_t a = mm_sym_addr(st, vname, &fv), da = mm_sym_addr(st, dname, &fd);
        if (!fv || !fd)
            continue;
        int w = (int)(da - a);
        if ((w != 1 && w != 2 && w != 4 && w != 8) || !(a >= MM_GBASE && a < end - w))
            continue;
        uint32_t en = rows[i].enode;
        uint32_t cat = 0, rank = 0, msg = 0;
        if (en >= MM_GBASE && en + 16 <= end) {
            msg = (uint32_t)rdu(rom, en, 4);
            cat = (uint32_t)rdu(rom, en + 8, 4);
            rank = (uint32_t)rdu(rom, en + 12, 4);
        }
        char label[4][128];
        message4(rom, msg, end, label);

        char sdef[24], smin[24], smax[24], sstep[24], swb[16], scat[16], srank[16];
        uint64_t lo = rdu(rom, da, w), hi = rdu(rom, da + w, w);
        snprintf(sdef, sizeof sdef, "%" PRIu64, rdu(rom, a, w));
        snprintf(smin, sizeof smin, "%" PRIu64, lo);
        snprintf(smax, sizeof smax, "%" PRIu64, hi);
        snprintf(sstep, sizeof sstep, "%" PRIu64, rdu(rom, da + 2 * w, w));
        snprintf(swb, sizeof swb, "%d", w * 8);
        snprintf(scat, sizeof scat, "%u", cat);
        snprintf(srank, sizeof srank, "%u", rank);
        const char *row[] = {x,    sdef,  smin,     smax,     sstep,    swb,
                             scat, srank, label[0], label[1], label[2], label[3]};
        mm_csv_row(f, row, 12);
        written++;

        /* the value words: one message per value from min to max, where the
           descriptor lists them in the file. The list word sits at +0x0c whatever
           the field width (the three fields are padded to a word each); a 64-bit
           row's would be at +0x18, and none of those enumerate (their values are
           scores). */
        if (fe && w <= 4 && hi >= lo && hi - lo <= 64) {
            uint32_t lst = (uint32_t)rdu(rom, da + 0x0c, 4);
            if (lst >= MM_GBASE && lst + 16 <= end) {
                for (uint64_t v = lo; v <= hi; v++) {
                    uint32_t m = lst + (uint32_t)(v - lo) * 16;
                    if (m + 16 > end)
                        break;
                    uint32_t p0 = (uint32_t)rdu(rom, m, 4);
                    if (p0 < MM_GBASE || p0 >= end)
                        break;
                    char words[4][128];
                    message4(rom, m, end, words);
                    if (!words[0][0])
                        break;
                    char sv[24];
                    snprintf(sv, sizeof sv, "%" PRIu64, v);
                    const char *erow[] = {x, sv, words[0], words[1], words[2], words[3]};
                    mm_csv_row(fe, erow, 6);
                    enums++;
                }
            }
        }
    }
    fclose(f);
    if (fe) {
        fclose(fe);
        mm_log(1, "  tables: adjustment_enums.csv (%ld value words)", enums);
    }
    for (size_t i = 0; i < nr; i++)
        free(rows[i].name);
    free(rows);
    return written;
}
