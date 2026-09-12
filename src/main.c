#define _POSIX_C_SOURCE 200809L /* strdup under -std=c11 */
/* main.c — ReRfM-MarsMiner CLI: read the machine ROMs, write the asset tree.
 *
 * Usage:
 *   marsminer --roms <chipdir> --bundle <updatedir> [--out assets] [options]
 *
 * The two ROM inputs mirror a real machine:
 *   --roms    directory of the paired mask-ROM chips (rfm_u100..u110.{rom,bin})
 *   --bundle  the flash update bundle (pin2000_*_game/_sf/_symbols/_im_flsh0/_bootdata.rom)
 */
#include "marsminer.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h> /* realpath */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- recursive count + size of an output subtree ------------------------ */
static void walk_dir(const char *path, long *files, uint64_t *bytes) {
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk_dir(p, files, bytes);
        else {
            (*files)++;
            *bytes += (uint64_t)st.st_size;
        }
    }
    closedir(d);
}

static void usage(const char *prog) {
    printf("ReRfM-MarsMiner — Revenge from Mars ROM asset extractor\n"
           "\n"
           "Usage: %s --roms <chipdir> --bundle <updatedir> [options]\n"
           "\n"
           "Inputs (from your own machine's ROMs):\n"
           "  --roms   DIR    paired mask-ROM chips  (rfm_u100..u110.{rom,bin})\n"
           "  --bundle DIR    flash update bundle    (pin2000_*_game/_sf/_symbols/...)\n"
           "                  Without one there is no symbol table: see --scan-images.\n"
           "\n"
           "Output:\n"
           "  --out    DIR    asset output root      (default: assets)\n"
           "  --work   DIR    intermediates (banks/…) (default: work)\n"
           "  (default: pack the assets into <out>.zip and remove the loose tree)\n"
           "  --loose         also keep the loose asset tree next to the .zip\n"
           "  --no-zip        write only the loose tree, no .zip\n"
           "\n"
           "Selection:\n"
           "  --only   LIST   comma list of stages: sounds,images,fonts,messages,tables\n"
           "  --force         rebuild even if outputs exist\n"
           "  --all-sounds    decode every DCS id, not just the game's used-id list\n"
           "  --no-dcs-check  skip the flash/sample consistency probe before decoding\n"
           "  --scan-images   find the anims in the image banks by their structure instead of\n"
           "                  from symbols, and name them by address. Makes --bundle optional,\n"
           "                  so a chips-only dump still yields its graphics. Added to a run\n"
           "                  that has a bundle, it picks up images no symbol points at.\n"
           "\n"
           "DCS overrides (else: chips u109/u110 + the bundle's sf.rom):\n"
           "  --dcs-u109 F  --dcs-u110 F  --dcs-flash F   raw DCS sample/flash ROMs\n"
           "  --chip-prefix P the <prefix> in <prefix>_uNNN.{rom,bin} (default: taken from the\n"
           "                  files in --roms; bare uNNN.{rom,bin} dumps need no prefix, and\n"
           "                  either form may be upper or lower case)\n"
           "  --used-ids F    sound-id allow-list override (default: derive it from the ROM's\n"
           "                  own acl tables at extract time — no static list needed)\n"
           "  --names   F     id->name CSV          (optional)\n"
           "\n"
           "Reporting:\n"
           "  -v, -vv         more verbose\n"
           "  -h, --help      this help\n",
           prog);
}

/* ---- stage bit from name ------------------------------------------------ */
static unsigned step_bit(const char *s) {
    if (!strcmp(s, "sounds"))
        return MM_STEP_SOUNDS;
    if (!strcmp(s, "images"))
        return MM_STEP_IMAGES;
    if (!strcmp(s, "fonts"))
        return MM_STEP_FONTS;
    if (!strcmp(s, "messages"))
        return MM_STEP_MESSAGES;
    if (!strcmp(s, "tables"))
        return MM_STEP_TABLES;
    if (!strcmp(s, "prepare"))
        return MM_STEP_PREPARE;
    if (!strcmp(s, "manifest"))
        return MM_STEP_MANIFEST;
    return 0;
}

