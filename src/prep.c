/* prep.c — Stage 1: raw ROMs -> intermediates.
 *   deinterleave paired 8 MiB chips -> 16 MiB banks   (1:1 deinterleave.py)
 *   assemble the BAR3 update flash                    (1:1 assemble_flash.py)
 *   parse the symbol-table ROM                        (symbols.c)
 *   load game.rom from the update bundle
 * Plus ROM version identification for the run report.
 */
#include "marsminer.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

/* ---- chip files ---------------------------------------------------------
 * Chip dumps arrive named two ways. The ROM sets this tool grew up on use
 * "<prefix>_uNNN.{rom,bin}" (rfm_u109.bin); a chip reader writes the bare
 * "UNNN.ROM" — no prefix at all, upper case. Both resolve here: the directory is
 * read once and every entry matched against
 *
 *     [<prefix>_] u <NNN> .{rom|bin}        letter and extension case-insensitive
 *
 * so neither form has to be renamed or symlinked to be read. The digits must be
 * exactly the three of the chip number, which is what keeps variant dumps
 * (rfm_u100r2.rom) out without a special case for them.
 */
#define CHIP_LO 100
#define CHIP_HI 110
#define CHIP_N (CHIP_HI - CHIP_LO + 1)

typedef struct {
    char path[CHIP_N][1024]; /* resolved file per chip; "" where that chip is absent */
    char prefix[64];         /* the prefix this dump uses; "" when the names are bare */
    int found;               /* how many chips resolved */
} mm_chipset;

/* ASCII case-insensitive compare (the C library's is behind a feature-test macro). */
static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
    }
    return !*a && !*b;
}

/* Does `name` name a chip dump? Returns the chip number and fills `pfx` with the file's own
   prefix ("" for the bare form) and `rank` with an extension preference (.rom before .bin,
   as the old loader had it). -1 for anything that is not a chip file. */
static int chip_of_name(const char *name, char *pfx, size_t pfxsz, int *rank) {
    const char *dot = strrchr(name, '.');
    if (!dot)
        return -1;
    if (ci_eq(dot, ".rom"))
        *rank = 0;
    else if (ci_eq(dot, ".bin"))
        *rank = 1;
    else
        return -1;

    const char *d = dot;
    int ndig = 0;
    while (d > name && d[-1] >= '0' && d[-1] <= '9') {
        d--;
        ndig++;
    }
    if (ndig != 3)
        return -1;
    if (d == name || (d[-1] != 'u' && d[-1] != 'U'))
        return -1;

    const char *u = d - 1;
    size_t plen = (size_t)(u - name);
    if (plen) { /* prefixed form — the separator must be '_', with something before it */
        if (plen < 2 || u[-1] != '_')
            return -1;
        plen--; /* drop the '_' itself */
        if (plen >= pfxsz)
            return -1;
        memcpy(pfx, name, plen);
        pfx[plen] = '\0';
    } else {
        pfx[0] = '\0';
    }
    int n = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
    return (n >= CHIP_LO && n <= CHIP_HI) ? n : -1;
}

struct chipent {
    char prefix[64], path[1024];
    int chip, rank;
};

/* Resolve the chip set in `roms_dir`. `want` pins the prefix (--chip-prefix; NULL or "" =
   work it out from the files). A directory holding more than one set is decided by
   completeness and the alternatives are named in a warning — never a silent readdir-order
   coin flip, which is what choosing "the first match" used to be. */
