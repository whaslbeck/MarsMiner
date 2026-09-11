// license:BSD-3-Clause
/*
 * dcs2_export.cpp — batch DCS audio extractor: render every sound id to completion, classify each
 * as one-shot SFX or looping music, and encode the PCM to Ogg Vorbis.
 *
 *   dcs2_export <roms_dir> <out_dir> [id_lo id_hi]
 *
 * For each id in [id_lo,id_hi] (default 0x0001..0x0b7b (the full RFM command-table range)):
 *   - boot + init the DCS board fresh, send the play triple (id, 0xff7f, chword) on channel 4,
 *   - render in chunks until the decoder goes idle (dequant@0x011a stops advancing) or a cap is hit,
 *   - a sound that idles before the cap is a ONE-SHOT (SFX); one still decoding at the cap is a LOOP,
 *   - trim trailing silence, write <out_dir>/sound/<id>.ogg, and append a manifest row.
 *
 * The render/decode path is the same core proven sample-exact against the reference for SWE1.
 */
#include "dcs2_p2k.h"
#include "flac_encode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <map>
#include <set>
#include <sys/stat.h>

/* debug/host hooks implemented in dcs2_p2k.cpp */
extern void     dcs2_dbg_send(uint16_t cmd);
extern unsigned dcs2_dbg_cmd_count(void);
extern long     dcs2_dbg_pchist_get(uint16_t pc);
extern void     dcs2_dbg_pchist_full_reset(void);
extern int      dcs2_dbg_sport(void);
extern void     dcs2_dbg_abreset(void);

static const int RATE = 31250;          /* DCS native SPORT rate */
static const int BLOCK = 1024;          /* frames per render block */
static const uint16_t DEQUANT_PC = 0x011a;

static std::vector<uint8_t> load(const char *path)
{
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize(n);
    if (fread(v.data(), 1, n, f) != (size_t)n) v.clear();
    fclose(f);
    return v;
}

/* id -> name from the ROM's DCS command table (dcs_cmd,priority,name). The play-triple id equals the
   dcs_cmd word, so this gives every extracted sound its real name (acd_rfm_music_*, acd_fx_*, ...). */
static std::map<uint16_t, std::string> load_names(const char *path)
{
    std::map<uint16_t, std::string> m;
    FILE *f = fopen(path, "r");
    if (!f) return m;
    char line[512];
    fgets(line, sizeof line, f);                     /* header */
    while (fgets(line, sizeof line, f)) {
        unsigned cmd; char name[400];
        /* "0x0019,0,acd_rfm_music_..."; skip the priority field */
        if (sscanf(line, "0x%x,%*[^,],%399[^\r\n]", &cmd, name) == 2 ||
            sscanf(line, "%x,%*[^,],%399[^\r\n]", &cmd, name) == 2)
            m[(uint16_t)cmd] = name;
    }
    fclose(f);
    return m;
}

/* coarse kind from the sound's name, so the manifest distinguishes real music from test tones,
   speech and effects (the "never idles" heuristic alone mislabels sustained tones as music). */
static const char *name_kind(const std::string &n)
{
    auto has = [&](const char *s) { return n.find(s) != std::string::npos; };
    if (n.empty())            return "unknown";
    if (has("sine_wave") || has("_tone") || has("pan_testing")) return "tone";
    if (has("_music_"))       return "music";
    if (has("_fx_") || has("_sfx"))                             return "fx";
    if (has("narrator") || has("clinton") || has("radio_guy") ||
        has("_alien") || has("speech") || has("_vo_") || has("_guy")) return "speech";
    if (has("kill_sound") || has("_ctrl") || has("_cmd"))      return "control";
    return "fx";
}

/* render `blocks` × BLOCK stereo frames, appending to pcm (if non-null), draining host responses
   each block exactly as the guest does (the DSP blocks on a full mailbox otherwise). */
static void render_blocks(int blocks, std::vector<int16_t> *pcm)
{
    int16_t buf[BLOCK * 2];
    for (int b = 0; b < blocks; b++) {
        dcs2_render(buf, BLOCK, RATE);
        for (int d = 0; (dcs2_flag_byte() & 0x80) && d < 16; d++) dcs2_read_response();
        if (pcm) pcm->insert(pcm->end(), buf, buf + BLOCK * 2);
    }
}