/* Is `sub` the same directory as `parent`, or inside it? Both are canonicalised first, so
   symlinks and ../ cannot sneak past. Unresolvable paths answer "no". */
static int path_within(const char *sub, const char *parent) {
    char rs[PATH_MAX], rp[PATH_MAX];
    if (!realpath(sub, rs) || !realpath(parent, rp))
        return 0;
    size_t n = strlen(rp);
    if (strncmp(rs, rp, n) != 0)
        return 0;
    return rs[n] == '\0' || rs[n] == '/';
}

int main(int argc, char **argv) {
    mm_opts o;
    memset(&o, 0, sizeof o);
    o.out_dir = "assets";
    o.work_dir = "work";
    o.steps = MM_STEP_ALL;
    unsigned asked_for = 0; /* stages named in --only (see the skip rule below) */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (mm_die("%s needs an argument", a), (char *)0))
        if (!strcmp(a, "--roms"))
            o.roms_dir = NEXT();
        else if (!strcmp(a, "--bundle"))
            o.bundle_dir = NEXT();
        else if (!strcmp(a, "--out"))
            o.out_dir = NEXT();
        else if (!strcmp(a, "--work"))
            o.work_dir = NEXT();
        else if (!strcmp(a, "--force"))
            o.force = 1;
        else if (!strcmp(a, "--all-sounds"))
            o.all_sounds = 1;
        else if (!strcmp(a, "--no-dcs-check"))
            o.no_dcs_check = 1;
        else if (!strcmp(a, "--scan-images"))
            o.scan_images = 1;
        else if (!strcmp(a, "--loose"))
            o.keep_loose = 1;
        else if (!strcmp(a, "--no-zip"))
            o.no_zip = 1;
        else if (!strcmp(a, "--dcs-u109"))
            o.dcs_u109 = NEXT();
        else if (!strcmp(a, "--dcs-u110"))
            o.dcs_u110 = NEXT();
        else if (!strcmp(a, "--dcs-flash"))
            o.dcs_flash = NEXT();
        else if (!strcmp(a, "--used-ids"))
            o.used_ids = NEXT();
        else if (!strcmp(a, "--names"))
            o.sound_names = NEXT();
        else if (!strcmp(a, "--chip-prefix"))
            o.chip_prefix = NEXT();
        else if (!strcmp(a, "-v"))
            o.verbose++;
        else if (!strcmp(a, "-vv"))
            o.verbose += 2;
        else if (!strcmp(a, "--only")) {
            o.steps = MM_STEP_PREPARE | MM_STEP_MANIFEST; /* prepare/manifest always run */
            char *list = strdup(NEXT());
            for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
                unsigned b = step_bit(t);
                if (!b)
                    mm_warn("unknown stage '%s'", t);
                o.steps |= b;
                asked_for |= b; /* named by the user: missing inputs are an error, not a skip */
            }
            free(list);
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else
            mm_die("unknown option '%s' (try --help)", a);
