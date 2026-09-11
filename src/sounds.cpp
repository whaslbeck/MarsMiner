/* sounds.cpp — the DCS-2 audio stage.  Reuses the project's authentic
 * ADSP-2105/SDRC decoder (vendor/dcs/) as an in-process function instead of a
 * separate executable, so MarsMiner stays a single binary.  1:1 with
 * extract.py:step_sounds (the primary decode; the NOKILL music-loop pass is
 * mm_music_loops in music.c).
 *
 * vendor/dcs/dcs2_export.cpp is compiled with -Dmain=dcs2_export_main so its
 * driver becomes a callable function; we set the same environment variables
 * extract.py sets, then invoke it with argv = { "dcs2_export", root, sounds }.
 *
 * The extraction allow-list is derived from the loaded image's own audio tables
 * (mm_used_ids_derive, used_ids.c) — no per-title id list is shipped.
 */
extern "C" {
#include "marsminer.h"
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* the two decode drivers, compiled with -Dmain=dcs2_{export,extract}_main (C++ linkage). */
int dcs2_export_main(int argc, char **argv);
int dcs2_extract_main(int argc, char **argv);
long mm_music_loops(const mm_opts *o, mm_ctx *c, const char *sounds_dir); /* music.c (C++) */

/* ---- DCS flash/sample consistency probe --------------------------------
 * Decode a few known music ids under NOKILL (so the group-KILL can't hide a
 * mismatch by truncating to ~0.03 s) and measure spectral flatness. A self-
 * consistent set decodes music with a peaky spectrum (~0.03..0.18); a mismatched
 * flash/sample pair decodes white noise (~0.7..0.85). Warn loudly on noise.
 * Assumes DCS_U109/U110/FLASH are already set in the environment. */
static std::vector<double> probe_read_wav(const char *path) {
    std::vector<double> out;
    mm_buf b = mm_load(path);
    if (!b.data)
        return out;
    size_t off = 44;
    for (size_t i = 0; i + 4 <= b.len; i++)
        if (memcmp(b.data + i, "data", 4) == 0) {
            off = i + 8;
            break;
        }
    const int16_t *s = (const int16_t *)(b.data + off);
    size_t n = (b.len > off) ? (b.len - off) / 2 : 0;
    out.reserve(n / 2);
    for (size_t i = 0; i + 1 < n; i += 2)
        out.push_back(((double)s[i] + (double)s[i + 1]) * 0.5);
    mm_buf_free(&b);
    return out;
}

static void dcs_consistency_check(const mm_opts *o, const char *sounds_dir) {
    if (o->no_dcs_check)
        return;
    static const char *cand[] = {"0001", "0005", "000b", "0011", "0002"}; /* music ids */
    std::string tmp = std::string(sounds_dir) + "/.dcscheck.wav";
    char root[16];
    snprintf(root, sizeof root, ".");

    double best_flat = 2.0;
    int best_peak = 0;
    int audible = 0;
    for (size_t k = 0; k < sizeof cand / sizeof cand[0]; k++) {
        setenv("DCS_NOKILL", "1", 1);
        setenv("DCS_RENDERBLOCKS", "32", 1); /* ~1 s, enough for flatness */
        setenv("DCS_WAV", tmp.c_str(), 1);
        char *av[4] = {(char *)"dcs2_extract", root, (char *)cand[k], (char *)"4"};
        dcs2_extract_main(4, av);
        std::vector<double> x = probe_read_wav(tmp.c_str());
        remove(tmp.c_str());
        if ((int)x.size() < 8192)
            continue;
        int win = 8192, bi = 0;
        double best = 0; /* loudest window */
        for (int i = 0; i + win < (int)x.size(); i += win / 2) {
            double e = 0;
            for (int j = 0; j < win; j++)
                e += x[i + j] * x[i + j];
            if (e > best) {
                best = e;
                bi = i;
            }
        }
        int peak = 0;
        for (double v : x) {
            int a = (int)std::fabs(v);
            if (a > peak)
                peak = a;
        }
        if (peak < 1200)
            continue; /* not audible — skip */
        audible++;
        double flat = mm_spectral_flatness(&x[bi], win);
        mm_log(1, "  dcs-check id %s: peak=%d flatness=%.3f", cand[k], peak, flat);
        if (flat < best_flat) {
            best_flat = flat;
            best_peak = peak;
        }
    }
    unsetenv("DCS_NOKILL");
    unsetenv("DCS_RENDERBLOCKS");
    unsetenv("DCS_WAV");

    if (audible == 0) {
        mm_warn("DCS consistency: could not probe (no audible reference sound) — proceeding");
        return;
    }
    if (best_flat > 0.35) {
        mm_warn("========================================================================");
        mm_warn("DCS FLASH/SAMPLE MISMATCH DETECTED (reference music decodes to noise:");
        mm_warn("  spectral flatness %.2f, peak %d — a consistent set is ~0.03..0.18).", best_flat,
                best_peak);
        mm_warn("The sound flash and the u109/u110 sample ROMs are from DIFFERENT builds;");
        mm_warn("most sounds will be white noise the group-KILL truncates to ~0.03 s.");
        mm_warn("Use a self-consistent set (flash+u109+u110 from the SAME sound board) via");
        mm_warn("  --dcs-flash FILE  --dcs-u109 FILE  --dcs-u110 FILE");
        mm_warn("========================================================================");
        /* Refuse rather than silently emit noise. The default (chips u109/u110 + the bundle's
           sf.rom) is a mismatched combo; producing broken audio here would be shipped unnoticed.
           --no-dcs-check (handled above) is the deliberate override. */
        mm_die(
            "DCS flash/sample mismatch — refusing to extract noise (see above; or --no-dcs-check)");
    } else if (best_flat < 0.25) {
        mm_info("DCS consistency: OK (reference music flatness %.2f — flash & samples match)",
                best_flat);
    } else {
        mm_warn("DCS consistency: inconclusive (reference flatness %.2f) — spot-check the output",
                best_flat);
    }
}

extern "C" long mm_stage_sounds(const mm_opts *o, mm_ctx *c) {
    (void)c;
    char sounds[1200];
    snprintf(sounds, sizeof sounds, "%s/sounds", o->out_dir);
    mm_mkdir_p(sounds);

    /* resolve the raw DCS ROMs (explicit override, else chips u109/u110 + bundle sf.rom) */
    char u109[1024], u110[1024], flash[1024];
    mm_resolve_dcs_roms(o, u109, u110, flash);
    if (!u109[0] || !u110[0] || !flash[0]) {
        mm_warn("sounds: need DCS ROMs (u109,u110,flash). Give --roms + --bundle or "
                "--dcs-u109/u110/flash.");
        return -1;
    }
    if (!mm_path_exists(u109) || !mm_path_exists(u110) || !mm_path_exists(flash)) {
        mm_warn("sounds: one of the DCS ROMs is missing:\n  u109=%s\n  u110=%s\n  flash=%s", u109,
                u110, flash);
        return -1;
    }
    setenv("DCS_U109", u109, 1);
    setenv("DCS_U110", u110, 1);
    setenv("DCS_FLASH", flash, 1);

    /* Allow-list: restrict the decode to the ids the loaded game can actually play. This is
       DERIVED FROM THE IMAGE here, at extract time (used_ids.c walks its acl tables) — MarsMiner
       ships no per-title play set, so this works for any Pinball 2000 image. An explicit
       --used-ids replaces the derivation; if the derivation fails and no file was given we decode
       every defined id (slow but complete) rather than guess. Derived list goes to work/. */
    char derived[1200];
    derived[0] = 0;
    if (o->all_sounds) {
        unsetenv("DCS_USED_IDS");
        mm_log(1, "  allow-list: none (--all-sounds: decoding every defined id)");
    } else if (o->used_ids && mm_path_exists(o->used_ids)) {
        setenv("DCS_USED_IDS", o->used_ids, 1);
        mm_log(1, "  allow-list: %s (explicit --used-ids)", o->used_ids);
    } else {
        if (o->used_ids)
            mm_warn("  --used-ids %s not readable — deriving from the ROM instead", o->used_ids);
        const char *wd = (o->work_dir && o->work_dir[0]) ? o->work_dir : o->out_dir;
        snprintf(derived, sizeof derived, "%s/dcs_used_ids.rom.txt", wd);
        std::vector<uint16_t> ids(65536);
        long acls = 0;
        long n = mm_used_ids_derive(c, ids.data(), ids.size(), &acls);
        if (n > 0)
            n = mm_used_ids_write(
                derived, ids.data(), n,
                "# dcs_used_ids — the play set of THIS image, derived from its own\n"
                "# *_acl audio tables at extract time (MarsMiner used_ids.c).\n");
        if (n > 0) {
            setenv("DCS_USED_IDS", derived, 1);
            mm_info("  allow-list: %ld play-ids derived from %ld acl tables in the image", n, acls);
        } else {
            unsetenv("DCS_USED_IDS");
            mm_warn("  allow-list: ROM derivation failed — decoding ALL defined ids");
        }
    }

    /* optional id->name table */
    if (o->sound_names && mm_path_exists(o->sound_names))
        setenv("DCS_NAMES", o->sound_names, 1);

    mm_info("sounds: decoding DCS-2 audio (u109/u110 + flash) -> FLAC");
    mm_log(1, "  u109 = %s", u109);
    mm_log(1, "  u110 = %s", u110);
    mm_log(1, "  flash= %s", flash);

    /* verify the flash and sample ROMs are a self-consistent set before the full run */
    dcs_consistency_check(o, sounds);

    /* root: only used by the decoder for default rom paths (all overridden above)
       and the default names path; a harmless placeholder is fine. */
    char root[16];
    snprintf(root, sizeof root, ".");
    char *argv[3] = {(char *)"dcs2_export", root, sounds};
    int rc = dcs2_export_main(3, argv);
    if (rc != 0) {
        mm_warn("sounds: DCS decode returned %d", rc);
    }

    /* music-loop pass (NOKILL re-extract + seamless crossfade), like _music_loops */
    long looped = mm_music_loops(o, c, sounds);
    if (looped > 0)
        mm_log(1, "  music loop pass: %ld tracks", looped);

    /* ship the manifest under the engine's expected index name too */
    char man[1300], idx[1300];
    snprintf(man, sizeof man, "%s/manifest.csv", sounds);
    snprintf(idx, sizeof idx, "%s/index.csv", sounds);
    if (mm_path_exists(man)) {
        mm_buf b = mm_load(man);
        if (b.data) {
            mm_write_file(idx, b.data, b.len);
            mm_buf_free(&b);
        }
    }

    /* count *.flac in the sounds dir */
    long n = 0;
    /* cheap count via manifest lines - 1, else fall back to 0 */
    mm_buf mb = mm_load(man);
    if (mb.data) {
        for (size_t i = 0; i < mb.len; i++)
            if (mb.data[i] == '\n')
                n++;
        if (n > 0)
            n--; /* header */
        mm_buf_free(&mb);
    }
    return n;
}
