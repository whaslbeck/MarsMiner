#define _POSIX_C_SOURCE 200809L /* strdup under -std=c11 */
/* tables.c — collect the id/name/index tables the engine needs, into tables/.
 * 1:1 with extract.py:step_tables:
 *   - symbols.csv         (from the parsed symbol table)
 *   - adjustments.csv, adjustment_enums.csv (extract_adjustments, ROM-derived)
 *   - copy the canonical pre-derived tables that live in work/ if present
 *     (anim_index.csv, dcs_sound_commands.csv->sound_commands.csv,
 *      switch_numbers.json) — exactly as extract.py copies them from WORK.
 */
#include "marsminer.h"

#include <stdlib.h>
#include <string.h>

static int copy_file(const char *src, const char *dst) {
    mm_buf b = mm_load(src);
    if (!b.data)
        return -1;
    int rc = mm_write_file(dst, b.data, b.len);
    mm_buf_free(&b);
    return rc;
}

/* --dump SYM:LEN,... — raw bytes at named symbols, verbatim, to tables/rom/SYM.bin. Which
   symbols is the caller's business (a title's asset script), so nothing here knows a title. */
static long dump_symbols(const mm_opts *o, mm_ctx *c, const char *tables_dir) {
    if (!o->dumps || !*o->dumps)
        return 0;
    char dir[1100], dst[1400];
    snprintf(dir, sizeof dir, "%s/rom", tables_dir);
    mm_mkdir_p(dir);
    long n = 0;
    char *list = strdup(o->dumps);
    for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        char *colon = strrchr(t, ':');
        long len = colon ? strtol(colon + 1, NULL, 0) : 0;
        if (!colon || len <= 0) {
            mm_warn("--dump '%s': want SYM:LEN", t);
            continue;
        }
        *colon = 0;
        int found = 0;
        uint32_t addr = mm_sym_addr(&c->symtab, t, &found);
        const uint8_t *p = found ? mm_rd_bytes(&c->game_rom, &c->aspace, addr, (size_t)len) : NULL;
        if (!p) {
            mm_warn("--dump %s: %s", t, found ? "not readable" : "no such symbol");
            continue;
        }
        snprintf(dst, sizeof dst, "%s/%s.bin", dir, t);
        if (mm_write_file(dst, p, (size_t)len) == 0) {
            n++;
            mm_log(1, "  tables: rom/%s.bin (%ld bytes @0x%08x)", t, len, addr);
        }
    }
    free(list);
    return n;
}

long mm_stage_tables(const mm_opts *o, mm_ctx *c) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/tables", o->out_dir);
    mm_mkdir_p(dir);
    long n = 0;
    char dst[1200], src[1200];

    /* symbols.csv (slimmed name->address CSV, same content parse_symbols emits) */
    snprintf(dst, sizeof dst, "%s/symbols.csv", dir);
    if (mm_symbols_write_csv(&c->symtab, dst) == 0) {
        n++;
        mm_log(1, "  tables: symbols.csv");
    }

    /* adjustments.csv + adjustment_enums.csv (ROM-derived) */
    snprintf(dst, sizeof dst, "%s/adjustments.csv", dir);
    char enums[1200];
    snprintf(enums, sizeof enums, "%s/adjustment_enums.csv", dir);
    long adj = mm_extract_adjustments(o, c, dst, enums);
    if (adj >= 0) {
        n += 2;
        mm_log(1, "  tables: adjustments.csv (%ld rows)", adj);
    }

    /* canonical pre-derived tables copied from work/ (as extract.py does) */
    const char *work = o->work_dir ? o->work_dir : "work";
    struct {
        const char *from, *to;
    } copies[] = {
        {"anim_index.csv", "anim_index.csv"},
        {"dcs_sound_commands.csv", "sound_commands.csv"},
        {"switch_numbers.json", "switch_numbers.json"},
    };
    for (size_t i = 0; i < sizeof copies / sizeof copies[0]; i++) {
        snprintf(src, sizeof src, "%s/%s", work, copies[i].from);
        if (!mm_path_exists(src))
            continue;
        snprintf(dst, sizeof dst, "%s/%s", dir, copies[i].to);
        if (copy_file(src, dst) == 0) {
            n++;
            mm_log(1, "  tables: %s (copied)", copies[i].to);
        }
    }
    n += dump_symbols(o, c, dir);
    return n;
}

/* messages stage writes tables/messages.csv */
long mm_stage_messages(const mm_opts *o, mm_ctx *c) {
    char dir[1024], out[1200];
    snprintf(dir, sizeof dir, "%s/tables", o->out_dir);
    mm_mkdir_p(dir);
    snprintf(out, sizeof out, "%s/messages.csv", dir);
    long n = mm_extract_messages(o, c, out);
    return n;
}

long mm_stage_images(const mm_opts *o, mm_ctx *c) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/images", o->out_dir);
    mm_mkdir_p(dir);
    long a = mm_render_anims(o, c, dir);
    long m = mm_render_movies(o, c, dir);
    mm_log(1, "images: %ld anim frames, %ld movie frames", a, m);
    return a + m;
}

long mm_stage_fonts(const mm_opts *o, mm_ctx *c) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/fonts", o->out_dir);
    mm_mkdir_p(dir);
    return mm_extract_fonts(o, c, dir);
}