#undef NEXT
    }
    mm_verbosity = o.verbose;

    if (!o.roms_dir && !o.bundle_dir) {
        usage(argv[0]);
        return 2;
    }
    /* No bundle means no game.rom and no symbol table, so the symbol-driven walk cannot start.
       The structural image scan is then the only way in — turn it on rather than making the
       user ask for the only mode that can work. */
    if (!o.bundle_dir && !o.scan_images) {
        if (!o.roms_dir)
            mm_die("nothing to read: give --roms (chip dumps) and, if you have it, --bundle");
        o.scan_images = 1;
        mm_info("no --bundle: no symbol table, so images are found by structure (--scan-images)");
    }
    if (o.scan_images && !o.roms_dir)
        mm_die("--scan-images needs --roms: the structures it looks for live in the image banks");

    mm_info("ReRfM-MarsMiner");
    mm_info("  roms   : %s", o.roms_dir ? o.roms_dir : "(none)");
    mm_info("  bundle : %s", o.bundle_dir ? o.bundle_dir : "(none — image scan only)");
    mm_info("  out    : %s", o.out_dir);
    if (mm_mkdir_p(o.out_dir) != 0)
        mm_die("cannot create --out %s", o.out_dir);

    /* Writing 15k asset files into the directory holding your ROM dumps is never what anyone
       meant, and --out is deleted by default after packing, so refuse it outright. */
    if ((o.roms_dir && path_within(o.out_dir, o.roms_dir)) ||
        (o.bundle_dir && path_within(o.out_dir, o.bundle_dir)))
        mm_die("--out %s is inside a ROM directory — refusing to write assets there", o.out_dir);
    if ((o.roms_dir && path_within(o.work_dir, o.roms_dir)) ||
        (o.bundle_dir && path_within(o.work_dir, o.bundle_dir)))
        mm_die("--work %s is inside a ROM directory — refusing to write there", o.work_dir);

    mm_ctx c;
    memset(&c, 0, sizeof c);
    mm_stage_prepare(&o, &c);

    /* ---- identify + report ROM versions ---- */
    mm_idinfo id;
    mm_identify(&o, &c, &id);
    mm_info("ROM identity:");
    if (id.game_version[0])
        mm_info("  game version : %s", id.game_version);
    mm_info("  symbols      : %zu", c.symtab.count);
    if (id.flash_md5[0])
        mm_info("  DCS flash    : md5 %s", id.flash_md5);
    if (id.u109_md5[0])
        mm_info("  DCS u109     : md5 %s", id.u109_md5);
    if (id.u110_md5[0])
        mm_info("  DCS u110     : md5 %s", id.u110_md5);

    /* ---- run stages ----
       A stage returns -1 on failure. Collect that: the run must not report success when it
       produced nothing, or a script around this tool cannot tell the two apart. */
    long n_sounds = 0, n_images = 0, n_fonts = 0, n_messages = 0, n_tables = 0;
    int failed = 0;

    /* The sounds stage needs the DCS set (u109/u110 + the sound flash); a chips-only dump has
       no flash. Skipping it is right when it is only on because it is on by default — but if
       --only named it, the missing input is what the user has to hear about, so let the stage
       run and fail. */
    if ((o.steps & MM_STEP_SOUNDS) && !(asked_for & MM_STEP_SOUNDS)) {
        char su109[1024], su110[1024], sflash[1024];
        mm_resolve_dcs_roms(&o, su109, su110, sflash);
        if (!su109[0] || !su110[0] || !sflash[0]) {
            mm_info("sounds: skipped — no DCS sound flash (needs --bundle's *_sf.rom, or "
                    "--dcs-flash/--dcs-u109/--dcs-u110)");
            o.steps &= ~MM_STEP_SOUNDS;
        }
    }
    if (o.steps & MM_STEP_SOUNDS)
        failed += ((n_sounds = mm_stage_sounds(&o, &c)) < 0);
    if (o.steps & MM_STEP_IMAGES)
        failed += ((n_images = mm_stage_images(&o, &c)) < 0);
    if (o.steps & MM_STEP_FONTS)
        failed += ((n_fonts = mm_stage_fonts(&o, &c)) < 0);
    if (o.steps & MM_STEP_MESSAGES)
        failed += ((n_messages = mm_stage_messages(&o, &c)) < 0);
    if (o.steps & MM_STEP_TABLES)
        failed += ((n_tables = mm_stage_tables(&o, &c)) < 0);

    /* ---- manifest.json + summary ---- */
    long files = 0;
    uint64_t bytes = 0;
    walk_dir(o.out_dir, &files, &bytes);
    char hb[32];

    if (o.steps & MM_STEP_MANIFEST) {
        char mpath[1200];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", o.out_dir);
        FILE *mf = fopen(mpath, "wb");
        if (mf) {
            fprintf(mf, "{\n  \"generator\": \"ReRfM-MarsMiner\",\n");
            if (id.game_version[0])
                fprintf(mf, "  \"game_version\": \"%s\",\n", id.game_version);
            fprintf(mf, "  \"assets\": {\n");
            fprintf(mf, "    \"sounds\":   { \"count\": %ld, \"format\": \"flac\" },\n", n_sounds);
            fprintf(mf, "    \"images\":   { \"count\": %ld, \"format\": \"png-rgba\" },\n",
                    n_images);
            fprintf(mf, "    \"fonts\":    { \"count\": %ld, \"format\": \"png-atlas+csv\" },\n",
                    n_fonts);
            fprintf(mf, "    \"messages\": { \"count\": %ld, \"format\": \"csv\" },\n", n_messages);
            fprintf(mf, "    \"tables\":   { \"count\": %ld, \"format\": \"csv\" }\n", n_tables);
            fprintf(mf, "  },\n  \"total_files\": %ld,\n  \"total_bytes\": %llu\n}\n", files,
                    (unsigned long long)bytes);
            fclose(mf);
        }
    }

    /* ---- package into a .zip container (default) ---- */
    char zip_path[1200] = {0};
    long zip_files = -1;
    if (o.no_zip) {
        /* nothing to pack */
    } else if (failed) {
        mm_warn("not packing a container: %d stage(s) failed — the loose tree in %s is "
                "incomplete, fix the error and re-run",
                failed, o.out_dir);
    } else {
        snprintf(zip_path, sizeof zip_path, "%s.zip", o.out_dir);
        mm_info("packing assets into %s", zip_path);
        zip_files = mm_zip_pack_dir(o.out_dir, zip_path);
        if (zip_files < 0)
            mm_warn("failed to write %s", zip_path);
        else if (!o.keep_loose) {
            /* Default: the zip is the deliverable, so the loose tree goes. --out is a path the
               USER named, so refuse unless it holds only what a run writes — this must never be
               able to eat someone's directory because they pointed --out at the wrong place. */
            if (mm_is_asset_tree(o.out_dir))
                mm_rmtree(o.out_dir);
            else
                mm_warn("keeping %s: it holds entries this run did not write, so it is not "
                        "safe to remove (pass --loose to silence this)",
                        o.out_dir);
        }
    }

    mm_info("done. asset summary:");
    if (o.steps & MM_STEP_SOUNDS)
        mm_info("  sounds   : %ld", n_sounds);
    if (o.steps & MM_STEP_IMAGES)
        mm_info("  images   : %ld frames", n_images);
    if (o.steps & MM_STEP_FONTS)
        mm_info("  fonts    : %ld", n_fonts);
    if (o.steps & MM_STEP_MESSAGES)
        mm_info("  messages : %ld", n_messages);
    if (o.steps & MM_STEP_TABLES)
        mm_info("  tables   : %ld", n_tables);
    if (zip_files >= 0) {
        struct stat zst;
        uint64_t zsz = (stat(zip_path, &zst) == 0) ? (uint64_t)zst.st_size : 0;
        mm_info("  container: %s (%ld files, %s)", zip_path, zip_files, mm_human_size(zsz, hb));
        if (!o.keep_loose)
            mm_info("  (loose tree removed; pass --loose to keep it)");
    } else {
        mm_info("  total    : %ld files, %s in %s", files, mm_human_size(bytes, hb), o.out_dir);
    }

    if (failed)
        mm_warn("%d stage(s) failed — see the messages above", failed);

    mm_aspace_free(&c.aspace);
    mm_symtab_free(&c.symtab);
    mm_buf_free(&c.game_rom);
    mm_buf_free(&c.flash);
    return failed ? 1 : 0;
}