/* enqueue a command word and render until the DSP pops it (drives DSP + SPORT clock together) */
static void send_rt(uint16_t cmd)
{
    dcs2_dbg_send(cmd);
    for (int i = 0; i < 64 && dcs2_dbg_cmd_count(); i++) render_blocks(1, nullptr);
}

static void drain_sync(void)
{
    for (int i = 0; i < 8; i++) {
        if (!(dcs2_flag_byte() & 0x80)) break;
        if (!dcs2_read_response()) break;
    }
}

/* full board init: diag (SRAM/ROM/BONG) + let the bong play out + ACE1 stream mode + mixer volumes.
   Must run after every dcs2_prepare() (which resets the DSP) so each id starts from a clean board. */
static void boot_init(void)
{
    dcs2_write_cmd(0x003a); drain_sync();   /* SRAM test  */
    dcs2_write_cmd(0x001b); drain_sync();   /* ROM cksum  */
    dcs2_write_cmd(0x00aa); drain_sync();   /* BONG       */
    for (int i = 0; i < 400 && dcs2_dbg_sport(); i++) render_blocks(1, nullptr);   /* bong out */

    send_rt(0xace1);
    dcs2_read_response(); dcs2_read_response();          /* ace1 ack 0x0100 0x000c */
    send_rt(0x55aa); send_rt(0x609f);                    /* GLOBAL_VOL */
    send_rt(0x55ab); send_rt(0x3fff);                    /* channel VOL */
    send_rt(0x55ac); send_rt(0x3f7f);                    /* channel PAN */
}

struct Result {
    long   blocks;      /* dequant blocks decoded */
    size_t frames;      /* PCM frames after trimming */
    int    peak;
    bool   loop;        /* true = still decoding at the cap (music); false = idled (one-shot) */
    size_t period;      /* detected loop period in frames (0 = none / not a loop) */
};

/* Find the sample-exact loop of a looping track. The DCS decoder is deterministic and its compressed
   stream loops, so once the IMDCT overlap state has re-converged (one loop past the start) the decoded
   PCM repeats BIT-FOR-BIT with the stream's period. We therefore recover the loop by exact-matching
   raw samples, not by lossy cross-correlation — which yields the true FUNDAMENTAL period (a correlation
   peak also fires at every multiple) and a bit-perfect seam.

   Anchors a reference window well past the initial transient, scans for the smallest lag P whose window
   matches exactly, verifies the match over a long span, then walks the loop start back to the earliest
   frame that is still periodic (so the OGG begins at the sound's natural start when there is no intro).
   Sets *loop_start; returns the period in frames, or 0 if the track has no exact loop (through-composed).
   `interleaved` is stereo S16; compares both channels. */
static size_t detect_loop_period(const int16_t *interleaved, size_t frames, int rate, size_t *loop_start)
{
    *loop_start = 0;
    if (frames < (size_t)rate * 4) return 0;

    auto eq = [&](size_t x, size_t y, size_t w) -> bool {         /* stereo-exact window compare */
        return std::memcmp(interleaved + x * 2, interleaved + y * 2, w * 2 * sizeof(int16_t)) == 0;
    };

    const size_t T  = frames / 4;                                 /* skip the pre-convergence transient */
    const size_t Wv = 512;                                        /* per-probe window (frames) */
    const size_t min_p = 256;                                     /* shortest loop we accept */
    const size_t max_p = (frames - T) / 2;                        /* need >= 2 periods to verify */
    if (T + Wv >= frames || max_p <= min_p) return 0;

    /* p is the loop period iff the WHOLE post-transient signal repeats every p samples. Verify that
       globally (sampled windows spanning [T, frames-p]); a merely-local match is not a loop. The
       smallest p that passes is the fundamental. */
    auto global_periodic = [&](size_t p) -> bool {
        size_t hi = frames - p - Wv;
        if (hi <= T) return false;
        size_t step = (hi - T) / 48; if (step < Wv) step = Wv;
        for (size_t i = T; i <= hi; i += step)
            if (!eq(i, i + p, Wv)) return false;
        return true;
    };

    size_t period = 0;
    for (size_t p = min_p; p <= max_p; p++) {
        if (interleaved[T * 2] != interleaved[(T + p) * 2]) continue;   /* fast reject on first sample */
        if (!eq(T, T + p, 64)) continue;                               /* cheap local pre-check */
        if (global_periodic(p)) { period = p; break; }
    }
    if (!period) return 0;

    /* walk the loop start back to the earliest still-periodic frame (natural start / past any intro) */
    size_t s = T;
    size_t cw = period < Wv ? period : Wv;
    while (s >= period && eq(s - period, s, cw)) s -= period;
    *loop_start = s;
    return period;
}

