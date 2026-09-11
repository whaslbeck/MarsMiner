/* tables.c — collect the id/name/index tables the engine needs, into tables/.
 * 1:1 with extract.py:step_tables:
 *   - symbols.csv         (from the parsed symbol table)
 *   - adjustments.csv     (extract_adjustments, ROM-derived)
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

    /* adjustments.csv (ROM-derived) */
    snprintf(dst, sizeof dst, "%s/adjustments.csv", dir);
    long adj = mm_extract_adjustments(o, c, dst);
    if (adj >= 0) {
        n++;
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
