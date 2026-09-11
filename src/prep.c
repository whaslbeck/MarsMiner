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

/* Find "<roms>/<game>_u<chip>.{rom,bin}" (loader tries .rom then .bin). */
static int find_chip(const char *roms, const char *game, const char *chip, char *out,
                     size_t outsz) {
    const char *sufs[] = {".rom", ".bin"};
    for (int i = 0; i < 2; i++) {
        snprintf(out, outsz, "%s/%s_u%s%s", roms, game, chip, sufs[i]);
        if (mm_path_exists(out))
            return 1;
    }
    return 0;
}

/* The chip dumps are named <prefix>_uNNN.{rom,bin}, and the prefix is the game's short name
   ("rfm_u109.bin"). Rather than hard-code one title, look at what is actually in the directory:
   take the prefix from the first *_u1NN.{rom,bin} we find. --chip-prefix overrides. */
static const char *chip_prefix(const mm_opts *o, char *buf, size_t bufsz) {
    if (o->chip_prefix && o->chip_prefix[0])
        return o->chip_prefix;
    DIR *d = o->roms_dir ? opendir(o->roms_dir) : NULL;
    if (!d)
        return "rfm";
    struct dirent *e;
    const char *found = NULL;
    while (!found && (e = readdir(d))) {
        const char *u = strstr(e->d_name, "_u1");
        size_t nl = strlen(e->d_name);
        if (!u || u == e->d_name || nl < 8)
            continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || (strcmp(dot, ".rom") != 0 && strcmp(dot, ".bin") != 0))
            continue;
        size_t pl = (size_t)(u - e->d_name);
        if (pl + 1 > bufsz)
            continue;
        memcpy(buf, e->d_name, pl);
        buf[pl] = '\0';
        found = buf;
    }
    closedir(d);
    return found ? found : "rfm";
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
    const char *name, *a, *b;
    size_t size;
};

int mm_deinterleave(const char *roms_dir, const char *out_dir, const char *prefix, int verbose) {
    static const struct bankdef BANKS[] = {
        {"bank0", "100", "101", 16u * 1024 * 1024},
        {"bank1", "102", "103", 16u * 1024 * 1024},
        {"bank2", "104", "105", 16u * 1024 * 1024},
        {"bank3", "106", "107", 16u * 1024 * 1024},
    };
    if (mm_mkdir_p(out_dir) != 0) {
        mm_warn("cannot create %s", out_dir);
        return -1;
    }
    int built = 0;
    for (size_t i = 0; i < sizeof BANKS / sizeof BANKS[0]; i++) {
        char pa[1024], pb[1024];
        if (!find_chip(roms_dir, prefix, BANKS[i].a, pa, sizeof pa) ||
            !find_chip(roms_dir, prefix, BANKS[i].b, pb, sizeof pb)) {
            mm_warn("[%s] chips u%s/u%s not found — skipping", BANKS[i].name, BANKS[i].a,
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
            mm_log(1, "[%s] u%s+u%s -> %s (%zu MiB)", BANKS[i].name, BANKS[i].a, BANKS[i].b, outp,
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
    char pbuf[64];
    u109[0] = u110[0] = flash[0] = '\0';
    if (o->dcs_u109)
        snprintf(u109, 1024, "%s", o->dcs_u109);
    else if (o->roms_dir)
        find_chip(o->roms_dir, chip_prefix(o, pbuf, sizeof pbuf), "109", u109, 1024);
    if (o->dcs_u110)
        snprintf(u110, 1024, "%s", o->dcs_u110);
    else if (o->roms_dir)
        find_chip(o->roms_dir, chip_prefix(o, pbuf, sizeof pbuf), "110", u110, 1024);
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
    if (!c->game_rom.data)
        mm_die("no game.rom (need --bundle with *_game.rom)");
    if (!c->symtab.count)
        mm_die("no symbols (need --bundle with *_symbols.rom)");

    /* Build image banks unless they already exist and not --force.
       Sized above banks_dir[] so the compiler can see the join always fits. */
    char bank0[sizeof c->banks_dir + 32];
    snprintf(bank0, sizeof bank0, "%s/rfm_bank0.bin", c->banks_dir);
    if (o->force || !mm_path_exists(bank0)) {
        if (!o->roms_dir)
            mm_warn("no --roms: image banks not built (images/fonts will be empty)");
        else {
            char pbuf[64];
            const char *pfx = chip_prefix(o, pbuf, sizeof pbuf);
            mm_log(1, "deinterleaving image banks from %s (chip prefix \"%s\")", o->roms_dir, pfx);
            mm_deinterleave(o->roms_dir, c->banks_dir, pfx, o->verbose);
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
    return 0;
}
