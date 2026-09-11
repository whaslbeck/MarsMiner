/* adjustments.c — extract the RFM adjustment table from game.rom.
 * 1:1 with extract_adjustments.py.
 *   _adj_<name>    factory default value
 *   _d_adj_<name>  {min,max,step} definition struct; field width = addr gap (1|2|4|8)
 *   adj_<name>_enode  ResourceManager node; +0x08 = category-mask bit
 *
 * The width comes from the ROM, so all four sizes count. Accepting only 2 and 4 silently
 * dropped nine adjustments: adj_current_volume (u8, 0..31) and the eight 64-bit SCORE
 * thresholds (adj_score_level_1..4, adj_replay_pct_{score,magnitude,fixed_boost,auto_boost}),
 * whose maxima are 10-20 billion and do not fit in 32 bits. A gap of 20 is the two
 * Resource<String> adjustments (network_login/password), correctly left out of a numeric table.
 */
#include "marsminer.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    uint32_t addr;
} ent;

static int find_ent(const ent *v, size_t n, const char *name, uint32_t *addr) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i].name, name) == 0) {
            *addr = v[i].addr;
            return 1;
        }
    return 0;
}

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

long mm_extract_adjustments(const mm_opts *o, mm_ctx *c, const char *out_csv) {
    (void)o;
    const mm_symtab *st = &c->symtab;
    const mm_buf *rom = &c->game_rom;
    uint32_t end = (uint32_t)(MM_GBASE + rom->len);

    ent *adj = malloc(st->count * sizeof *adj);
    ent *dadj = malloc(st->count * sizeof *dadj);
    ent *enode = malloc(st->count * sizeof *enode);
    if (!adj || !dadj || !enode) {
        free(adj);
        free(dadj);
        free(enode);
        return -1;
    }
    size_t na = 0, nd = 0, ne = 0;

    for (size_t i = 0; i < st->count; i++) {
        const char *nm = st->syms[i].name;
        uint32_t a = st->syms[i].addr;
        size_t l = strlen(nm);
        if (strncmp(nm, "_d_adj_", 7) == 0) {
            dadj[nd].name = (char *)(nm + 7);
            dadj[nd].addr = a;
            nd++;
        } else if (strncmp(nm, "_adj_", 5) == 0) {
            adj[na].name = (char *)(nm + 5);
            adj[na].addr = a;
            na++;
        } else if (strncmp(nm, "adj_", 4) == 0 && l > 6 && strcmp(nm + l - 6, "_enode") == 0) {
            /* key = nm[4:-6] — store a heap copy since we trim the suffix */
            size_t kl = l - 4 - 6;
            char *k = malloc(kl + 1);
            if (!k)
                continue;
            memcpy(k, nm + 4, kl);
            k[kl] = '\0';
            enode[ne].name = k;
            enode[ne].addr = a;
            ne++;
        }
    }

    /* sort adj by name (Python sorted(adj.items())) */
    qsort(adj, na, sizeof *adj, cmp_name);

    FILE *f = fopen(out_csv, "wb");
    if (!f) {
        for (size_t i = 0; i < ne; i++)
            free(enode[i].name);
        free(adj);
        free(dadj);
        free(enode);
        return -1;
    }
    const char *hdr[] = {"adjustment", "default", "min", "max", "step", "width_bits", "category"};
    mm_csv_row(f, hdr, 7);

    long rows = 0;
    for (size_t i = 0; i < na; i++) {
        uint32_t a = adj[i].addr, da;
        if (!find_ent(dadj, nd, adj[i].name, &da))
            continue;
        int w = (int)(da - a);
        if ((w != 1 && w != 2 && w != 4 && w != 8) || !(a >= MM_GBASE && a < end - w))
            continue;
        uint32_t cat = 0, en;
        if (find_ent(enode, ne, adj[i].name, &en) && en + 8 >= MM_GBASE && en + 8 < end - 4)
            cat = (uint32_t)rdu(rom, en + 8, 4);
        char full[300];
        snprintf(full, sizeof full, "adj_%s", adj[i].name);
        char sdef[24], smin[24], smax[24], sstep[24], swb[16], scat[16];
        snprintf(sdef, sizeof sdef, "%" PRIu64, rdu(rom, a, w));
        snprintf(smin, sizeof smin, "%" PRIu64, rdu(rom, da, w));
        snprintf(smax, sizeof smax, "%" PRIu64, rdu(rom, da + w, w));
        snprintf(sstep, sizeof sstep, "%" PRIu64, rdu(rom, da + 2 * w, w));
        snprintf(swb, sizeof swb, "%d", w * 8);
        snprintf(scat, sizeof scat, "%u", cat);
        const char *row[] = {full, sdef, smin, smax, sstep, swb, scat};
        mm_csv_row(f, row, 7);
        rows++;
    }
    fclose(f);
    for (size_t i = 0; i < ne; i++)
        free(enode[i].name);
    free(adj);
    free(dadj);
    free(enode);
    return rows;
}