/* Play one id and capture PCM until idle or the cap. Assumes a freshly booted+inited board. */
static Result play_capture(uint16_t id, int channel, int max_seconds, std::vector<int16_t> &pcm)
{
    const uint16_t chword = 0x8000 | (channel << 7);
    dcs2_dbg_abreset();
    dcs2_dbg_pchist_full_reset();               /* zero counts + arm the pc histogram */
    dcs2_dbg_send(id); dcs2_dbg_send(0xff7f); dcs2_dbg_send(chword);

    extern size_t dcs2_dbg_stream_word(void);
    static bool strace = getenv("DCS_STREAMTRACE");
    const int CHUNK = strace ? 1 : 8;            /* render granularity for idle detection */
    const int idle_need = strace ? 32 : 4;       /* consecutive idle chunks => finished */
    const int max_blocks = max_seconds * RATE / BLOCK;
    long prev = -1;
    int idle_run = 0;
    int rendered = 0;
    bool loop = false;
    while (rendered < max_blocks) {
        render_blocks(CHUNK, &pcm);
        rendered += CHUNK;
        if (strace) fprintf(stderr, "[strace] blk=%d frame=%d streamword=%#zx\n",
                            rendered, rendered * BLOCK, dcs2_dbg_stream_word());
        long cur = dcs2_dbg_pchist_get(DEQUANT_PC);
        if (cur == prev) { if (++idle_run >= idle_need) break; }
        else             { idle_run = 0; }
        prev = cur;
    }
    if (rendered >= max_blocks) loop = true;      /* hit the cap without going idle */

    if (getenv("DCS_PCHISTDUMP")) {
        extern void dcs2_dbg_pchist_dumpfile(const char *);
        char p[512]; snprintf(p, sizeof p, "%s.%04x", getenv("DCS_PCHISTDUMP"), id);
        dcs2_dbg_pchist_dumpfile(p);
    }
    long blocks = dcs2_dbg_pchist_get(DEQUANT_PC);

    /* trim trailing near-silence (leaves a one-shot's natural tail, cuts the idle run-out) */
    int peak = 0;
    size_t last_nz = 0;
    for (size_t i = 0; i < pcm.size(); i++) {
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
        if (a > 24) last_nz = i;
    }
    size_t keep_frames = (last_nz / 2) + 1;
    if (keep_frames * 2 < pcm.size()) pcm.resize(keep_frames * 2);

    return Result{ blocks, pcm.size() / 2, peak, loop, 0 };
}