static void chipset_scan(const char *roms_dir, const char *want, mm_chipset *cs) {
    memset(cs, 0, sizeof *cs);
    DIR *d = roms_dir ? opendir(roms_dir) : NULL;
    if (!d)
        return;

    struct chipent *es = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char pfx[64];
        int rank = 0;
        int chip = chip_of_name(e->d_name, pfx, sizeof pfx, &rank);
        if (chip < 0)
            continue;
        if (want && want[0] && !ci_eq(pfx, want))
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            struct chipent *ne = (struct chipent *)realloc(es, ncap * sizeof *ne);
            if (!ne)
                break;
            es = ne;
            cap = ncap;
        }
        snprintf(es[n].prefix, sizeof es[n].prefix, "%s", pfx);
        snprintf(es[n].path, sizeof es[n].path, "%s/%s", roms_dir, e->d_name);
        es[n].chip = chip;
        es[n].rank = rank;
        n++;
    }
    closedir(d);
    if (!n) {
        free(es);
        return;
    }

    /* Which prefix? The pinned one, else the set covering the most chips (ties go to the
       lexicographically first, so the answer does not depend on directory order). */
    char chosen[64] = "";
    if (want && want[0]) {
        snprintf(chosen, sizeof chosen, "%s", want);
    } else {
        int best = -1, nsets = 0;
        for (size_t i = 0; i < n; i++) {
            int seen = 0;
            for (size_t j = 0; j < i; j++)
                if (ci_eq(es[j].prefix, es[i].prefix)) {
                    seen = 1;
                    break;
                }
            if (seen)
                continue;
            nsets++;
            unsigned mask = 0;
            for (size_t j = 0; j < n; j++)
                if (ci_eq(es[j].prefix, es[i].prefix))
                    mask |= 1u << (es[j].chip - CHIP_LO);
            int cnt = 0;
            for (int b = 0; b < CHIP_N; b++)
                cnt += (mask >> b) & 1u;
            if (cnt > best || (cnt == best && strcmp(es[i].prefix, chosen) < 0)) {
                best = cnt;
                snprintf(chosen, sizeof chosen, "%s", es[i].prefix);
            }
        }
        if (nsets > 1)
            mm_warn("%s holds %d chip sets — using \"%s\"; pass --chip-prefix to pick another",
                    roms_dir, nsets, chosen[0] ? chosen : "(no prefix)");
    }

    int rank[CHIP_N];
    for (int i = 0; i < CHIP_N; i++)
        rank[i] = 99;
    for (size_t i = 0; i < n; i++) {
        if (!ci_eq(es[i].prefix, chosen))
            continue;
        int k = es[i].chip - CHIP_LO;
        if (es[i].rank >= rank[k])
            continue;
        if (!cs->path[k][0])
            cs->found++;
        rank[k] = es[i].rank;
        snprintf(cs->path[k], sizeof cs->path[k], "%s", es[i].path);
    }
    snprintf(cs->prefix, sizeof cs->prefix, "%s", chosen);
    free(es);
}

/* Find the first file in `dir` whose name ends with `suffix`. */
static int find_suffix(const char *dir, const char *suffix, char *out, size_t outsz) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    size_t slen = strlen(suffix);
    int got = 0;
    /* glob(*suffix)[0] in Python is filesystem order; we mirror "first match".
       readdir order is unspecified, but these bundles have a single match per
       suffix, so the choice is unambiguous. */
    while ((e = readdir(d))) {
        size_t nlen = strlen(e->d_name);
        if (nlen >= slen && strcmp(e->d_name + nlen - slen, suffix) == 0) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            got = 1;
            break;
        }
    }
    closedir(d);
    return got;
}

/* interleave16: A0 A1 B0 B1 A2 A3 ... into a bank of `bank_size` bytes. */
static uint8_t *interleave16(const mm_buf *a, const mm_buf *b, size_t bank_size) {
    uint8_t *out = (uint8_t *)malloc(bank_size);
    if (!out)
        return NULL;
    memset(out, 0, bank_size);
    size_t n = a->len < b->len ? a->len : b->len;
    size_t max_pairs = (n / 2 < bank_size / 4) ? n / 2 : bank_size / 4;
    for (size_t k = 0; k < max_pairs; k++) {
        size_t si = k * 2, di = k * 4;
        out[di] = a->data[si];
        out[di + 1] = a->data[si + 1];
        out[di + 2] = b->data[si];
        out[di + 3] = b->data[si + 1];
    }
    return out;
}

/* ---- deinterleave (image banks only; DCS uses raw u109/u110 directly) ---- */

struct bankdef {
    const char *name;
    int a, b;
    size_t size;
};

