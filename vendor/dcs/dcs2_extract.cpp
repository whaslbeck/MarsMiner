// license:BSD-3-Clause
/*
 * dcs2_extract.cpp — milestone-3 bring-up: drive the DCS-2 harness through the guest's ACE1 runtime
 * init (traced from Encore's adsp engine), inject a sound-play triple, and render the SPORT PCM.
 *
 * The key over the earlier manual driving: the render loop is INTERLEAVED with the init, mirroring
 * Encore where the audio callback pulls PCM continuously (keeping the SPORT clock + IRQ1 alive)
 * while the guest concurrently posts commands. Without that continuous clock the runtime mixer's
 * SPORT never spins up.
 *
 * Usage: dcs2_extract <repo-root> [sound_id_hex [channel]]
 * Env:  DCS_PCHIST=1  → dump a hot-PC histogram of the render phase
 *       DCS_WAV=path  → write the rendered stereo S16 to a WAV file
 */
#include "dcs2_p2k.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

uint32_t dcs2_dbg_pc(void);
int      dcs2_dbg_sport(void);
void     dcs2_dbg_sport_state(int*,int*,int*,double*,int*);
long    *dcs2_pc_hist(void);
void     dcs2_dbg_disasm(uint16_t start, int count);
uint8_t  dcs2_flag_byte(void);
uint32_t dcs2_dbg_pm(uint16_t a);
uint16_t dcs2_dbg_dm(uint16_t a);
void     dcs2_dbg_send(uint16_t cmd);
unsigned dcs2_dbg_cmd_count(void);
uint32_t dcs2_dbg_imask(void);
uint32_t dcs2_dbg_icntl(void);

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

static const int RATE = 31250;   /* DCS native SPORT rate */

/* render `blocks` × 1024 stereo frames, appending to `pcm`, tracking peak.
   The DSP posts status/idle words to the host mailbox and BLOCKS until the host reads them
   (idle loop @0x3DBE waits for DM(0x0403)&0x40); so drain every pending response each block,
   exactly as the guest does continuously. */