static void mkpath(const std::string &p)
{
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <roms_dir> <out_dir> [id_lo id_hi]\n", argv[0]);
        return 2;
    }
    { extern void dcs2_set_gendtrace(int); if (getenv("DCS_GENDTRACE")) dcs2_set_gendtrace(1); }
    { extern void dcs2_set_dirtrace(int);  if (getenv("DCS_DIRTRACE"))  dcs2_set_dirtrace(1); }
    const char *roms = argv[1];
    std::string out = argv[2];
    unsigned id_lo = argc > 3 ? (unsigned)strtol(argv[3], nullptr, 16) : 0x0001;
    unsigned id_hi = argc > 4 ? (unsigned)strtol(argv[4], nullptr, 16) : 0x0b7b;

    /* ROM resolution: explicit env overrides, else the RFM chip/flash layout under <roms_dir> */
    auto pick = [&](const char *env, const char *rel) {
        const char *e = getenv(env);
        return e ? load(e) : load((std::string(roms) + "/" + rel).c_str());
    };
    /* Canonical source: romset-original (the correctly-read V3.6 dump). It decodes
       coherent audio where the 0180 chips give silence/noise on several ids (0143/0664/
       031d: chips r1 ~0 -> original +0.8..+1.0), and its full extraction is 99% clean on the
       long sounds. Override with DCS_U109/U110/FLASH for a different set. */
    auto u109  = pick("DCS_U109",  "roms/romset-original/rfm_u109.bin");
    auto u110  = pick("DCS_U110",  "roms/romset-original/rfm_u110.bin");
    auto flash = pick("DCS_FLASH", "roms/romset-original/SF.ROM");
    if (u109.empty() || u110.empty() || flash.empty()) {
        fprintf(stderr, "[dcs2_export] ROMs not found under %s (set DCS_U109/U110/FLASH)\n", roms);
        return 1;
    }

    /* arg2 is the sounds output directory itself: <out>/<id>.flac + <out>/manifest.csv */
    mkpath(out);
    std::string snd = out;

    /* id -> real name from the ROM's DCS command table (default work/dcs_sound_commands.csv) */
    std::string names_path = getenv("DCS_NAMES") ? getenv("DCS_NAMES")
                                                 : std::string(roms) + "/work/dcs_sound_commands.csv";
    std::map<uint16_t, std::string> names = load_names(names_path.c_str());

    /* Optional ALLOW-LIST (DCS_USED_IDS=<file>): restrict extraction to the sound ids the game
       actually plays (from the image's own audtab ard_ groups — MarsMiner's src/used_ids.c,
       work/dcs_used_sounds.csv, docs/24). One hex id per line ("0x05ea"/"05ea"/"5ea"); '#' comments
       and any trailing text after the id are ignored, so the CSV (id_hex,...) works directly. When the
       set is empty (no file / unreadable) every id in [id_lo,id_hi] is extracted (backward compatible). */
    std::set<unsigned> used_ids;
    if (const char *up = getenv("DCS_USED_IDS")) {
        if (FILE *uf = fopen(up, "r")) {
            char line[256];
            while (fgets(line, sizeof line, uf)) {
                char *s = line; while (*s == ' ' || *s == '\t') s++;
                if (*s == '#' || *s == '\n' || *s == '\0') continue;
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
                char *end = nullptr; unsigned v = (unsigned)strtol(s, &end, 16);
                if (end != s) used_ids.insert(v);
            }
            fclose(uf);
            printf("[dcs2_export] allow-list: %zu used ids (DCS_USED_IDS=%s)\n", used_ids.size(), up);
        } else {
            fprintf(stderr, "[dcs2_export] warning: DCS_USED_IDS=%s not readable — extracting all ids\n", up);
        }
    }

    int level = getenv("DCS_FLAC_LEVEL") ? atoi(getenv("DCS_FLAC_LEVEL")) : 8;   /* lossless; 8 = smallest */
    /* cap must hold at least two loop periods for detection to lock on; 45 s covers the RFM loops */
    int max_seconds = getenv("DCS_MAXSECS") ? atoi(getenv("DCS_MAXSECS")) : 45;
    int channel = 4;
    long min_frames = RATE / 100;                 /* < 10 ms of audio => treat as empty/no sound */

    FILE *man = fopen((snd + "/manifest.csv").c_str(), "w");
    if (man) fprintf(man, "id,name,kind,blocks,frames,seconds,peak,loop_seconds,file\n");

    int n_out = 0, n_empty = 0;
    printf("[dcs2_export] ids %04x..%04x -> %s  (FLAC L%d, cap=%ds, %zu names)\n",
           id_lo, id_hi, snd.c_str(), level, max_seconds, names.size());

    auto reboot = [&]() -> bool {
        if (!dcs2_prepare(u109.data(), u109.size(), u110.data(), u110.size(),
                          flash.data(), flash.size())) { fprintf(stderr, "prepare failed\n"); return false; }
        boot_init();
        return true;
    };

    /* Capture the board's power-on BONG as the named sound "dcs-bong" (engine scenes play it by name,
       not by DCS id — it's a diagnostic tone, not a numbered play command). Written once. */
    if (id_lo <= 0x0001) {
        if (dcs2_prepare(u109.data(), u109.size(), u110.data(), u110.size(),
                         flash.data(), flash.size())) {
            dcs2_write_cmd(0x003a); drain_sync();
            dcs2_write_cmd(0x001b); drain_sync();
            dcs2_write_cmd(0x00aa); drain_sync();                 /* BONG */
            std::vector<int16_t> bong;
            for (int i = 0; i < 400 && dcs2_dbg_sport(); i++) render_blocks(1, &bong);
            size_t bf = bong.size() / 2;
            while (bf > 0 && bong[(bf - 1) * 2] == 0 && bong[(bf - 1) * 2 + 1] == 0) bf--;  /* trim tail */
            if (bf > (size_t)(RATE / 100)) {
                flac_encode_s16((snd + "/dcs-bong.flac").c_str(), bong.data(), bf, 2, RATE, level);
                printf("  dcs-bong  %.2fs  (named startup tone)\n", (double)bf / RATE);
            }
        }
    }

    /* boot the board ONCE (as the hardware does), then play each id in turn. A one-shot returns the
       DSP to idle, so the next play triple starts clean; a capped LOOP leaves its voice running, so
       we re-boot after those (rare) to guarantee separation. */
    if (!reboot()) return 1;
    bool need_reboot = false;

    for (unsigned id = id_lo; id <= id_hi; id++) {
        if (!used_ids.empty() && used_ids.find(id) == used_ids.end()) continue;  /* skip unused sounds */
        if (need_reboot) { if (!reboot()) return 1; need_reboot = false; }

        std::vector<int16_t> pcm;
        extern void dcs2_dbg_region_reads(long[3]);
        extern void dcs2_dbg_sram_acc(long[4]);
        long rr0[3]; dcs2_dbg_region_reads(rr0);
        long sa0[4]; dcs2_dbg_sram_acc(sa0);
        Result r = play_capture((uint16_t)id, channel, max_seconds, pcm);
        need_reboot = r.loop;

        if (getenv("DCS_REGIONLOG")) {
            long rr1[3]; dcs2_dbg_region_reads(rr1);
            long sa1[4]; dcs2_dbg_sram_acc(sa1);
            long fl = rr1[0]-rr0[0], u9 = rr1[1]-rr0[1], u10 = rr1[2]-rr0[2];
            long smp = u9 + u10;
            const char *reg = (smp == 0) ? "FLASH" : (u9 >= u10 ? "U109" : "U110");
            int peak = 0; for (size_t i = 0; i < pcm.size(); i++) { int a = pcm[i]<0?-pcm[i]:pcm[i]; if (a>peak) peak=a; }
            printf("REGION id=%04x region=%s flash=%ld u109=%ld u110=%ld frames=%ld peak=%d "
                   "sram_dr=%ld sram_dw=%ld sram_pr=%ld sram_pw=%ld\n",
                   id, reg, fl, u9, u10, (long)r.frames, peak,
                   sa1[0]-sa0[0], sa1[1]-sa0[1], sa1[2]-sa0[2], sa1[3]-sa0[3]);
        }

        if (r.blocks == 0 || (long)r.frames < min_frames) { n_empty++; continue; }

        /* sustained sound (a held tone/drone or a music bed): if it repeats as bit-identical PCM,
           trim to a whole number of the sample-exact fundamental period. Storing a few periods
           (>= ~0.5 s) keeps a very short fundamental from repeating too often in the file. FLAC is
           lossless so the loop seam is bit-exact. */
        if (r.loop) {
            size_t s = 0, p = detect_loop_period(pcm.data(), r.frames, RATE, &s);
            if (p) {
                size_t total = pcm.size() / 2;
                size_t reps = 1;
                while (reps * p < (size_t)RATE / 2 && s + (reps + 1) * p <= total) reps++;
                size_t len = reps * p;
                if (s + len <= total) {
                    pcm.erase(pcm.begin(), pcm.begin() + s * 2);   /* drop pre-loop / intro */
                    pcm.resize(len * 2);                            /* keep N whole periods */
                    r.frames = len; r.period = p;                  /* period = fundamental */
                }
            }
        }

        auto it = names.find((uint16_t)id);
        std::string nm = it != names.end() ? it->second : "";
        const char *kind = name_kind(nm);

        char file[32]; snprintf(file, sizeof file, "%04x.flac", id);
        std::string path = snd + "/" + file;
        bool ok = flac_encode_s16(path.c_str(), pcm.data(), r.frames, 2, RATE, level);
        n_out++;

        double secs = (double)r.frames / RATE;
        double loopsecs = (double)r.period / RATE;
        char loopstr[24] = "";
        if (r.period) snprintf(loopstr, sizeof loopstr, "loop %.3fs", loopsecs);
        printf("  %04x  %-7s  %7.2fs  %-12s peak=%-5d  %s%s\n",
               id, kind, secs, loopstr, r.peak, nm.empty() ? file : nm.c_str(),
               ok ? "" : "  [ENCODE FAILED]");
        if (man) fprintf(man, "%04x,%s,%s,%ld,%zu,%.3f,%d,%.3f,%s\n",
                         id, nm.c_str(), kind, r.blocks, r.frames, secs, r.peak, loopsecs, file);
        fflush(stdout);
    }

    if (man) fclose(man);
    printf("[dcs2_export] done: %d sounds, %d empty. manifest=%s/manifest.csv\n",
           n_out, n_empty, snd.c_str());
    return 0;
}