int mm_deinterleave(const char *roms_dir, const char *out_dir, const char *prefix, int verbose) {
    static const struct bankdef BANKS[] = {
        {"bank0", 100, 101, 16u * 1024 * 1024},
        {"bank1", 102, 103, 16u * 1024 * 1024},
        {"bank2", 104, 105, 16u * 1024 * 1024},
        {"bank3", 106, 107, 16u * 1024 * 1024},
    };
    if (mm_mkdir_p(out_dir) != 0) {
        mm_warn("cannot create %s", out_dir);
        return -1;
    }
    mm_chipset cs;
    chipset_scan(roms_dir, prefix, &cs);
    if (!cs.found) {
        mm_warn("no chip dumps in %s — expected <prefix>_uNNN.{rom,bin} or uNNN.{rom,bin}",
                roms_dir);
        return 0;
    }
    if (verbose)
        mm_log(1, "chip set: %d file(s), prefix \"%s\"", cs.found,
               cs.prefix[0] ? cs.prefix : "(none)");

    int built = 0;
    for (size_t i = 0; i < sizeof BANKS / sizeof BANKS[0]; i++) {
        const char *pa = cs.path[BANKS[i].a - CHIP_LO], *pb = cs.path[BANKS[i].b - CHIP_LO];
        if (!pa[0] || !pb[0]) {
            mm_warn("[%s] chips u%d/u%d not found — skipping", BANKS[i].name, BANKS[i].a,
                    BANKS[i].b);
            continue;
        }
        mm_buf a = mm_load(pa), b = mm_load(pb);
        if (!a.data || !b.data) {
            mm_buf_free(&a);
            mm_buf_free(&b);
            continue;
        }
        uint8_t *bank = interleave16(&a, &b, BANKS[i].size);
        mm_buf_free(&a);
        mm_buf_free(&b);
        if (!bank)
            return -1;
        char outp[1024];
        snprintf(outp, sizeof outp, "%s/rfm_%s.bin", out_dir, BANKS[i].name);
        int rc = mm_write_file(outp, bank, BANKS[i].size);
        free(bank);
        if (rc != 0) {
            mm_warn("cannot write %s", outp);
            return -1;
        }
        built++;
        if (verbose)
            mm_log(1, "[%s] u%d+u%d -> %s (%zu MiB)", BANKS[i].name, BANKS[i].a, BANKS[i].b, outp,
                   BANKS[i].size / 1024 / 1024);
    }
    return built;
}

/* ---- assemble BAR3 flash ------------------------------------------------- */

int mm_assemble_flash(const char *bundle_dir, mm_buf *out) {
    const size_t BAR3 = 4u * 1024 * 1024;
    uint8_t *flash = (uint8_t *)malloc(BAR3);
    if (!flash)
        return -1;
    memset(flash, 0xFF, BAR3);

    char pboot[1024], pflsh[1024], pgame[1024], psym[1024];
    if (!find_suffix(bundle_dir, "_bootdata.rom", pboot, sizeof pboot) ||
        !find_suffix(bundle_dir, "_im_flsh0.rom", pflsh, sizeof pflsh) ||
        !find_suffix(bundle_dir, "_game.rom", pgame, sizeof pgame) ||
        !find_suffix(bundle_dir, "_symbols.rom", psym, sizeof psym)) {
        mm_warn("assemble_flash: missing one of *_bootdata/_im_flsh0/_game/_symbols.rom in %s",
                bundle_dir);
        free(flash);
        return -1;
    }
    /* bootdata truncated to 32 KiB at offset 0 */
    mm_buf boot = mm_load(pboot);
    size_t bn = boot.len < 0x8000 ? boot.len : 0x8000;
    if (boot.data)
        memcpy(flash, boot.data, bn);
    mm_buf_free(&boot);

    size_t off = 0x8000;
    const char *seq[3] = {pflsh, pgame, psym};
    for (int i = 0; i < 3; i++) {
        mm_buf b = mm_load(seq[i]);
        if (b.data) {
            size_t n = (off + b.len <= BAR3) ? b.len : (BAR3 - off);
            memcpy(flash + off, b.data, n);
            off += b.len;
        }
        mm_buf_free(&b);
    }
    out->data = flash;
    out->len = BAR3;
    return 0;
}

/* ---- version identification --------------------------------------------- */

/* Derive a "1.80"-style version string from the bundle filename if possible. */
static void derive_version(const mm_opts *o, char out[32]) {
    out[0] = '\0';
    if (!o->bundle_dir)
        return;
    char p[1024];
    if (find_suffix(o->bundle_dir, "_game.rom", p, sizeof p)) {
        /* pin2000_50070_0180_game.rom -> "0180" -> "1.80" */
        const char *s = strstr(p, "50070_");
        if (s) {
            s += 6;
            if (s[0] && s[1] && s[2] && s[3]) {
                char maj = s[1], min1 = s[2], min2 = s[3];
                snprintf(out, 32, "%c.%c%c", maj, min1, min2);
                return;
            }
        }
    }
}

/* Which raw DCS ROMs will the sounds stage use? (explicit override, else the
   real machine config: chips u109/u110 + the bundle's sf.rom). */