static int g_nodrain = 0;   /* toggled off during the decode-capture render */
static void render_blocks(int blocks, std::vector<int16_t> *pcm, int *peak)
{
    static int force = -1; if (force < 0) force = getenv("DCS_FORCEFLAGS") ? 1 : 0;
    int dmax = getenv("DCS_DRAINMAX") ? atoi(getenv("DCS_DRAINMAX")) : 16;
    int16_t buf[1024 * 2];
    for (int b = 0; b < blocks; b++) {
        if (force) { extern void dcs2_dbg_setdm(uint16_t,uint16_t); dcs2_dbg_setdm(0x387d, 0); dcs2_dbg_setdm(0x0b5d, 0); }
        dcs2_render(buf, 1024, RATE);
        for (int drains = 0; !g_nodrain && (dcs2_flag_byte() & 0x80) && drains < dmax; drains++)
            dcs2_read_response();
        for (int i = 0; i < 1024 * 2; i++) {
            int a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > *peak) *peak = a;
        }
        if (pcm) pcm->insert(pcm->end(), buf, buf + 1024 * 2);
    }
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "../..";
    uint16_t sound_id = argc > 2 ? (uint16_t)strtol(argv[2], nullptr, 16) : 0x03e7;
    int channel       = argc > 3 ? atoi(argv[3]) : 4;

    const char *u109p = getenv("DCS_U109"), *u110p = getenv("DCS_U110"), *flashp = getenv("DCS_FLASH");
    /* Canonical source: romset-original (the correctly-read V3.6 dump — see dcs2_export.cpp). */
    auto u109  = u109p  ? load(u109p)  : load((std::string(root) + "/roms/romset-original/rfm_u109.bin").c_str());
    auto u110  = u110p  ? load(u110p)  : load((std::string(root) + "/roms/romset-original/rfm_u110.bin").c_str());
    auto flash = flashp ? load(flashp) : load((std::string(root) + "/roms/romset-original/SF.ROM").c_str());
    if (u109.empty() || u110.empty() || flash.empty()) {
        printf("[skip] ROMs not found under %s\n", root); return 0;
    }

    if (!dcs2_prepare(u109.data(), u109.size(), u110.data(), u110.size(),
                      flash.data(), flash.size())) {
        printf("prepare FAILED\n"); return 1;
    }
    printf("prepare OK, pc=%04x sport=%d\n", dcs2_dbg_pc(), dcs2_dbg_sport());
    if (getenv("DCS_OPFUZZ")) { extern int dcs2_opfuzz(void); return dcs2_opfuzz(); }
    if (const char *hb = getenv("DCS_HOSTBOOT")) {   /* faithful TWO-STAGE boot like the real machine */
        FILE *f = fopen(hb, "rb");
        if (f) { static uint32_t pm[0x4000]; fread(pm, 4, 0x4000, f); fclose(f);
                 unsigned n = getenv("DCS_HOSTBOOT_N") ? strtol(getenv("DCS_HOSTBOOT_N"),0,16) : 0x100;
                 /* pass 1: run the flash-resident boot loader to completion (pages PM 0x2800-0x3FFF) */
                 extern void dcs2_dbg_runboot(void); dcs2_dbg_runboot();
                 printf("flash boot pass 1 done (pc=%04x)\n", dcs2_dbg_pc() & 0x3fff);
                 /* pass 2: x86 host boot loader (pages PM 0x0173-0x01FE + 0x0800-0x27FF) */
                 extern void dcs2_dbg_hostboot(const uint32_t*,unsigned); dcs2_dbg_hostboot(pm, n);
                 printf("host boot pass 2 done: %x loader words from %s (pc=%04x)\n", n, hb, dcs2_dbg_pc() & 0x3fff); }
    }
    { extern void dcs2_dbg_pchist_full_reset(void); if (getenv("DCS_PCHISTALL")) dcs2_dbg_pchist_full_reset(); }  /* whole-run histogram */
    { extern void dcs2_set_gendtrace(int); if (getenv("DCS_GENDTRACE_EARLY")) { dcs2_set_gendtrace(1);
          extern void dcs2_dbg_pchist_full_reset(void); dcs2_dbg_pchist_full_reset(); } }
    { extern void dcs2_set_romtrace(int); if (getenv("DCS_ROMTRACE_EARLY")) dcs2_set_romtrace(1); }
    { extern void dcs2_set_wtrace(int); if (getenv("DCS_WTRACE")) dcs2_set_wtrace(1); }
    { extern void dcs2_set_dirtrace(int); if (getenv("DCS_DIRTRACE")) dcs2_set_dirtrace(1); }
    { extern void dcs2_dbg_m4watch(int); if (getenv("DCS_M4WATCH")) dcs2_dbg_m4watch(1); }
    { extern void dcs2_dbg_romwatch(int,long*,size_t*,size_t*); dcs2_dbg_romwatch(1,0,0,0); }  /* whole-run compressed-ROM span */

    /* --- COMMAND-STREAM REPLAY: feed a live-game capture (P2K_DCS_CMDLOG: "<pcm_frame> <cmd>" rows)
       into the DSP at the same PCM-frame pacing. The log IS the complete host behaviour (diag, init,
       volumes, play triples, per-tick pacing), so the normal driver init is bypassed entirely. --- */
    if (const char *rp = getenv("DCS_CMDREPLAY")) {
        FILE *rf = fopen(rp, "r");
        if (!rf) { fprintf(stderr, "cannot open replay log %s\n", rp); return 1; }
        std::vector<std::pair<long,uint16_t>> log;
        { unsigned long long fr; unsigned cmd;
          while (fscanf(rf, "%llu %x", &fr, &cmd) == 2) log.push_back({(long)fr, (uint16_t)cmd}); }
        fclose(rf);
        long total_frames = log.empty() ? 0 : log.back().first + 31250;
        if (const char *ex = getenv("DCS_REPLAY_EXTRA")) total_frames += atol(ex) * 31250;
        printf("replay: %zu commands over %.1fs (+tail) from %s\n",
               log.size(), total_frames / 31250.0, rp);
        std::vector<int16_t> pcm;
        pcm.reserve((size_t)total_frames * 2);
        int16_t buf[256 * 2];
        size_t li = 0;
        long frames_done = 0;
        int peak = 0;
        while (frames_done < total_frames) {
            while (li < log.size() && log[li].first <= frames_done) {
                dcs2_write_cmd(log[li].second);   /* full write path (ace1 acks etc.), like the guest */
                li++;
            }
            int n = 256;
            if (li < log.size() && log[li].first - frames_done < n)
                n = (int)(log[li].first - frames_done) > 0 ? (int)(log[li].first - frames_done) : 1;
            dcs2_render(buf, n, 31250);
            static int nodrain = getenv("DCS_REPLAY_NODRAIN") ? 1 : 0;
            if (!nodrain) for (int d = 0; (dcs2_flag_byte() & 0x80) && d < 16; d++) dcs2_read_response();
            for (int i = 0; i < n * 2; i++) { int a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > peak) peak = a; }
            pcm.insert(pcm.end(), buf, buf + n * 2);
            frames_done += n;
        }
        printf("replay done: %ld frames (%.1fs) peak=%d\n", frames_done, frames_done / 31250.0, peak);
        if (const char *dd = getenv("DCS_DUMPDM")) {
            unsigned a0 = strtol(dd, nullptr, 16); const char *c = strchr(dd, ':');
            unsigned n = c ? strtol(c + 1, nullptr, 16) : 16;
            printf("DM[%04x..]:", a0);
            for (unsigned i = 0; i < n; i++) printf(" %04x", dcs2_dbg_dm(a0 + i));
            printf("\n");
        }
        if (const char *hf = getenv("DCS_PCHISTFILE")) {
            extern void dcs2_dbg_pchist_dumpfile(const char*); dcs2_dbg_pchist_dumpfile(hf);
        }
        if (const char *wp = getenv("DCS_WAV")) {
            FILE *w = fopen(wp, "wb");
            if (w) {
                uint32_t nframes = pcm.size() / 2, dbytes = nframes * 4;
                uint32_t r0 = 36 + dbytes, brate = 31250 * 4; uint16_t two = 2, bps = 16, ba = 4, fmt = 1, pcm16 = 16;
                uint32_t rate = 31250;
                fwrite("RIFF", 1, 4, w); fwrite(&r0, 4, 1, w); fwrite("WAVE", 1, 4, w);
                fwrite("fmt ", 1, 4, w); fwrite(&pcm16, 4, 1, w); fwrite(&fmt, 2, 1, w);
                fwrite(&two, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&brate, 4, 1, w);
                fwrite(&ba, 2, 1, w); fwrite(&bps, 2, 1, w);
                fwrite("data", 1, 4, w); fwrite(&dbytes, 4, 1, w);
                fwrite(pcm.data(), 2, pcm.size(), w); fclose(w);
                printf("wrote WAV %s (%u frames)\n", wp, nframes);
            }
        }
        return 0;
    }

    /* keep the SPORT clock alive between/after every command (SETTLE=0 disables) */
    int settle_n = getenv("SETTLE") ? atoi(getenv("SETTLE")) : 2;
    auto settle = [&](int blocks){ if (!settle_n) return; int pk = 0; render_blocks(blocks * settle_n / 2, nullptr, &pk); };

    /* drain every latched DSP->host response word (the guest reads ccXX + 000a after each diag) */
    auto drain = [&](const char *tag){
        for (int i = 0; i < 8; i++) {
            if (!(dcs2_flag_byte() & 0x80)) break;
            uint16_t r = dcs2_read_response();
            printf("  %s read %04x (pc=%04x)\n", tag, r, dcs2_dbg_pc());
            if (!r) break;
        }
    };

    /* render-driven command send: enqueue + raise IRQ2, then render (drives the DSP + SPORT clock
       concurrently, exactly like Encore's audio callback) until the DSP pops it. */
    auto send_rt = [&](uint16_t cmd){
        dcs2_dbg_send(cmd);
        for (int i = 0; i < 64 && dcs2_dbg_cmd_count(); i++) { int pk = 0; render_blocks(1, nullptr, &pk); }
    };

    /* --- diag phase: synchronous (runs before any SPORT playback) --- */
    dcs2_write_cmd(0x003a); drain("003a");   /* SRAM test  */
    if (!getenv("DCS_SKIP001B")) { dcs2_write_cmd(0x001b); drain("001b"); }   /* ROM cksum  */
    dcs2_write_cmd(0x00aa); drain("00aa");   /* BONG       */

    /* let the boot BONG play out under the continuous render clock so the DSP returns to idle */
    for (int i = 0; i < 400 && dcs2_dbg_sport(); i++) { int pk = 0; render_blocks(1, nullptr, &pk); }
    printf("  bong drained: sport=%d pc=%04x flag=%02x\n",
           dcs2_dbg_sport(), dcs2_dbg_pc(), dcs2_flag_byte());

    if (getenv("DCS_DRAINFREE")) g_nodrain = 1;

    /* --- streaming init (render-driven) --- */
    if (getenv("DCS_BA")) { send_rt(0x00ba); printf("  sent 00BA (mixer startup)\n"); }
    send_rt(0xace1);
    printf("  ace1 ack: %04x %04x\n", dcs2_read_response(), dcs2_read_response());
    send_rt(0x55aa); send_rt(0x609f);   /* GLOBAL_VOL */
    send_rt(0x55ab); send_rt(0x3fff);   /* ch VOL     */
    send_rt(0x55ac); send_rt(0x3f7f);   /* ch PAN     */
    printf("  after init: pc=%04x sport=%d flag=%02x imask=%04x icntl=%04x\n",
           dcs2_dbg_pc(), dcs2_dbg_sport(), dcs2_flag_byte(), dcs2_dbg_imask(), dcs2_dbg_icntl());
    if (const char *dr = getenv("DCS_DISRANGE")) {   /* "3dca:40" */
        unsigned s = 0, c = 0; sscanf(dr, "%x:%x", &s, &c);
        fprintf(stderr, "--- disasm %04x..%04x ---\n", s, s + c);
        dcs2_dbg_disasm(s, c);
    }
    if (getenv("DCS_DIS")) {
        fprintf(stderr, "--- engine 0x3980 ---\n"); dcs2_dbg_disasm(0x3980, 0x10);
        fprintf(stderr, "--- stream 0x3d84 ---\n"); dcs2_dbg_disasm(0x3d84, 0x08);
        fprintf(stderr, "--- idle 0x3db0 ---\n");   dcs2_dbg_disasm(0x3db0, 0x18);
        fprintf(stderr, "PM[3980]=%06x PM[3d84]=%06x PM[3dbf]=%06x PM[3deb]=%06x\n",
                dcs2_dbg_pm(0x3980), dcs2_dbg_pm(0x3d84), dcs2_dbg_pm(0x3dbf), dcs2_dbg_pm(0x3deb));
    }

    /* play a sound triple on a channel, then render `blocks` to let it settle */
    auto play = [&](uint16_t id, int ch, int blocks){
        uint16_t cw = 0x8000 | (ch << 7);
        send_rt(id); send_rt(0xff7f); send_rt(cw);
        for (int i = 0; i < blocks; i++) { int pk = 0; render_blocks(1, nullptr, &pk); }
    };

    /* --- prime the decoder like the guest boot: initial sounds on ch4/ch5, render to steady state ---
       (Encore's generate_track inherits an ALREADY-RUNNING decoder the guest spun up this way.) */
    if (const char *pe = getenv("DCS_PRIME")) {
        uint16_t pid = (uint16_t)strtol(pe, nullptr, 16);
        if (pid) {
            printf("prime id=%04x on ch4+ch5, render to steady state\n", pid);
            play(pid, 4, 20);
            play(pid, 5, 20);
            { int b,l,i,pp; double r; dcs2_dbg_sport_state(&b,&l,&i,&r,&pp);
              printf("  after prime: sport=%d inc=%d rate=%.1f pc=%04x DM[387D]=%04x DM[0B5D]=%04x\n",
                     dcs2_dbg_sport(), i, r, dcs2_dbg_pc(), dcs2_dbg_dm(0x387d), dcs2_dbg_dm(0x0b5d)); }
        }
    }

    /* single-step PC trace from the sound-init landmark 0x2810, for the diff vs Encore */
    if (const char *pt = getenv("DCS_PCTRACE")) {
        extern void dcs2_pctrace_start(uint32_t,long,const char*);
        unsigned trig = 0x2810; long n = getenv("DCS_PCTRACEN") ? atol(getenv("DCS_PCTRACEN")) : 20000;
        if (const char *tg = getenv("DCS_PCTRIG")) sscanf(tg, "%x", &trig);
        dcs2_pctrace_start(trig, n, pt);
        printf("pctrace armed: trigger=%04x max=%ld -> %s\n", trig, n, pt);
    }

    /* --- inject the target sound and CAPTURE continuously, like Encore's generate_track (renders
       straight from the play triple). The triple is delivered as three FIFO commands; the DSP's
       IRQ2 handler picks it up and sets up the voice. --- */
    uint16_t chword = 0x8000 | (channel << 7);
    /* Prime the streaming decoder exactly like the P2K runtime does before any track: two priming
       triples (03e7/ff7f/8200 then 03e8/ff7f/8280) set up the DRAM stream state. Without this the
       decoder stalls after ~12 blocks on u109/u110-streamed music. (Disable with DCS_NOPRIME.) */
    extern void dcs2_dbg_send(uint16_t); extern void dcs2_dbg_abreset(void);
    dcs2_dbg_abreset();
    /* Encore's generate_track batches two priming triples (03e7/03e8) before the target. Batching them
       makes multi-voice music (0x0016) set up all its tracks like Encore, BUT it corrupts other sounds
       (0x0014: 109->2 blocks), and even for 0x0016 the compressed decode still stalls because RFM music
       takes a decode path our flash-booted engine routes differently (Encore's dequant@011a=0 for 0x0016,
       ours=13). So priming is OFF by default; enable the batch experiment with DCS_BATCHPRIME. */
    if (getenv("DCS_BATCHPRIME")) {
        dcs2_dbg_send(0x03e7); dcs2_dbg_send(0xff7f); dcs2_dbg_send(0x8200);
        dcs2_dbg_send(0x03e8); dcs2_dbg_send(0xff7f); dcs2_dbg_send(0x8280);
    }
    if (getenv("DCS_PRIME_RT")) {
        /* Encore's exact runtime order: each command word advanced (rendered) before the next, so the
           priming sounds (03e7 g4 / 03e8 g5) are decoding and STILL ACTIVE when the target (g3) starts —
           reproducing the co-active 3-group state its generate_track relies on. */
        send_rt(0x03e7); send_rt(0xff7f); send_rt(0x8200);
        send_rt(0x03e8); send_rt(0xff7f); send_rt(0x8280);
        send_rt(sound_id); send_rt(0xff7f); send_rt(chword);
    } else {
        dcs2_dbg_send(sound_id); dcs2_dbg_send(0xff7f); dcs2_dbg_send(chword);
    }
    { int b,l,i,pp; double r; dcs2_dbg_sport_state(&b,&l,&i,&r,&pp);
      printf("  after inject: sport=%d inc=%d rate=%.1f pc=%04x flag=%02x\n",
             dcs2_dbg_sport(), i, r, dcs2_dbg_pc(), dcs2_flag_byte()); }
    { extern void dcs2_dbg_pchist_full_reset(void); if (!getenv("DCS_PCHISTALL")) dcs2_dbg_pchist_full_reset(); }
    { extern void dcs2_dbg_sdrc2(int,long*,int*,int*); dcs2_dbg_sdrc2(1,0,0,0); }  /* watch the EPM stream page */
    std::vector<int16_t> pcm;
    int peak = 0;
    { extern void dcs2_set_romtrace(int); if (getenv("DCS_ROMTRACE")) dcs2_set_romtrace(1); }  /* trace the sound's own EPM reads */
    { extern void dcs2_set_gendtrace(int); if (getenv("DCS_GENDTRACE")) dcs2_set_gendtrace(1); }  /* group-boundary state */
    if (const char *dp = getenv("DCS_DUMPPM")) {   /* dump my flash-booted PM (0x4000 words, u32 LE) */
        FILE *f = fopen(dp, "wb");
        if (f) { for (unsigned a = 0; a < 0x4000; a++) { uint32_t w = dcs2_dbg_pm(a); fwrite(&w, 4, 1, f); } fclose(f);
                 printf("dumped PM -> %s\n", dp); }
    }
    if (const char *lp = getenv("DCS_LOADPM")) {   /* overwrite engine PM with a captured image, then play */
        FILE *f = fopen(lp, "rb");
        if (f) { static uint32_t pm[0x4000]; size_t n = fread(pm, 4, 0x4000, f); fclose(f);
                 unsigned lo = getenv("DCS_LOADPM_LO") ? strtol(getenv("DCS_LOADPM_LO"),0,16) : 0;
                 unsigned hi = getenv("DCS_LOADPM_HI") ? strtol(getenv("DCS_LOADPM_HI"),0,16) : 0x4000;
                 extern void dcs2_dbg_load_pm(const uint32_t*,unsigned,unsigned); dcs2_dbg_load_pm(pm, lo, hi);
                 printf("loaded PM[%04x..%04x) from %s (%zu words)\n", lo, hi, lp, n); }
    }
    int rblocks = getenv("DCS_RENDERBLOCKS") ? atoi(getenv("DCS_RENDERBLOCKS")) : 120;
    render_blocks(rblocks, &pcm, &peak);    /* ~4 s at 31250, capturing from the play triple */
    { extern long dcs2_dbg_pchist_get(uint16_t);
      printf("DECODE-COUNTS frames=%zu imdct@0088=%ld fill@00bb=%ld window@00a6=%ld imdctpost@00ce=%ld\n"
             "              dequant@011a=%ld eng@28b2=%ld blockdec@36aa=%ld scale@0101=%ld\n",
             pcm.size()/2, dcs2_dbg_pchist_get(0x0088), dcs2_dbg_pchist_get(0x00bb),
             dcs2_dbg_pchist_get(0x00a6), dcs2_dbg_pchist_get(0x00ce),
             dcs2_dbg_pchist_get(0x011a), dcs2_dbg_pchist_get(0x28b2),
             dcs2_dbg_pchist_get(0x36aa), dcs2_dbg_pchist_get(0x0101));
      printf("              gate@2d51=%ld skip@2d52->2e0a=%ld decode@2d59=%ld  routine-entry@2d40=%ld\n",
             dcs2_dbg_pchist_get(0x2d51), dcs2_dbg_pchist_get(0x2d52),
             dcs2_dbg_pchist_get(0x2d59), dcs2_dbg_pchist_get(0x2d40));
      printf("DISPATCH: dispatch@29a2=%ld grpdecode@308a=%ld voicemix@3a40=%ld idle@3dbe=%ld DM[0EC8done]=%04x\n",
             dcs2_dbg_pchist_get(0x29a2), dcs2_dbg_pchist_get(0x308a),
             dcs2_dbg_pchist_get(0x3a40), dcs2_dbg_pchist_get(0x3dbe), dcs2_dbg_dm(0x0ec8));
      printf("VOICEMIX src: vol@3818=%04x index@381A=%04x src-ptr@3816=%04x\n",
             dcs2_dbg_dm(0x3818), dcs2_dbg_dm(0x381a), dcs2_dbg_dm(0x3816));
      printf("  block-ptr table 0x3825[13]:");
      for (int i = 0; i < 13; i++) printf(" %04x", dcs2_dbg_dm(0x3825 + i)); printf("\n");
      uint16_t sp = dcs2_dbg_dm(0x3816);
      printf("  source buffer @%04x[16]:", sp);
      for (int i = 0; i < 16; i++) printf(" %04x", dcs2_dbg_dm((sp + i) & 0x3fff)); printf("\n"); }
    { int b,l,i,pp; double r; dcs2_dbg_sport_state(&b,&l,&i,&r,&pp);
      printf("  post-render SPORT: base=%04x len=%d inc=%d rate=%.1f enabled=%d pc=%04x\n",
             b, l, i, r, dcs2_dbg_sport(), dcs2_dbg_pc()); }

    long nonzero = 0;
    for (int16_t s : pcm) if ((s < 0 ? -s : s) > 16) nonzero++;
    printf("\nRENDER: frames=%zu peak=%d nonzero=%ld (%.1f%%)\n",
           pcm.size() / 2, peak, nonzero, 100.0 * nonzero / pcm.size());
    { int abnz = 0, abpk = 0; for (int a = 0x3400; a < 0x3400 + 960; a++) {
        int16_t v = (int16_t)dcs2_dbg_dm(a); int m = v < 0 ? -v : v; if (m) abnz++; if (m > abpk) abpk = m; }
      int wnz = 0; for (int a = 0x1000; a < 0x1100; a++) if (dcs2_dbg_dm(a)) wnz++;
      printf("autobuffer 0x3400[960]: nonzero=%d peak=%d ; freqbuf 0x1000[256] nonzero=%d\n", abnz, abpk, wnz);
      extern void dcs2_dbg_abwrites(long*,long*,int*); long w,nz; int pk; dcs2_dbg_abwrites(&w,&nz,&pk);
      printf("autobuffer WRITES total=%ld nonzero=%ld wpeak=%d\n", w, nz, pk);
      extern void dcs2_dbg_snap(int*,int*,int*,int*,int*); int sb,sl,si,sp,snz;
      dcs2_dbg_snap(&sb,&sl,&si,&sp,&snz);
      printf("snap: base=%04x len=%d inc=%d peak=%d nonzero=%d\n", sb, sl, si, sp, snz);
      /* block-directory state: gate @0x2d51 reads DM(I0), I0 = 0x0DF6 + (DM(0x0DE3)<<4) + DM(0x0E56) */
      uint16_t de3 = dcs2_dbg_dm(0x0de3), e56 = dcs2_dbg_dm(0x0e56);
      uint16_t i0 = (0x0df6 + (de3 << 4) + e56) & 0x3fff;
      printf("block-dir: DM[0DE3]=%04x DM[0E56]=%04x -> I0=%04x DM[I0]=%04x\n", de3, e56, i0, dcs2_dbg_dm(i0));
      printf("  directory 0x0DF6[24]:"); for (int i = 0; i < 24; i++) printf(" %04x", dcs2_dbg_dm(0x0df6 + i)); printf("\n");
      printf("  ctrl-dir 0x0C25[48]:"); for (int i = 0; i < 48; i++) printf(" %04x", dcs2_dbg_dm(0x0c25 + i)); printf("\n");
      printf("  stream: DM[0DD3..6]=%04x %04x %04x %04x  DM[0DE9(end)]=%04x  SDRC[2]=%04x  DM[0482]=%04x\n",
             dcs2_dbg_dm(0x0dd3), dcs2_dbg_dm(0x0dd4), dcs2_dbg_dm(0x0dd5), dcs2_dbg_dm(0x0dd6),
             dcs2_dbg_dm(0x0de9), dcs2_dbg_dm(0x0482), dcs2_dbg_dm(0x0482));
      if (const char *dd = getenv("DCS_DUMPDM")) { unsigned a=0,c=16; sscanf(dd,"%x:%x",&a,&c);
             printf("  DM[%04x..]:", a); for (unsigned i=0;i<c;i++) printf(" %04x", dcs2_dbg_dm(a+i)); printf("\n"); }
      extern void dcs2_dbg_sdrc2(int,long*,int*,int*); long chg; int lo,hi; dcs2_dbg_sdrc2(1,&chg,&lo,&hi);
      printf("EPM stream page SDRC[2]: changes=%ld range=%04x..%04x\n", chg, lo, hi);
      extern void dcs2_dbg_romwatch(int,long*,size_t*,size_t*); long rr; size_t rlo, rhi; dcs2_dbg_romwatch(-1,&rr,&rlo,&rhi);
      printf("EPM sound-data reads=%ld span=%#zx..%#zx (%.0f KiB) — advancing span = stream continues\n",
             rr, rlo, rhi, (rhi > rlo ? (rhi - rlo) / 1024.0 : 0)); }

    if (const char *wp = getenv("DCS_WAV")) {
        FILE *w = fopen(wp, "wb");
        if (w) {
            uint32_t nframes = pcm.size() / 2, dbytes = nframes * 4;
            uint32_t r0 = 36 + dbytes, brate = RATE * 4; uint16_t two = 2, bps = 16, ba = 4, fmt = 1, pcm16 = 16;
            uint32_t rate = RATE;
            fwrite("RIFF", 1, 4, w); fwrite(&r0, 4, 1, w); fwrite("WAVE", 1, 4, w);
            fwrite("fmt ", 1, 4, w); fwrite(&pcm16, 4, 1, w); fwrite(&fmt, 2, 1, w);
            fwrite(&two, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&brate, 4, 1, w);
            fwrite(&ba, 2, 1, w); fwrite(&bps, 2, 1, w);
            fwrite("data", 1, 4, w); fwrite(&dbytes, 4, 1, w);
            fwrite(pcm.data(), 2, pcm.size(), w); fclose(w);
            printf("wrote WAV %s (%u frames)\n", wp, nframes);
        }
    }

    if (const char *hf = getenv("DCS_PCHISTFILE")) {   /* full per-PC histogram of the render phase */
        extern void dcs2_dbg_pchist_dumpfile(const char*); dcs2_dbg_pchist_dumpfile(hf);
        printf("pchist -> %s\n", hf);
    }
    printf("flag_byte=%02x\n", dcs2_flag_byte());
    if (getenv("DCS_MBOXHEALTH")) {
        extern void dcs2_dbg_mbox_health(long*,long*,long*,long*,unsigned*);
        long enq=0, pop=0, pend=0, re=0; unsigned qc=0;
        dcs2_dbg_mbox_health(&enq, &pop, &pend, &re, &qc);
        printf("[mbox-health] enqueued=%ld consumed=%ld dropped=%ld  still_queued=%u  "
               "render_frames_with_pending_cmd=%ld  irq2_reasserts=%ld\n",
               enq, pop, enq - pop - (long)qc, qc, pend, re);
    }
    { extern void dcs2_dbg_abwpc(void); fprintf(stderr, "=== PCs that wrote non-zero PCM (bong, since track writes none) ===\n"); dcs2_dbg_abwpc(); }
    {   long *h = dcs2_pc_hist();
        printf("decode-path visits: fill@00bb=%ld imdct@0088=%ld window@00a6=%ld decode@28b2=%ld dequant@011a=%ld\n",
               h[0x00bb], h[0x0088], h[0x00a6], h[0x28b2], h[0x011a]);
        printf("fifo-path visits:   sport0rx@3517=%ld fiford@355c=%ld irq2@36dc=%ld  streamrd@3d84=%ld\n",
               h[0x3517], h[0x355c], h[0x36dc], h[0x3d84]);
        printf("wait/other visits:  waitmix@3934=%ld cmdchk@38e6=%ld voicemix@3a42=%ld imdctcross@00ce=%ld\n",
               h[0x3934], h[0x38e6], h[0x3a42], h[0x00ce]);
        printf("PM[0010]=%06x  workDM[0B56]=%04x workDM[0B5D]=%04x DM[387D]=%04x DM[0F03]=%04x\n",
               dcs2_dbg_pm(0x0010), dcs2_dbg_dm(0x0b56), dcs2_dbg_dm(0x0b5d), dcs2_dbg_dm(0x387d), dcs2_dbg_dm(0x0f03));
        { extern void dcs2_dbg_dag(int*,int*,int*); int iv[8],mv[8],lv[8]; dcs2_dbg_dag(iv,mv,lv);
          printf("M0-7 = %04x %04x %04x %04x %04x %04x %04x %04x\n",
                 mv[0]&0xffff,mv[1]&0xffff,mv[2]&0xffff,mv[3]&0xffff,mv[4]&0xffff,mv[5]&0xffff,mv[6]&0xffff,mv[7]&0xffff); }
    }
    if (getenv("DCS_PCHIST")) {
        long *h = dcs2_pc_hist();
        std::vector<std::pair<long,int>> top;
        for (int a = 0; a < 0x4000; a++) if (h[a]) top.push_back({h[a], a});
        std::sort(top.rbegin(), top.rend());
        printf("\nhot PCs (render phase):\n");
        int lo = 0x4000, hi = 0;
        for (int i = 0; i < 24 && i < (int)top.size(); i++) {
            printf("  %04x : %ld\n", top[i].second, top[i].first);
            if (i < 12) { lo = std::min(lo, top[i].second); hi = std::max(hi, top[i].second); }
        }
        fprintf(stderr, "\n--- disasm of hot band %04x..%04x ---\n", lo, hi);
        dcs2_dbg_disasm(lo, hi - lo + 2);
    }
    return 0;
}