void mm_resolve_dcs_roms(const mm_opts *o, char u109[1024], char u110[1024], char flash[1024]) {
    mm_chipset cs;
    chipset_scan(o->roms_dir, o->chip_prefix, &cs);
    u109[0] = u110[0] = flash[0] = '\0';
    if (o->dcs_u109)
        snprintf(u109, 1024, "%s", o->dcs_u109);
    else
        snprintf(u109, 1024, "%s", cs.path[109 - CHIP_LO]);
    if (o->dcs_u110)
        snprintf(u110, 1024, "%s", o->dcs_u110);
    else
        snprintf(u110, 1024, "%s", cs.path[110 - CHIP_LO]);
    if (o->dcs_flash)
        snprintf(flash, 1024, "%s", o->dcs_flash);
    else if (o->bundle_dir) {
        char p[1024];
        if (find_suffix(o->bundle_dir, "_sf.rom", p, sizeof p))
            snprintf(flash, 1024, "%s", p);
    }
}

static void md5_of_file(const char *path, char out[33]) {
    out[0] = '\0';
    mm_buf b = mm_load(path);
    if (b.data) {
        mm_md5_hex(b.data, b.len, out);
        mm_buf_free(&b);
    }
}

void mm_identify(const mm_opts *o, mm_ctx *c, mm_idinfo *id) {
    memset(id, 0, sizeof *id);
    id->dcs_consistent = -1;
    derive_version(o, id->game_version);
    char u109[1024], u110[1024], flash[1024];
    mm_resolve_dcs_roms(o, u109, u110, flash);
    if (u109[0])
        md5_of_file(u109, id->u109_md5);
    if (u110[0])
        md5_of_file(u110, id->u110_md5);
    if (flash[0])
        md5_of_file(flash, id->flash_md5);
    (void)c;
}

/* ---- stage_prepare ------------------------------------------------------- */

int mm_stage_prepare(const mm_opts *o, mm_ctx *c) {
    snprintf(c->banks_dir, sizeof c->banks_dir, "%s/banks", o->work_dir ? o->work_dir : "work");

    /* game.rom + symbols.rom come from the update bundle. */
    if (o->bundle_dir) {
        char pgame[1024], psym[1024];
        if (find_suffix(o->bundle_dir, "_game.rom", pgame, sizeof pgame))
            c->game_rom = mm_load_required(pgame, "game.rom");
        if (find_suffix(o->bundle_dir, "_symbols.rom", psym, sizeof psym)) {
            mm_buf srom = mm_load_required(psym, "symbols.rom");
            if (mm_symbols_parse(&c->symtab, &srom) != 0)
                mm_warn("failed to parse symbol table %s", psym);
            mm_buf_free(&srom);
        }
    }
    c->game_len = c->game_rom.len; /* the bundle's own extent; --scan-images appends past it */
    /* Without a symbol table there is nothing to walk — unless --scan-images, which finds the
       image structures by shape instead and supplies both below, once the banks are loaded. */
    if (!o->scan_images) {
        if (!c->game_rom.data)
            mm_die("no game.rom (need --bundle with *_game.rom, or --scan-images)");
        if (!c->symtab.count)
            mm_die("no symbols (need --bundle with *_symbols.rom, or --scan-images)");
    }

    /* Build image banks unless they already exist and not --force.
       Sized above banks_dir[] so the compiler can see the join always fits. */
    char bank0[sizeof c->banks_dir + 32];
    snprintf(bank0, sizeof bank0, "%s/rfm_bank0.bin", c->banks_dir);
    if (o->force || !mm_path_exists(bank0)) {
        if (!o->roms_dir)
            mm_warn("no --roms: image banks not built (images/fonts will be empty)");
        else {
            mm_log(1, "deinterleaving image banks from %s", o->roms_dir);
            mm_deinterleave(o->roms_dir, c->banks_dir, o->chip_prefix, o->verbose);
        }
    } else {
        mm_log(1, "image banks present in %s — skipping deinterleave", c->banks_dir);
    }

    /* Assemble the BAR3 update flash (used by a few anims/fonts). */
    if (o->bundle_dir) {
        if (mm_assemble_flash(o->bundle_dir, &c->flash) != 0)
            mm_warn("flash not assembled (some anims may not resolve)");
    }

    /* Build the shared address space (banks + flash). */
    if (mm_aspace_init(&c->aspace, c->banks_dir, c->flash) != 0)
        mm_die("out of memory building address space");

    /* Structural image scan — needs the address space, so it runs last. */
    if (o->scan_images) {
        long n = mm_scan_images(o, c);
        if (n <= 0 && !c->symtab.count)
            mm_die("--scan-images found no image structures in %s — is --roms pointing at the "
                   "mask-ROM chips, and do their names match --chip-prefix?",
                   o->roms_dir ? o->roms_dir : "(no --roms given)");
    }
    return 0;
}
