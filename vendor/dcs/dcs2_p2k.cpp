// license:BSD-3-Clause
/*
 * dcs2_p2k.cpp — Pinball-2000 DCS-2 board harness. See dcs2_p2k.h.
 *
 * Faithful port of the DCS-2 board behaviour (SDRC data-memory map, SPORT1 autobuffer, host
 * mailbox, 28F800 flash boot) driving the vendored MAME ADSP-2105 core through emu_shim.h's
 * callback seam. QEMU's threading/worker machinery is dropped: an offline renderer submits a
 * command and steps the DSP to completion synchronously.
 */
#include "emu_shim.h"
#include "adsp2105.h"
#include "dcs2_p2k.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <vector>

/* ---- constants (Pinball-2000 DCS-2) ---- */
#define P2K_DCS_SOUND_FLASH_SIZE (1024 * 1024)
#define P2K_DCS_BANK_SIZE        (8 * 1024 * 1024)   /* u109+u110 interleaved */
#define P2K_DCS_REGION_WORDS     0x600000
#define P2K_DCS_U109_WORD_OFFSET 0x200000
#define P2K_DCS_U110_WORD_OFFSET 0x400000

/* control[] register indices (address 0x3fe0 + index) */
enum {
    S1_AUTOBUF_REG = 15, S1_RFSDIV_REG, S1_SCLKDIV_REG, S1_CONTROL_REG,
    S0_AUTOBUF_REG, S0_RFSDIV_REG, S0_SCLKDIV_REG, S0_CONTROL_REG,
    S0_MCTXLO_REG, S0_MCTXHI_REG, S0_MCRXLO_REG, S0_MCRXHI_REG,
    TIMER_SCALE_REG, TIMER_COUNT_REG, TIMER_PERIOD_REG, WAITSTATES_REG,
    SYSCONTROL_REG,
};

/* ============================================================================================
 *  The DSP driver: a thin subclass exposing the protected drive points the harness needs.
 * ========================================================================================== */
static machine_config s_mcfg;

struct Dsp : public adsp2105_device {
    Dsp() : adsp2105_device(s_mcfg, "dcs", nullptr, 10000000) {}
    void start()                    { device_start(); }   // binds the memory spaces to the callbacks
    void reset()                    { device_reset(); }
    int  execute(int cycles)        { m_icount = cycles; execute_run(); return cycles - m_icount; }
    void set_irq(int line, int st)  { execute_set_input(line, st); }
    uint32_t pc() const             { return m_pc; }
    uint32_t get_i(int n) const     { return m_i[n]; }
    int32_t  get_m(int n) const     { return m_m[n]; }
    uint32_t get_l(int n) const     { return m_l[n]; }
    uint32_t cntr() const           { return m_cntr; }
    /* set I and recompute its circular-buffer base (update_i is inline in another TU) */
    void set_i(int n, uint32_t v)   { m_i[n] = v & 0x3fff; m_base[n] = m_i[n] & m_lmask[n]; }
    int  se() const { return m_core.se.s; }
    uint16_t si() const { return m_core.si.u; }
    uint16_t sr0() const { return m_core.sr.srx.sr0.u; }
    uint16_t sr1() const { return m_core.sr.srx.sr1.u; }
    uint16_t ar() const { return m_core.ar.u; }
    uint16_t ax0() const { return m_core.ax0.u; }
    uint16_t ay0() const { return m_core.ay0.u; }
    uint8_t  px() const { return m_px; }
    uint32_t imask() const { return m_imask; }
    uint32_t icntl() const { return m_icntl; }

    /* -------- opcode self-test (datasheet differential fuzz) helpers -------- */
    void t_pc(uint16_t v)     { m_pc = v; }
    void t_astat(uint16_t v)  { m_astat = v; }
    void t_mstat(uint16_t v)  { m_mstat = v & m_mstat_mask; m_mstat_prev = m_mstat; }
    void t_imask(uint16_t v)  { m_imask = v; }
    void t_icntl(uint16_t v)  { m_icntl = v; }
    void t_check_irqs()       { check_irqs(); }
    uint16_t g_astat() const  { return (uint16_t)m_astat; }
    void s_ax0(uint16_t v) { m_core.ax0.u = v; }  void s_ax1(uint16_t v) { m_core.ax1.u = v; }
    void s_ay0(uint16_t v) { m_core.ay0.u = v; }  void s_ay1(uint16_t v) { m_core.ay1.u = v; }
    void s_ar (uint16_t v) { m_core.ar.u  = v; }  void s_af (uint16_t v) { m_core.af.u  = v; }
    void s_mx0(uint16_t v) { m_core.mx0.u = v; }  void s_my0(uint16_t v) { m_core.my0.u = v; }
    void s_mr0(uint16_t v) { m_core.mr.mrx.mr0.u = v; }
    void s_sr (uint32_t v) { m_core.sr.sr = v; }
    void s_sr0(uint16_t v) { m_core.sr.srx.sr0.u = v; }
    void s_sr1(uint16_t v) { m_core.sr.srx.sr1.u = v; }
    void s_si (uint16_t v) { m_core.si.u = v; }
    void s_se (int v)      { m_core.se.s = (int8_t)v; }
    uint16_t g_ar () const { return m_core.ar.u; }
    uint16_t g_af () const { return m_core.af.u; }
    uint32_t g_sr () const { return m_core.sr.sr; }
    uint64_t g_mr () const { return m_core.mr.mr; }
};
static Dsp s_cpu;

/* ============================================================================================
 *  Harness state
 * ========================================================================================== */
struct Harness {
    std::vector<uint8_t>  flash;         /* 1 MiB sound flash                   */
    std::vector<uint8_t>  sound_rom;     /* 8 MiB u109/u110 interleaved bytes   */
    std::vector<uint16_t> sound_data;    /* 0x600000-word SDRC view             */

    uint16_t control[32] = {};
    uint16_t sdrc[4] = {};
    uint8_t  sdrc_seed = 0;
    uint16_t rom_bank = 0;
    uint16_t sram[0x10000] = {};
    uint16_t data[0x4000] = {};
    uint32_t program[0x4000] = {};       /* internal PM (< 0x0800); filled by the flash boot */
    uint8_t  boot_page[0x1000] = {};     /* the boot-loader image, for the host-boot re-upload */

    /* host<->DSP mailbox */
    uint16_t commands[65536] = {};
    unsigned command_head = 0, command_count = 0;
    uint16_t output_data = 0, output_control = 0;
    bool     output_full = false;
    unsigned output_writes = 0;
    /* board-synthesized responses (SDRC security / script-mode acks) — not from the DSP mailbox */
    uint16_t host_ack[2] = {0, 0};
    unsigned host_ack_head = 0, host_ack_count = 0;

    /* SPORT1 autobuffer */
    bool   sport_enabled = false;
    int    ireg = 0, increment = 0, length = 0, base = 0, play_pos = 0, next_irq_pos = 0;
    double source_rate = 0, source_phase = 0, cycle_phase = 0;
    int16_t last_sample[2] = {0, 0};
    int16_t snap[0x1000] = {};            /* harness-side double buffer of the SPORT autobuffer */

    /* ADSP-2105 programmable timer: TCOUNT decrements every (TSCALE+1) cycles; on underflow it
       reloads TPERIOD and raises the TIMER interrupt (vector 0x0018). MAME drives this from a host
       timer via m_timer_fired_cb; offline we count cycles here. The DCS engine enables it (MSTAT
       bit5) while streaming — its ISR services the SDRC page, keeping continuous playback alive. */
    bool   timer_enabled = false;
    int    timer_subcycle = 0;            /* cycle accumulator within one TSCALE tick */

    bool initialized = false;
};
static Harness H;

/* ============================================================================================
 *  Memory map — the SDRC data space, program space, SPORT tx. (Callbacks the shim dispatches to.)
 * ========================================================================================== */
static bool g_romtrace = false;
/* command-1 (segment-chain) data-path trace: capture every ROM read the next-segment
   descriptor fetch performs, plus the assembled 24-bit address, to compare vs raw ROM. */
bool g_cmd1trace = false;
bool g_cmd1_active = false;
long g_cmd1_reads = 0;

/* mailbox-command health (the Encore "mailbox starvation" check, docs/31 lead): count how many
   host commands are enqueued vs actually popped by the DSP, and how many render frames run with a
   command still pending (the synchronous analog of Encore's starvation). DCS_MBOXFIX = re-assert
   IRQ2 in the render loop whenever a command is pending (the Encore fix), to test if it changes
   the decode at all. */
long g_mbox_enq = 0, g_mbox_pop = 0, g_mbox_render_pending_frames = 0, g_mbox_reassert = 0;
static int g_mboxfix = -1;
u16 dcs_data_read_impl(offs_t address);
static bool g_romwatch = false; static long g_rom_reads = 0; static size_t g_rom_min = ~0ULL, g_rom_max = 0;
static size_t g_stream_word = 0;
size_t dcs2_dbg_stream_word(void){ return g_stream_word; }
/* per-region compressed-stream read tally, for classifying which ROM a sound streams from
   ([0]=flash word<0x200000, [1]=U109 0x200000-0x3fffff, [2]=U110 >=0x400000). Caller diffs
   across a play (no reset) to get the region mix of one sound. */
static long g_reg_reads[3] = {0, 0, 0};
void dcs2_dbg_region_reads(long out[3]){ out[0]=g_reg_reads[0]; out[1]=g_reg_reads[1]; out[2]=g_reg_reads[2]; }

/* ---- experimental sample-ROM ADDRESS transform (descramble search, DCS_U109XFORM) ----
   Applies a candidate address-line/offset transform to reads from the U109/U110 sample ROMs only
   (flash is untouched), so we can hunt the socket-wiring scramble by decoding + scoring vs the master
   WAV. Spec (env DCS_U109XFORM): "swapA:i:j" swap address bits i,j; "off:N" add N (signed) words;
   "rev:n" reverse the low n address bits; combine with '+' e.g. "swapA:3:4+off:-2". Applied to the
   chip-relative word (word - chipbase). g_xf_* parsed once. */
static bool g_xf_parsed = false, g_xf_on = false;
static long g_sample_shift = 0;   /* DCS_SAMPLE_SHIFT: signed word delta added to every sample-ROM read
                                     (word>=0x200000) — relocates a whole sample-ROM stream, for the
                                     ground-truth address sweep (find a sound's TRUE ROM location). */
static int  g_xf_swap_i = -1, g_xf_swap_j = -1, g_xf_rev_n = 0; static long g_xf_off = 0;
static void xf_parse(){
    g_xf_parsed = true; const char *s = getenv("DCS_U109XFORM"); if (!s) return; g_xf_on = true;
    /* very small hand parser over '+'-separated ops */
    std::string spec = s;
    size_t p = 0;
    while (p < spec.size()) {
        size_t e = spec.find('+', p); std::string op = spec.substr(p, e==std::string::npos?e:e-p);
        if (op.rfind("swapA:",0)==0){ int i,j; if(sscanf(op.c_str()+6,"%d:%d",&i,&j)==2){g_xf_swap_i=i;g_xf_swap_j=j;} }
        else if (op.rfind("off:",0)==0){ g_xf_off = atol(op.c_str()+4); }
        else if (op.rfind("rev:",0)==0){ g_xf_rev_n = atoi(op.c_str()+4); }
        if (e==std::string::npos) break; p = e+1;
    }
}
static inline size_t xf_apply(size_t rel){
    if (g_xf_swap_i>=0){ size_t bi=(rel>>g_xf_swap_i)&1, bj=(rel>>g_xf_swap_j)&1;
        rel &= ~((size_t)1<<g_xf_swap_i); rel &= ~((size_t)1<<g_xf_swap_j);
        rel |= bj<<g_xf_swap_i; rel |= bi<<g_xf_swap_j; }
    if (g_xf_rev_n>0){ size_t low=0; for(int k=0;k<g_xf_rev_n;k++) if((rel>>k)&1) low|=(size_t)1<<(g_xf_rev_n-1-k);
        rel = (rel & ~(((size_t)1<<g_xf_rev_n)-1)) | low; }
    if (g_xf_off) rel = (size_t)((long)rel + g_xf_off);
    return rel;
}
static long g_sram_acc[4]={0,0,0,0}; /* [0]data-rd [1]data-wr [2]prog-rd [3]prog-wr */
void dcs2_dbg_sram_acc(long o[4]){o[0]=g_sram_acc[0];o[1]=g_sram_acc[1];o[2]=g_sram_acc[2];o[3]=g_sram_acc[3];}
void dcs2_dbg_romwatch(int on, long *reads, size_t *lo, size_t *hi){ if (on >= 0) { g_romwatch = on; if (on){g_rom_reads=0;g_rom_min=~0ULL;g_rom_max=0;} } if(reads)*reads=g_rom_reads; if(lo)*lo=g_rom_min; if(hi)*hi=g_rom_max; }
u16 dcs_data_read(offs_t address)
{
    u16 v = dcs_data_read_impl(address);
    if (g_romtrace) {
        unsigned rom_st = H.sdrc[0] & 3;
        unsigned rom_base = rom_st == 0 ? 0x0000 : rom_st == 1 ? 0x3000 : 0x3400;
        unsigned page_words = (rom_st != 0 && !(H.sdrc[0] & 0x10)) ? 4096 : 1024;
        bool in_epm = (rom_st != 3) && (H.sdrc[0] & 0x20) &&
                      address >= rom_base && address < rom_base + page_words;
        bool in_bank = address >= 0x2000 && address <= 0x2fff;
        if (in_epm || in_bank) {
            static long n = 0;
            if (n++ < 300) {
                const char *reg; size_t word;
                if (in_bank) {
                    size_t off = ((size_t)(H.rom_bank & 0x7ff) << 12) | (address - 0x2000);
                    reg = "BANK(u109/u110 byte)"; word = off;
                } else {
                    word = ((size_t)(H.sdrc[2] & 0x1fff) * page_words + address - rom_base)
                           % H.sound_data.size();
                    reg = word >= 0x400000 ? "U110" : word >= 0x200000 ? "U109" : "FLASH";
                }
                fprintf(stderr, "[romrd] DM[%04x]=%04x  %-20s word/off=%#zx  sdrc2=%04x bank=%03x pc=%04x\n",
                        address, v, reg, word, H.sdrc[2], H.rom_bank, s_cpu.pc());
            }
        }
    }
    return v;
}
void dcs2_set_romtrace(int on) { g_romtrace = on; }
u16 dcs_data_read_impl(offs_t address)
{
    if (address == 0x0400) {
        uint16_t c = H.command_count ? H.commands[H.command_head] : 0;
        if (getenv("DCS_CMDTRACE")) { static int r=0; if(r++<30) fprintf(stderr,"[rd cmd] 0x0400 -> %04x (cnt=%u pc=%04x)\n", c, H.command_count, s_cpu.pc()); }
        return c;
    }
    if (address == 0x0402) return H.output_control;
    if (address == 0x0403) return (H.command_count ? 0x80 : 0) | (H.output_full ? 0 : 0x40);
    if (address >= 0x0480 && address <= 0x0483) {
        unsigned reg = address - 0x0480;
        if (reg != 3) return H.sdrc[reg];
        static const uint16_t security[8] = {
            0x5a81, 0x5aa4, 0x5a00, 0x5ab9, 0x5a03, 0x5a69, 0x5a20, 0x5aff };
        unsigned mode = (H.sdrc[0] >> 13) & 7;
        return mode == 2 ? 0x5a00 | ((H.sdrc_seed & 0x3f) << 1) : security[mode];
    }

    unsigned rom_st = H.sdrc[0] & 3;
    bool rom_enabled = rom_st != 3;
    unsigned rom_base = rom_st == 0 ? 0x0000 : rom_st == 1 ? 0x3000 : 0x3400;
    unsigned page_words = (rom_st != 0 && !(H.sdrc[0] & 0x10)) ? 4096 : 1024;
    if (rom_enabled && address >= rom_base && address < rom_base + page_words) {
        if (H.sdrc[0] & 0x20) {
            size_t word = ((size_t)(H.sdrc[2] & 0x1fff) * page_words + address - rom_base)
                          % H.sound_data.size();
            if (!g_xf_parsed) xf_parse();
            if (g_xf_on && word >= 0x200000) {          /* transform sample-ROM reads only */
                size_t base = word < 0x400000 ? 0x200000 : 0x400000;
                size_t rel = xf_apply(word - base) & (0x200000 - 1);
                word = base + rel;
            }
            if (g_sample_shift && word >= 0x200000)     /* DCS_SAMPLE_SHIFT: relocate the whole sample-ROM stream */
                word = (word + (size_t)g_sample_shift) % H.sound_data.size();
            if (getenv("DCS_ACCLOG") && (H.sdrc[2] & 0xe000)) {
                static long n = 0;
                if (n++ < 24) fprintf(stderr, "[acc-R] DM[%04x] sdrc0=%04x sdrc1=%04x sdrc2=%04x pc=%04x\n",
                                      address, H.sdrc[0], H.sdrc[1], H.sdrc[2], s_cpu.pc() & 0x3fff);
            }
            if (g_romwatch) { g_rom_reads++; if (word < g_rom_min) g_rom_min = word; if (word > g_rom_max) g_rom_max = word; }
            g_stream_word = word;    /* latest compressed-stream read offset (for loop-wrap detection) */
            g_reg_reads[word < 0x200000 ? 0 : word < 0x400000 ? 1 : 2]++;   /* region tally */
            if (g_cmd1_active && g_cmd1_reads < 60) {
                g_cmd1_reads++;
                const char *reg = word<0x80000?"FLASH":word<0x200000?"GAP":word<0x400000?"U109":"U110";
                fprintf(stderr, "    [rom-read] pc=%04x DM$%04x sdrc2=%04x rom_bank=%04x sdrc0=%04x -> word=%#08zx %-5s val=%04x\n",
                        s_cpu.pc()&0x3fff, address, H.sdrc[2], H.rom_bank, H.sdrc[0], word, reg, H.sound_data[word]);
            }
            if (getenv("DCS_CKTRACE")) {
                unsigned pc = s_cpu.pc() & 0x3fff;
                if (pc >= 0x3900 && pc < 0x3a00) {
                    static unsigned last_pg = 0xffff; static long ck = 0;
                    unsigned pg = H.sdrc[2] & 0x1fff;
                    if (pg != last_pg && ck++ < 80) {
                        fprintf(stderr, "[ck] page=%04x pw=%u -> word=%#zx region=%s val=%04x pc=%04x\n",
                                pg, page_words, word,
                                word<0x80000?"FLASH":word<0x200000?"GAP":word<0x400000?"U109":"U110",
                                H.sound_data[word], pc);
                        last_pg = pg;
                    }
                }
            }
            if (getenv("DCS_U109TRACE") && word >= 0x200000 && (s_cpu.pc()&0x3fff) < 0x3900) {
                static long ut = 0;
                if (ut++ < 60) fprintf(stderr, "[u109] addr=%04x rom_base=%04x epmpg=%04x pw=%u -> word=%#zx val=%04x pc=%04x\n",
                                       address, rom_base, H.sdrc[2]&0x1fff, page_words, word, H.sound_data[word], s_cpu.pc()&0x3fff);
            }
            if (getenv("DCS_EPMTRACE")) { static long e=0; if(e++<40) fprintf(stderr,"[epm] #%ld addr=%04x epmpg=%04x pw=%u -> word=%#zx val=%04x (%s) pc=%04x\n", e, address, H.sdrc[2]&0x1fff, page_words, word, H.sound_data[word], word<0x80000?"FLASH":word<0x200000?"GAP":word<0x400000?"U109":"U110", s_cpu.pc()&0x3fff); }
            return H.sound_data[word];
        }
        unsigned page = (H.sdrc[0] >> 7) & 7;
        size_t word = (size_t)page * page_words + address - rom_base;
        size_t byte = (word * 2) & (P2K_DCS_SOUND_FLASH_SIZE - 1);
        return (uint16_t)(H.flash[byte] | (H.flash[byte + 1] << 8));
    }

    unsigned dm_st = H.sdrc[1] & 3;
    unsigned dm_base = dm_st == 1 ? 0x0000 : dm_st == 2 ? 0x3000 : 0x3400;
    if (dm_st && address >= dm_base && address < dm_base + 0x400) {
        size_t word = ((size_t)(H.sdrc[2] & 0x7ff) * 1024 + address - dm_base) % H.sound_data.size();
        if (getenv("DCS_DMPAGE") && (s_cpu.pc()&0x3fff) < 0x3900) {
            static long dp = 0;
            if (dp++ < 40) fprintf(stderr, "[dmpg] addr=%04x sdrc2=%04x pg(&7ff)=%04x -> word=%#zx val=%04x region=%s pc=%04x\n",
                address, H.sdrc[2], H.sdrc[2]&0x7ff, word, H.sound_data[word],
                word<0x80000?"FLASH":word<0x200000?"GAP(0)":word<0x400000?"U109":"U110", s_cpu.pc()&0x3fff);
        }
        return H.sound_data[word];
    }

    bool sm_enabled = (H.sdrc[0] & 0x0800) != 0;
    bool sm_bank = (H.sdrc[0] & 0x1000) != 0;
    if (sm_enabled) {
        if (!sm_bank && address >= 0x0800 && address <= 0x17ff) { g_sram_acc[0]++; return H.sram[address - 0x0800]; }
        if (address >= 0x1800 && address <= 0x27ff)
            { g_sram_acc[0]++; return H.sram[(sm_bank ? 0x3000 : 0x1000) + address - 0x1800]; }
        if (address >= 0x2800 && address <= 0x37ff) { g_sram_acc[0]++; return H.sram[0x2000 + address - 0x2800]; }
    }

    if (address >= 0x3800 && address <= 0x39ff) return H.data[address];
    if (address >= 0x2000 && address <= 0x2fff) {
        size_t offset = ((size_t)(H.rom_bank & 0x7ff) << 12) | (address - 0x2000);
        return H.sound_rom[offset & (P2K_DCS_BANK_SIZE - 1)];
    }
    if (address >= 0x3fe0) return H.control[address - 0x3fe0];
    return 0xffff;
}

static long g_wrhist[16];
static long g_pchist[0x4000];
static bool g_wtrace = false;
void dcs2_set_wtrace(int on){ g_wtrace = on; }
static bool g_dirtrace = false;
void dcs2_set_dirtrace(int on){ g_dirtrace = on; }
/* watch for spurious writes to M4 (should only ever be set to 0 at 0x2810) */
bool g_m4watch = false;
int32_t g_m4watch_prev = 0;
void adsp_m4changed(uint32_t pc, int32_t oldv, int32_t newv, uint32_t op){
    static int n = 0;
    if (n++ < 30) fprintf(stderr, "[M4] pc=%04x op=%06x  %04x -> %04x\n", pc, op & 0xffffff, oldv & 0xffff, newv & 0xffff);
}
void dcs2_dbg_m4watch(int on){ g_m4watch = on ? 1 : 0; }
static long g_ab_writes = 0, g_ab_nzwrites = 0; static int g_ab_wpeak = 0;
static bool g_sdrc2watch = false; static long g_sdrc2_changes = 0; static int g_sdrc2_min = 0xffff, g_sdrc2_max = 0;
void dcs2_dbg_sdrc2(int on, long *chg, int *lo, int *hi){ g_sdrc2watch = on; if (on){g_sdrc2_changes=0;g_sdrc2_min=0xffff;g_sdrc2_max=0;} if(chg)*chg=g_sdrc2_changes; if(lo)*lo=g_sdrc2_min; if(hi)*hi=g_sdrc2_max; }
static long g_ab_wpc[0x4000];
void dcs2_dbg_abwpc(void){ /* dump top PCs that write non-zero PCM */
    for (int r = 0; r < 8; r++) { int mi = 0; for (int i = 1; i < 0x4000; i++) if (g_ab_wpc[i] > g_ab_wpc[mi]) mi = i;
        if (!g_ab_wpc[mi]) break; fprintf(stderr, "  ab-write PC=%04x : %ld\n", mi, g_ab_wpc[mi]); g_ab_wpc[mi] = 0; } }
void dcs2_dbg_abwrites(long *w, long *nz, int *pk){ *w = g_ab_writes; *nz = g_ab_nzwrites; *pk = g_ab_wpeak; }
void dcs2_dbg_abreset(void){ g_ab_writes = g_ab_nzwrites = 0; g_ab_wpeak = 0; for (auto &s : H.snap) s = 0; }
void dcs2_dbg_snap(int *base,int *len,int *inc,int *peak,int *nz){
    *base=H.base; *len=H.length; *inc=H.increment; int pk=0,c=0;
    for(int i=0;i<H.length&&i<0x1000;i++){int m=H.snap[i]<0?-H.snap[i]:H.snap[i]; if(m){c++; if(m>pk)pk=m;}}
    *peak=pk; *nz=c;
}

/* per-instruction PC trace for the single-step diff vs Encore */
bool g_adsp_pctrace_enabled = false;
static uint32_t g_pct_trigger = 0;
static long g_pct_count = 0, g_pct_max = 0;
static FILE *g_pct_fp = nullptr;
static bool g_pct_armed = false;
static long g_pchist_full[0x4000];   /* per-instruction histogram (via the pctrace hook) */
long dcs2_dbg_pchist_get(uint16_t pc){ return g_pchist_full[pc & 0x3fff]; }
void dcs2_dbg_pchist_dumpfile(const char *path){
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (unsigned a = 0; a < 0x4000; a++)
        if (g_pchist_full[a]) fprintf(f, "%04x %ld\n", a, g_pchist_full[a]);
    fclose(f);
}
void dcs2_dbg_pchist_full_reset(void){ memset(g_pchist_full, 0, sizeof g_pchist_full); g_adsp_pctrace_enabled = true; }
static bool g_gendtrace = false;
void dcs2_set_gendtrace(int on){ g_gendtrace = on; }
void adsp_pctrace(uint32_t pc)
{
    g_pchist_full[pc & 0x3fff]++;    /* count every executed instruction */
    if (g_cmd1trace) {
        unsigned p = pc & 0x3fff;
        static long blk = 0; static uint16_t lastcode = 0;
        if (p == 0x2d32) {   /* frame-header read: AR=DM(I6,M5); (word&0x7F)==0x7F => clean segment end */
            unsigned i6 = s_cpu.get_i(6) & 0x3fff;
            uint16_t w = (i6>=0x800 && i6<=0x17ff) ? H.sram[i6-0x800] : dcs_data_read(i6);
            unsigned framec = dcs_data_read(0x0e56);
            uint16_t m6 = dcs_data_read((0x1140 + framec) & 0x3fff);
            static long fr = 0;
            if (fr++ < 30)
                fprintf(stderr, "  [frame %u] hdr-word=%04x low7=%02x %s  M6tbl[%04x]=%d\n",
                        framec, w, w & 0x7f, (w&0x7f)==0x7f ? "<<TERMINATOR (clean end)" : "",
                        (0x1140+framec)&0x3fff, (int16_t)m6);
        }
        /* MILESTONE-1 bit-cursor trace (DCS_FRAMETRACE): per frame, log the header-stream position
           (byte reader ptr DM$0DE1:page/DM$0DE2:pos) + code + M6 + the coefficient-stream word, so
           the two streams' divergence and the missed terminator can be pinpointed. */
        if (getenv("DCS_FRAMETRACE") && p == 0x2e4a) {  /* allocation reader ROM read (0x2E35 x2e49) */
            extern size_t dcs2_dbg_stream_word(void); size_t w=dcs2_dbg_stream_word();
            const char *reg = w<0x80000?"FLASH":w<0x200000?"GAP":w<0x400000?"U109":"U110";
            static long an=0; if(an++<24) fprintf(stderr,"[ALLOC-read] word=%#08zx %-5s\n", w, reg);
        }
        if (getenv("DCS_FRAMETRACE") && p == 0x2d32) {
            extern size_t dcs2_dbg_stream_word(void);
            unsigned i6 = s_cpu.get_i(6) & 0x3fff;
            uint16_t w = (i6>=0x800 && i6<=0x17ff) ? H.sram[i6-0x800] : dcs_data_read(i6);
            unsigned framec = dcs_data_read(0x0e56);
            unsigned page = dcs_data_read(0x0de1), pos = dcs_data_read(0x0de2);
            static long fr = 0;
            fprintf(stderr, "[FT] fr=%3ld framec=%3u code=%02x %s hdrbuf_i6=%04x hdr=%04x  streamptr page=%04x pos=%04x  coef_word=%#08zx\n",
                    fr++, framec, w & 0x7f, (w&0x7f)==0x7f?"TERM":"    ", i6, w, page, pos, dcs2_dbg_stream_word());
        }
        if (p == 0x00f2) {   /* dequant frame setup: codebook/table pointers DM[0x0F00..0x0F03] */
            static long fs = 0;
            if (fs++ < 8)
                fprintf(stderr, "  [frame-setup] DM$0F00=%04x DM$0F01=%04x DM$0F02=%04x DM$0F03=%04x  "
                        "I1..I5=%04x/%04x/%04x/%04x\n",
                        dcs_data_read(0x0f00), dcs_data_read(0x0f01), dcs_data_read(0x0f02), dcs_data_read(0x0f03),
                        (unsigned)s_cpu.get_i(1), (unsigned)s_cpu.get_i(2), (unsigned)s_cpu.get_i(4), (unsigned)s_cpu.get_i(5));
        }
        if (p == 0x011a) {   /* dequant entry: CNTR = M6 (coefficient budget for this block) */
            extern size_t dcs2_dbg_stream_word(void);
            static long dq = 0;
            fprintf(stderr, "  [dequant #%ld] M6(->CNTR)=%d  stream_word=%#zx\n",
                    ++dq, (int)s_cpu.get_m(6), dcs2_dbg_stream_word());
            if (dq == 3) {   /* voice metadata resolved: Encore reads sram[0x455]=DM0xC55, sram[0x4b0]=DM0xCB0 */
                fprintf(stderr, "  --- voice_head DM[0x0C55..0x0C64] (16w): ");
                for (int a=0x0c55;a<=0x0c64;a++) fprintf(stderr,"%04x ", dcs_data_read(a));
                fprintf(stderr, "\n  --- voice_body DM[0x0CB0..0x0CE2] (51w): ");
                for (int a=0x0cb0;a<=0x0ce2;a++) fprintf(stderr,"%04x ", dcs_data_read(a));
                fprintf(stderr, "\n");
            }
        }
        if (p == 0x0156) {   /* kill decision: SR1=CNTR; IF GT skips the kill. Log the danger zone. */
            int c = (int16_t)s_cpu.cntr();
            static long kl = 0;
            if (c <= 1 && kl++ < 24)
                fprintf(stderr, "    [kill-check] CNTR=%d SE=%d SI=%04x AR=%04x (CNTR<=0 => 0xEDDE+kill)\n",
                        c, (int)s_cpu.se(), s_cpu.si(), s_cpu.ar());
        }
        if (p == 0x309d) { lastcode = s_cpu.ar(); ++blk; }
        if (p == 0x30a5) {   /* AR = DM[0x0DAF+grp] (segment-end match target), AY1 = code */
            uint16_t tgt = s_cpu.ar(), code = s_cpu.ay0()/*grp add*/; (void)code;
            /* AY1 holds the code here; read it via af path is hard — recompute from lastcode */
            static long shown = 0;
            if (blk <= 6 || (tgt == lastcode)) {
                if (shown++ < 40)
                    fprintf(stderr, "[blk %ld] code=%04x  match-target DM[0x0DAF+grp]=%04x  -> %s\n",
                            blk, lastcode, tgt,
                            lastcode==0xffff ? "CONTINUE(0xffff)" : (tgt==lastcode ? "MATCH => dispatch/END" : "continue(no match)"));
            }
        }
        if (p == 0x2a60) {
            unsigned page = dcs_data_read(0x0de1), pos = dcs_data_read(0x0de2);
            size_t word = ((size_t)page << 9) | (pos >> 1);
            const char *reg = word<0x200000?"FLASH":word<0x400000?"U109":word<0x600000?"U110":"?";
            fprintf(stderr, "        dispatch segment-command = %u   [stream page=%04x pos=%04x -> word=%#zx %s]\n",
                    s_cpu.ar() & 0xff, page, pos, word, reg);
        }
        if (p == 0x30e5) fprintf(stderr, "        *** GROUP KILLED @0x30e5 (stream ptr 0x0E6A zeroed) ***\n");
        static int win = 0;
        if (p == 0x2a9a && win < 30) {          /* entering command-1 (next-segment install) */
            win++; g_cmd1_active = true; g_cmd1_reads = 0;
            fprintf(stderr, "\n=== command-1 #%d @0x2A9A  (sound continuation-code was NON-0xFFFF => dispatch) ===\n", win);
            fprintf(stderr, "  stream ptr  DM$0DE1(page)=%04x DM$0DE2(pos)=%04x   snd PM$3FEC(page)=%06x PM$3FEE(pos)=%06x\n",
                    dcs_data_read(0x0de1), dcs_data_read(0x0de2), dcs_program_read(0x3fec), dcs_program_read(0x3fee));
        }
        if (g_cmd1_active && p == 0x2aa0) {    /* 24-bit next-seg address just assembled into PX:AR */
            unsigned addr = ((unsigned)s_cpu.px() << 16) | s_cpu.ar();
            const char *reg = addr<0x80000?"FLASH":addr<0x200000?"GAP":addr<0x400000?"U109":"U110";
            fprintf(stderr, "  >>> assembled 24-bit next-seg addr = %06x  region=%s\n", addr, reg);
            fprintf(stderr, "      saved-return ptr DM$0DDF=%04x DM$0DE0=%04x\n",
                    dcs_data_read(0x0ddf), dcs_data_read(0x0de0));
            fprintf(stderr, "  --- continuation reads AFTER command-1 installs the new stream pointer ---\n");
            /* keep g_cmd1_active so the rom-read log captures the continuation fetches */
        }
    }
    if (g_gendtrace) {
        unsigned p = pc & 0x3fff;
        if (p == 0x39d1 || p == 0x39d0 || p == 0x39cf) {   /* around the ee01 store: dump verify state */
            static long vn = 0;
            if (vn++ < 6)
                fprintf(stderr, "[vfail] pc=%04x region=%04x sum@386D=%04x bounds@3863..67=%04x %04x %04x %04x %04x page@0482=%04x AR=%04x AY0=%04x\n",
                        p, dcs_data_read(0x3844), dcs_data_read(0x386d),
                        dcs_data_read(0x3863), dcs_data_read(0x3864), dcs_data_read(0x3865),
                        dcs_data_read(0x3866), dcs_data_read(0x3867), dcs_data_read(0x0482),
                        s_cpu.ar(), s_cpu.ay0());
        }
        if (p == 0x39bd) {   /* verify-region table load: index + the 10-word entry */
            static long tn = 0;
            if (tn++ < 12) {
                unsigned idx = dcs_data_read(0x3844);
                unsigned base = 0x08c0 + idx * 10;
                fprintf(stderr, "[vtab] idx=%u entry@%04x:", idx, base);
                for (int i = 0; i < 10; i++) fprintf(stderr, " %04x", dcs_data_read(base + i));
                fprintf(stderr, "\n");
            }
        }
        if (p == 0x3b61 || p == 0x3b63 || p == 0x3b65) {   /* memory-verify FAILURE chosen (ee01/02/03) */
            static long eef = 0;
            if (eef++ < 8)
                fprintf(stderr, "[eefail] pc=%04x I4=%04x I1=%04x SR0=%04x AX0=%04x AY0=%04x PX=%02x "
                        "DM[38E1..5]=%04x %04x %04x %04x %04x\n",
                        p, (unsigned)s_cpu.get_i(4), (unsigned)s_cpu.get_i(1),
                        s_cpu.sr0(), s_cpu.ax0(), s_cpu.ay0(), s_cpu.px(),
                        dcs_data_read(0x38e1), dcs_data_read(0x38e2), dcs_data_read(0x38e3),
                        dcs_data_read(0x38e4), dcs_data_read(0x38e5));
        }
        if (p == 0x36b0 && getenv("DCS_36AA")) {   /* stream-reader entry: page/ptr/end bounds */
            static long n=0;
            if (n++<20) fprintf(stderr,"[36aa] #%ld page(0B60)=%04x ptr(0B61)=%04x end(0B63)=%04x acc(0B64)=%04x flag(3810)=%04x\n",
                n, dcs_data_read(0x0b60), dcs_data_read(0x0b61), dcs_data_read(0x0b63), dcs_data_read(0x0b64), dcs_data_read(0x3810));
        }
        if (p == 0x3d8b && getenv("DCS_SRWORD")) {   /* stream reader: actual sound_data WORD + value */
            static long sw = 0;
            if (sw++ < 40) {
                unsigned i5 = s_cpu.get_i(5) & 0x3fff;
                unsigned dm_st = H.sdrc[1] & 3;
                unsigned dm_base = dm_st == 1 ? 0x0000 : dm_st == 2 ? 0x3000 : 0x3400;
                size_t word = ((size_t)(H.sdrc[2] & 0x7ff) * 1024 + (i5 >= dm_base ? i5 - dm_base : i5)) % H.sound_data.size();
                fprintf(stderr, "[srw] #%ld I5=%04x dmpage=%03x -> word=%#zx val=%04x  (region=%s)\n",
                        sw, i5, H.sdrc[2]&0x7ff, word, H.sound_data[word],
                        word < 0x80000 ? "FLASH" : word < 0x200000 ? "GAP(zero?)" : word < 0x400000 ? "U109" : "U110");
            }
        }
        if (p == 0x3d8b && getenv("DCS_SRTRACE")) {   /* stream reader: read compressed word at I5 in page */
            static long sr = 0;
            if (sr++ < 60) {
                unsigned i5 = s_cpu.get_i(5) & 0x3fff;
                fprintf(stderr, "[sr] #%ld I5=%04x page(DM0482)=%04x PM3FEC=%06x PM3FEE=%06x val@I5=%04x\n",
                        sr, i5, dcs_data_read(0x0482), dcs_program_read(0x3fec), dcs_program_read(0x3fee),
                        dcs_data_read(i5));
            }
        }
        if ((p == 0x2fdd || p == 0x2fe2) && getenv("DCS_STRM")) {   /* raw segment-stream byte read */
            static long n=0;
            if (n++<48) {
                unsigned i0 = s_cpu.get_i(0) & 0x3fff;
                unsigned pg = dcs_data_read(0x0482);
                unsigned page_words = (H.sdrc[0] & 0x10) ? 1024 : 4096;
                size_t word = ((size_t)(H.sdrc[2] & 0x1fff) * page_words + i0) % H.sound_data.size();
                fprintf(stderr,"[strm] #%ld DE1=%04x DE2=%04x page(0482)=%04x I0=%04x -> word=%#zx val=%04x\n",
                        n, dcs_data_read(0x0de1), dcs_data_read(0x0de2), pg, i0, word, H.sound_data[word]);
            }
        }
        if (p == 0x011a && getenv("DCS_DQIN")) {   /* dequant INPUT buffer: is real compressed data present? */
            static long n=0;
            if (n++<8) {
                unsigned i1 = s_cpu.get_i(1) & 0x3fff;
                fprintf(stderr,"[dqin] #%ld I1=%04x buf:", n, i1);
                for (int k=0;k<10;k++) fprintf(stderr," %04x", dcs_data_read((i1+k)&0x3fff));
                fprintf(stderr,"\n");
            }
        }
        if (p == 0x011a && getenv("DCS_DQTRACE")) {   /* dequant entry: stream-pointer + block state */
            static long dn = 0;
            if (dn++ < 40)
                fprintf(stderr, "[dq] #%ld I1=%04x I0=%04x M6=%d DM[0DE9]=%04x DM[0DEB]=%04x DM[0DE3grp]=%04x sdrc2=%04x bank=%03x\n",
                        dn, (unsigned)s_cpu.get_i(1), (unsigned)s_cpu.get_i(0), (int)s_cpu.get_m(6),
                        dcs_data_read(0x0de9), dcs_data_read(0x0deb), dcs_data_read(0x0de3),
                        H.sdrc[2], H.rom_bank);
        }
        static bool dq13 = false;
        if (p == 0x011a) { static int dq = 0; if (++dq == 13) dq13 = true; }
        if (p == 0x2a0d && dq13) {   /* Nth pump AFTER the 13th dequant (= play-aligned) */
            static long n = 0;
            if (++n == 50) {
                if (const char *df = getenv("DCS_DUMPDMALL")) {
                    FILE *f = fopen(df, "wb");
                    if (f) {
                        for (unsigned a = 0; a < 0x4000; a++) { uint16_t v = dcs_data_read_impl(a); fwrite(&v, 2, 1, f); }
                        fwrite(H.sram, 2, 0x10000, f);
                        fclose(f);
                        fprintf(stderr, "[dmall] dumped at pump #%ld\n", n);
                    }
                }
            }
        }
        if (getenv("DCS_PTR") && p >= 0x013a && p <= 0x0144) {   /* decoder pointer reconstruction */
            static long n = 0;
            if (n++ < 60) fprintf(stderr, "[ptr] pc=%04x SI=%04x AR=%04x SR1=%04x SR0=%04x SE=%d I4=%04x M4=%d\n",
                p, s_cpu.si(), s_cpu.ar(), s_cpu.sr1(), s_cpu.sr0(), s_cpu.se(),
                (unsigned)s_cpu.get_i(4), (int)s_cpu.get_m(4));
        }
        if (p == 0x3a43) {   /* voicemix active-slot: volume + source at mix time */
            static long n = 0;
            if (n++ < 12)
                fprintf(stderr, "[vmix] vol@3818=%04x idx@381A=%04x src@3816=%04x master@0DE4=%04x slot@I5[-1]\n",
                        dcs_data_read(0x3818), dcs_data_read(0x381a), dcs_data_read(0x3816), dcs_data_read(0x0de4));
        }
        if (p == 0x28fa || p == 0x2946 || p == 0x294c) {   /* command queue dequeue / track index / count */
            static long n = 0;
            if (n++ < 60)
                fprintf(stderr, "[cmdq] pc=%04x AR=%04x  DM[0BE9wr]=%04x DM[0BEArd]=%04x\n",
                        p, s_cpu.ar(), dcs_data_read(0x0be9), dcs_data_read(0x0bea));
        }
        if (p == 0x2959 || p == 0x2962) {   /* play-time track enumeration: stream ptr + read pointer */
            static long n = 0;
            if (n++ < 60)
                fprintf(stderr, "[enum] pc=%04x AR=%04x PX=%02x DM[0DDF]=%04x DM[0DE0]=%04x DM[0DE1]=%04x DM[0DE2]=%04x\n",
                        p, s_cpu.ar(), s_cpu.px(), dcs_data_read(0x0ddf), dcs_data_read(0x0de0),
                        dcs_data_read(0x0de1), dcs_data_read(0x0de2));
        }
        if (p == 0x2a60) {   /* segment-end command + full per-group pointer table 0x0E6A[6*2] */
            static long n = 0;
            if (n++ < 40) {
                fprintf(stderr, "[segcmd] AR(cmd)=%04x grp=%04x  grpptr:",
                        s_cpu.ar(), dcs_data_read(0x0de3));
                for (int g = 0; g < 6; g++)
                    fprintf(stderr, " g%d=%04x:%04x", g, dcs_data_read(0x0e6a + 2*g), dcs_data_read(0x0e6b + 2*g));
                fprintf(stderr, "\n");
            }
        }
        if (p == 0x2d2e || p == 0x2e0e) {
            static long n = 0;
            if (n++ < 4000) {
                uint16_t i6 = (uint16_t)s_cpu.get_i(6);
                fprintf(stderr, "[gend] pc=%04x I6=%04x DM[I6]=%04x DM[0DE9]=%04x grp=%04x blk=%04x "
                        "DM[0DF3]=%04x DM[0DF4]=%04x\n",
                        p, i6, dcs_data_read(i6), dcs_data_read(0x0de9),
                        dcs_data_read(0x0de3), dcs_data_read(0x0e56),
                        dcs_data_read(0x0df3), dcs_data_read(0x0df4));
            }
        }
    }
    if (!g_pct_fp) return;           /* the sequential file-trace is optional */
    if (!g_pct_armed) {
        if ((pc & 0x3fff) != g_pct_trigger) return;
        g_pct_armed = true;
    }
    if (g_pct_fp && g_pct_count < g_pct_max) {
        fprintf(g_pct_fp, "%04x\n", pc & 0x3fff);
        if (++g_pct_count >= g_pct_max) { fclose(g_pct_fp); g_pct_fp = nullptr; g_adsp_pctrace_enabled = false; }
    }
}
void dcs2_pctrace_start(uint32_t trigger, long maxn, const char *path)
{
    g_pct_trigger = trigger & 0x3fff; g_pct_max = maxn; g_pct_count = 0;
    g_pct_armed = false; g_pct_fp = fopen(path, "w"); g_adsp_pctrace_enabled = (g_pct_fp != nullptr);
}
static bool g_abwrite = false;
void dcs2_set_abwrite(int on){ g_abwrite = on; }
void dcs_data_write(offs_t address, u16 value)
{
    /* MILESTONE-4 (DCS_ALLOCTRACE): catch the one-time fill of the allocation buffer DM[0x0C65..0x0C74]
       and log its source (the stream pointer + last ROM word) to see where U109 vs U110 sounds read
       their allocation header from. */
    if (getenv("DCS_ALLOCTRACE") && address >= 0x0c65 && address <= 0x0c74) {
        extern size_t dcs2_dbg_stream_word(void); size_t w = dcs2_dbg_stream_word();
        const char *reg = w<0x80000?"FLASH":w<0x200000?"GAP":w<0x400000?"U109":"U110";
        static long n=0;
        if (n++ < 48)
            fprintf(stderr, "[ALLOC-fill] DM[%04x]=%04x code=%02x %s  pc=%04x  streamptr page=%04x pos=%04x  lastROM=%#08zx %s\n",
                    address, value, value & 0x7f, (value&0x7f)==0x7f?"TERM":"    ",
                    s_cpu.pc()&0x3fff, dcs_data_read(0x0de1), dcs_data_read(0x0de2), w, reg);
    }
    if (g_cmd1trace && address >= 0x0bf4 && address <= 0x0bf9 && value != 0) {
        static long n=0;
        if (n++ < 20) fprintf(stderr, "  *** KILL-FLAG set: DM[%04x]=%04x  pc=%04x (grp=%u) ***\n",
                              address, value, s_cpu.pc()&0x3fff, (unsigned)(address-0x0bf4));
    }
    /* DCS_NOKILL: suppress the dequant-underflow group-kill flag to test whether the U109 stream
       carries more coherent audio past the truncation point (data-present vs spurious-kill test). */
    if (getenv("DCS_NOKILL") && address >= 0x0bf4 && address <= 0x0bf9 && value != 0) return;
    if (g_abwrite && address >= 0x2800 && address <= 0x37ff && value != 0) {
        static long n=0;
        if (n++ < 30) fprintf(stderr,"[AB-bank wr] DM[%04x]=%04x pc=%04x\n", address, value, s_cpu.pc());
    }
    if (address == 0x0400) {                          /* DSP pops a command */
        if (getenv("DCS_CMDTRACE")) { static int w=0; if(w++<30) fprintf(stderr,"[pop cmd] cnt %u->%u pc=%04x\n", H.command_count, H.command_count?H.command_count-1:0, s_cpu.pc()); }
        if (H.command_count) { H.command_head = (H.command_head + 1) & 65535; H.command_count--; g_mbox_pop++; }
        s_cpu.set_irq(ADSP2101_IRQ2, 0);
        if (H.command_count) s_cpu.set_irq(ADSP2101_IRQ2, 1);
        return;
    }
    if (address == 0x0401) {
        if (getenv("DCS_EEWATCH") && (value & 0xff00) == 0xee00) {
            static int n = 0;
            if (n++ < 4) {
                fprintf(stderr, "[eepost] DM[0401] <- %04x  pc=%04x  I4=%04x PM[I4]=%06x AR=%04x AX0=%04x AY0=%04x SR0=%04x\n",
                        value, s_cpu.pc() & 0x3fff, (unsigned)s_cpu.get_i(4),
                        dcs_program_read(s_cpu.get_i(4) & 0x3fff), s_cpu.ar(), s_cpu.ax0(), s_cpu.ay0(), s_cpu.sr0());
            }
        }
        H.output_data = value; H.output_full = true; H.output_writes++;
        if (getenv("DCS_TRACE")) fprintf(stderr, "[dsp->host] %04x (pc=%04x)\n", value, s_cpu.pc());
        return;
    }
    if (address == 0x0402) { H.output_control = value; return; }
    if (address >= 0x0480 && address <= 0x0483) {
        unsigned reg = address - 0x0480;
        if (g_dirtrace && reg == 2) {
            static long n = 0;
            if (n++ < 400) fprintf(stderr, "[sdrc2] page <- %04x  pc=%04x\n", value, s_cpu.pc());
        }
        if (reg == 2 && getenv("DCS_PLAYPG")) {
            unsigned pc = s_cpu.pc() & 0x3fff;
            static unsigned last_pg = 0xffff;
            unsigned pg = value & 0x1fff;
            if (pc < 0x3900 && pc > 0x0080 && pg != last_pg) {   /* exclude boot + checksum; dedupe */
                static long pp = 0;
                if (pp++ < 80) fprintf(stderr, "[playpg] sdrc2 <- %04x (epm_pg=%04x -> word_base=%#zx %s) pc=%04x\n",
                    value, pg, (size_t)pg * 1024,
                    (size_t)pg*1024<0x80000?"FLASH":(size_t)pg*1024<0x200000?"GAP":(size_t)pg*1024<0x400000?"U109":"U110", pc);
                last_pg = pg;
            }
        }
        if (reg == 2 && g_sdrc2watch && value != H.sdrc[2]) {
            g_sdrc2_changes++;
            if (value < g_sdrc2_min) g_sdrc2_min = value;
            if (value > g_sdrc2_max) g_sdrc2_max = value;
        }
        if (reg < 3) { H.sdrc[reg] = value; return; }
        switch ((H.sdrc[0] >> 13) & 7) {              /* SDRC security seed state machine */
        case 1: H.sdrc_seed = value; break;
        case 3: H.sdrc_seed = (H.sdrc_seed << 1) | 1; break;
        case 4: H.sdrc_seed += H.sdrc_seed >> 1; break;
        case 5: H.sdrc_seed ^= (H.sdrc_seed << 1) | 1; break;
        case 6: H.sdrc_seed = (((H.sdrc_seed << 7) ^ (H.sdrc_seed << 5) ^ (H.sdrc_seed << 4) ^
                                (H.sdrc_seed << 3)) & 0x80) | (H.sdrc_seed >> 1); break;
        case 7: H.sdrc_seed = ~H.sdrc_seed; break;
        }
        return;
    }

    unsigned dm_st = H.sdrc[1] & 3;
    unsigned dm_base = dm_st == 1 ? 0x0000 : dm_st == 2 ? 0x3000 : 0x3400;
    if (dm_st && address >= dm_base && address < dm_base + 0x400) {
        size_t word = ((size_t)(H.sdrc[2] & 0x7ff) * 1024 + address - dm_base) % H.sound_data.size();
        if (getenv("DCS_DRAMWR")) { static long n=0; if(n++<30) fprintf(stderr,"[dramwr] DM[%04x]=%04x -> word=%#zx (page=%03x) pc=%04x\n", address, value, word, H.sdrc[2]&0x7ff, s_cpu.pc()&0x3fff); }
        H.sound_data[word] = value;
        return;
    }

    if (g_wtrace && (address == 0x387d || address == 0x0b5d)) {
        static int n = 0;
        if (n++ < 40) fprintf(stderr, "[wtrace] DM[%04x] <- %04x  pc=%04x\n", address, value, s_cpu.pc());
    }
    if (getenv("DCS_VTABWATCH") && address >= 0x08c0 && address <= 0x08d3) {
        static int n = 0;
        if (n++ < 20) fprintf(stderr, "[vtabw] DM[%04x] <- %04x  pc=%04x\n",
                              address, value, s_cpu.pc() & 0x3fff);
    }
    if (g_dirtrace && s_cpu.pc() != 0x3d00) {
        /* sound-header / stream descriptor writes (0x0DD3..0x0DE0) = where the sound's data lives.
           skip the idle-loop clears at 0x3091/0x3093 that just re-zero 0x0DDF/0x0DE0 */
        if (address >= 0x0dd3 && address <= 0x0de2 && s_cpu.pc() != 0x3091 && s_cpu.pc() != 0x3093 && s_cpu.pc() != 0x3ada) {
            static long n = 0;
            if (n++ < 40)
                fprintf(stderr, "[hdr] DM[%04x] <- %04x  pc=%04x\n", address, value, s_cpu.pc());
        }
        /* per-group STREAM POINTER table 0x0E6A..0x0E76 (2 words/group): 0x3094 skips a group whose
           pointer is 0. The LP1->LP2 segment chaining updates this; a 0 means "segment ended". */
        if (address >= 0x0e6a && address <= 0x0e76) {
            static long n = 0;
            if (n++ < 60)
                fprintf(stderr, "[grpptr] DM[%04x] <- %04x  pc=%04x  grp=%04x\n",
                        address, value, s_cpu.pc(), dcs_data_read(0x0de3));
        }
        /* control-directory writes (0x0C25..0x0C55): what the render loop walks; low7==0x7F = end marker */
        if (address >= 0x0c25 && address <= 0x0c55) {
            static long n = 0;
            if (n++ < 48)
                fprintf(stderr, "[cdir] DM[%04x] <- %04x  (low7=%02x%s)  pc=%04x\n",
                        address, value, value & 0x7f, (value & 0x7f) == 0x7f ? " END" : "", s_cpu.pc());
        }
        /* directory BODY writes (0x0df6..0x0e56) that are non-zero = real block descriptors */
        if (address >= 0x0df6 && address <= 0x0e56 && value != 0) {
            static long n = 0;
            if (n++ < 200)
                fprintf(stderr, "[dir-fill] DM[%04x] <- %04x  pc=%04x  (grp=%04x blk=%04x)\n",
                        address, value, s_cpu.pc(),
                        dcs_data_read(0x0de3), dcs_data_read(0x0e56));
        }
        /* block-index advance (0x0e56) and any write to the stream/read pointers */
        if (address == 0x0e56) {
            static long n = 0;
            if (n++ < 120)
                fprintf(stderr, "[dir-blk] blk <- %04x  pc=%04x  grp=%04x\n",
                        value, s_cpu.pc(), dcs_data_read(0x0de3));
        }
    }
    if (address >= 0x3400 && address < 0x3400 + 960) {   /* SPORT autobuffer output region */
        g_ab_writes++;
        if (value) { g_ab_nzwrites++; if ((int16_t)value > g_ab_wpeak) g_ab_wpeak = (int16_t)value;
                     H.snap[address - 0x3400] = (int16_t)value;   /* tap the fill: last non-zero per pos */
                     g_ab_wpc[s_cpu.pc() & 0x3fff]++; }           /* which PC writes non-zero PCM */
    }
    bool sm_enabled = (H.sdrc[0] & 0x0800) != 0;
    bool sm_bank = (H.sdrc[0] & 0x1000) != 0;
    if (sm_enabled) {
        if (!sm_bank && address >= 0x0800 && address <= 0x17ff) { g_sram_acc[1]++; H.sram[address - 0x0800] = value; return; }
        if (address >= 0x1800 && address <= 0x27ff) {
            g_sram_acc[1]++; H.sram[(sm_bank ? 0x3000 : 0x1000) + address - 0x1800] = value; return; }
        if (address >= 0x2800 && address <= 0x37ff) { g_sram_acc[1]++; H.sram[0x2000 + address - 0x2800] = value; return; }
    }
    if (address >= 0x3800 && address <= 0x39ff) { H.data[address] = value; return; }
    if (address == 0x3000) { H.rom_bank = value & 0x7ff; return; }
    if (address >= 0x3fe0) {
        unsigned reg = address - 0x3fe0;
        H.control[reg] = value;
        if ((reg == SYSCONTROL_REG && !(value & 0x0800)) ||
            (reg == S1_AUTOBUF_REG && !(value & 0x0002)))
            H.sport_enabled = false;
    }
}

/* program memory: internal PM below 0x0800, overlaid SRAM above (24-bit words packed in SRAM) */
u32 dcs_program_read(offs_t address)
{
    if (address >= 0x0800) {
        size_t off = 0x4800 + (address - 0x0800) * 2; g_sram_acc[2]++;
        return (H.sram[off] | ((uint32_t)H.sram[off + 1] << 16)) & 0xffffff;
    }
    return H.program[address & 0x3fff];               /* internal PM, filled by the flash boot */
}
void dcs_program_write(offs_t address, u32 value)
{
    if (getenv("DCS_EEWATCH") && (value & 0xff00) == 0xee00) {
        static int n = 0;
        if (n++ < 8) fprintf(stderr, "[eew] PM[%04x] <- %06x  pc=%04x\n",
                             address & 0x3fff, value & 0xffffff, s_cpu.pc() & 0x3fff);
    }
    if (getenv("DCS_EEWATCH") && (address & 0x3fff) == 0x3ffc) {
        static int n = 0;
        if (n++ < 12) fprintf(stderr, "[3ffc] PM[3FFC] <- %06x  pc=%04x AR=%04x AX0=%04x AY0=%04x SR0=%04x SR1=%04x\n",
                             value & 0xffffff, s_cpu.pc() & 0x3fff,
                             s_cpu.ar(), s_cpu.ax0(), s_cpu.ay0(), s_cpu.sr0(), s_cpu.sr1());
    }
    if (address >= 0x0800) {
        size_t off = 0x4800 + (address - 0x0800) * 2; g_sram_acc[3]++;
        H.sram[off] = value; H.sram[off + 1] = value >> 16;
        return;
    }
    H.program[address & 0x3fff] = value & 0xffffff;
}

u16  dcs_io_read(offs_t)          { return 0; }
void dcs_io_write(offs_t, u16)    {}
u32  dcs_sport_rx(int)            { return 0; }
void dcs_timer_fired(int en)      {
    H.timer_enabled = en;
    if (getenv("DCS_TMRTRACE")) { static int n=0; if(n++<10)
        fprintf(stderr,"[timer] MSTAT-timer %s: TSCALE=%04x TCOUNT=%04x TPERIOD=%04x pc=%04x\n",
            en?"ENABLE":"disable", H.control[TIMER_SCALE_REG], H.control[TIMER_COUNT_REG],
            H.control[TIMER_PERIOD_REG], s_cpu.pc()&0x3fff); }
}

/* Execute `cycles` on the DSP while servicing the ADSP-2105 programmable timer: decrement TCOUNT
   every (TSCALE+1) cycles and raise the TIMER interrupt on underflow (reloading TPERIOD), so the
   engine's timer ISR runs on schedule. Disabled by DCS_NOTIMER (to A/B against the old behaviour). */
static void execute_timed(int cycles)
{
    static int notimer = -1; if (notimer < 0) notimer = getenv("DCS_NOTIMER") ? 1 : 0;
    if (notimer || !H.timer_enabled) { s_cpu.execute(cycles); return; }
    int scale = (H.control[TIMER_SCALE_REG] & 0xff) + 1;   /* cycles per TCOUNT decrement */
    while (cycles > 0) {
        int tcount = H.control[TIMER_COUNT_REG] & 0xffff;
        /* cycles until the next TCOUNT decrement, then until underflow */
        int to_dec = scale - H.timer_subcycle;
        int budget = to_dec + tcount * scale;             /* cycles until underflow */
        int run = cycles < budget ? cycles : budget;
        s_cpu.execute(run);
        cycles -= run;
        /* advance the timer model by `run` cycles */
        int adv = H.timer_subcycle + run;
        int decs = adv / scale;
        H.timer_subcycle = adv % scale;
        if (decs) {
            if (decs <= tcount) {
                H.control[TIMER_COUNT_REG] = (uint16_t)(tcount - decs);
            } else {
                /* underflow: reload TPERIOD, raise TIMER irq (edge-latched) */
                H.control[TIMER_COUNT_REG] = H.control[TIMER_PERIOD_REG];
                s_cpu.set_irq(ADSP2101_TIMER, 1);
                s_cpu.set_irq(ADSP2101_TIMER, 0);
            }
        }
    }
}
void dcs_dmovlay(u32)             {}

/* SPORT1 autobuffer transmit — set up the PCM DMA parameters (MAME DCS SPORT callback). */
void dcs_sport_tx(int port, u32 /*data*/)
{
    if (port != 1 || !(H.control[SYSCONTROL_REG] & 0x0800) || !(H.control[S1_AUTOBUF_REG] & 0x0002))
        return;
    H.ireg = (H.control[S1_AUTOBUF_REG] >> 9) & 7;
    int mreg = (H.control[S1_AUTOBUF_REG] >> 7) & 3;
    mreg |= H.ireg & 4;
    /* the M (modify) registers are 14-bit signed — MAME reads the increment via state_int, which
       applies signed_mask(0x3fff). Reading m_m raw would keep stale upper bits (0x8001 -> -32767
       instead of the real +1), which zeroes the sample count. */
    H.increment = (int)util::sext((uint32_t)s_cpu.get_m(mreg) & 0x3fff, 14);
    H.length = s_cpu.get_l(H.ireg);
    int source = s_cpu.get_i(H.ireg);
    source &= ~0xf;
    s_cpu.set_i(H.ireg, source);
    H.base = source & 0x3fff;
    H.play_pos = H.base;
    H.next_irq_pos = H.length / 2;
    H.source_rate = 8000000.0 / (2.0 * (H.control[S1_SCLKDIV_REG] + 1) * 16.0);
    H.sport_enabled = H.length > 0 && H.increment != 0;
    if (getenv("DCS_SPTRACE"))
        fprintf(stderr, "[sport_tx] AUTOBUF=%04x SYS=%04x ireg=%d mreg=%d M=%d L=%d I=%04x base=%04x rate=%.1f en=%d\n",
                H.control[S1_AUTOBUF_REG], H.control[SYSCONTROL_REG], H.ireg, mreg,
                H.increment, H.length, source & 0x3fff, H.base, H.source_rate, H.sport_enabled);
}

/* ============================================================================================
 *  Boot + mailbox + render
 * ========================================================================================== */
bool dcs2_prepare(const uint8_t *u109, size_t u109_len,
                  const uint8_t *u110, size_t u110_len,
                  const uint8_t *flash, size_t flash_len)
{
    if (getenv("DCS_LOGERR")) g_adsp_logerr = 1;
    if (getenv("DCS_CMD1")) { g_cmd1trace = true; g_adsp_pctrace_enabled = true; }
    if (u109_len != P2K_DCS_BANK_SIZE / 2 || u110_len != P2K_DCS_BANK_SIZE / 2 ||
        flash_len != P2K_DCS_SOUND_FLASH_SIZE)
        return false;

    H.flash.assign(flash, flash + flash_len);
    if (const char *ss = getenv("DCS_SAMPLE_SHIFT")) g_sample_shift = strtol(ss, nullptr, 0);

    /* raw interleaved u109/u110 for the rom_bank window: [u109 word][u110 word] per 4 bytes */
    H.sound_rom.assign(P2K_DCS_BANK_SIZE, 0);
    size_t chip_words = P2K_DCS_BANK_SIZE / 4;
    for (size_t i = 0; i < chip_words; i++) {
        H.sound_rom[i * 4 + 0] = u109[i * 2 + 0];
        H.sound_rom[i * 4 + 1] = u109[i * 2 + 1];
        H.sound_rom[i * 4 + 2] = u110[i * 2 + 0];
        H.sound_rom[i * 4 + 3] = u110[i * 2 + 1];
    }

    /* SDRC word view: flash at word 0, u109 at 0x200000, u110 at 0x400000 (real address gaps) */
    H.sound_data.assign(P2K_DCS_REGION_WORDS, 0);
    size_t flash_words = P2K_DCS_SOUND_FLASH_SIZE / 2;
    for (size_t i = 0; i < flash_words; i++)
        H.sound_data[i] = flash[i * 2] | (flash[i * 2 + 1] << 8);
    int u109fmt = getenv("DCS_U109FMT") ? atoi(getenv("DCS_U109FMT")) : 0;
    size_t chip_bytes = P2K_DCS_BANK_SIZE / 2;   /* 4 MiB per chip */
    switch (u109fmt) {
    default:
    case 0:  /* current: u109 block @0x200000 (LE words), u110 block @0x400000 (LE words) */
        for (size_t i = 0; i < chip_words; i++) {
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + i] = u109[i * 2] | (u109[i * 2 + 1] << 8);
            H.sound_data[P2K_DCS_U110_WORD_OFFSET + i] = u110[i * 2] | (u110[i * 2 + 1] << 8);
        }
        break;
    case 1:  /* byte-interleave across [0x200000,0x600000): word = u109[i] | u110[i]<<8 */
        for (size_t i = 0; i < chip_bytes; i++)
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + i] = u109[i] | (u110[i] << 8);
        break;
    case 2:  /* byte-interleave swapped: word = u110[i] | u109[i]<<8 */
        for (size_t i = 0; i < chip_bytes; i++)
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + i] = u110[i] | (u109[i] << 8);
        break;
    case 3:  /* word-interleave: even=u109 word, odd=u110 word */
        for (size_t k = 0; k < chip_words; k++) {
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + 2 * k]     = u109[k * 2] | (u109[k * 2 + 1] << 8);
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + 2 * k + 1] = u110[k * 2] | (u110[k * 2 + 1] << 8);
        }
        break;
    case 4:  /* word-interleave swapped */
        for (size_t k = 0; k < chip_words; k++) {
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + 2 * k]     = u110[k * 2] | (u110[k * 2 + 1] << 8);
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + 2 * k + 1] = u109[k * 2] | (u109[k * 2 + 1] << 8);
        }
        break;
    case 5:  /* u109 block big-endian words, u110 block BE */
        for (size_t i = 0; i < chip_words; i++) {
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + i] = u109[i * 2 + 1] | (u109[i * 2] << 8);
            H.sound_data[P2K_DCS_U110_WORD_OFFSET + i] = u110[i * 2 + 1] | (u110[i * 2] << 8);
        }
        break;
    case 6:  /* CONTIGUOUS: u109 right after flash (word 0x80000), u110 after u109 (0x280000) */
        for (size_t i = 0; i < chip_words; i++) {
            H.sound_data[0x80000 + i]  = u109[i * 2] | (u109[i * 2 + 1] << 8);
            H.sound_data[0x280000 + i] = u110[i * 2] | (u110[i * 2 + 1] << 8);
        }
        break;
    case 7:  /* contiguous, u110 first then u109 */
        for (size_t i = 0; i < chip_words; i++) {
            H.sound_data[0x80000 + i]  = u110[i * 2] | (u110[i * 2 + 1] << 8);
            H.sound_data[0x280000 + i] = u109[i * 2] | (u109[i * 2 + 1] << 8);
        }
        break;
    case 8:  /* SWAP chips: u110 @0x200000, u109 @0x400000 (LE) */
        for (size_t i = 0; i < chip_words; i++) {
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + i] = u110[i * 2] | (u110[i * 2 + 1] << 8);
            H.sound_data[P2K_DCS_U110_WORD_OFFSET + i] = u109[i * 2] | (u109[i * 2 + 1] << 8);
        }
        break;
    case 9:  /* u109 LE @0x200000, u110 BE @0x400000 */
        for (size_t i = 0; i < chip_words; i++) {
            H.sound_data[P2K_DCS_U109_WORD_OFFSET + i] = u109[i * 2] | (u109[i * 2 + 1] << 8);
            H.sound_data[P2K_DCS_U110_WORD_OFFSET + i] = u110[i * 2 + 1] | (u110[i * 2] << 8);
        }
        break;
    }
    /* The address range between the flash (ends 0x080000) and U109 (starts 0x200000) is modelled as a
       zero gap — but RFM's DSP ROM-checksum (001b) FAILS against that (ee01) while SWE1 passes, i.e.
       the real SDRC presents SOMETHING there (partial decode mirroring). DCS_GAPMIRROR selects a
       hypothesis: 1 = flash mirrored across the gap, 2 = u109 mirrored, 3 = u110 mirrored. */
    if (const char *gm = getenv("DCS_GAPMIRROR")) {
        int mode = atoi(gm);
        for (size_t w = flash_words; w < P2K_DCS_U109_WORD_OFFSET; w++) {
            switch (mode) {
            case 1: H.sound_data[w] = H.sound_data[w % flash_words]; break;
            case 2: H.sound_data[w] = H.sound_data[P2K_DCS_U109_WORD_OFFSET + (w % chip_words)]; break;
            case 3: H.sound_data[w] = H.sound_data[P2K_DCS_U110_WORD_OFFSET + (w % chip_words)]; break;
            }
        }
    }

    memset(H.data, 0, sizeof H.data);
    memset(H.program, 0, sizeof H.program);
    memset(H.control, 0, sizeof H.control);
    memset(H.sdrc, 0, sizeof H.sdrc);
    memset(H.sram, 0, sizeof H.sram);
    H.rom_bank = 0; H.command_head = 0; H.command_count = 0;
    H.output_data = 0; H.output_control = 0; H.output_full = false;
    H.sport_enabled = false; H.source_phase = 0; H.cycle_phase = 0;
    H.last_sample[0] = H.last_sample[1] = 0;

    /* ADSP boot page: the low byte of each of the first 0x1000 flash words. Kept for the host-boot
       re-upload (dcs2_reboot). */
    for (size_t i = 0; i < sizeof H.boot_page; i++) H.boot_page[i] = flash[i * 2];
    static bool started = false;
    if (!started) { s_cpu.start(); started = true; }      // bind spaces once
    s_cpu.load_boot_data(H.boot_page, H.program);
    s_cpu.reset();
    H.initialized = true;
    return true;
}

void dcs2_write_cmd(uint16_t command)
{
    if (!H.initialized) return;

    if (command == 0xace1) {              /* script/stream-mode entry: SDRC acks 0x0100 then 0x000C */
        H.host_ack[0] = 0x0100; H.host_ack[1] = 0x000c;
        H.host_ack_head = 0; H.host_ack_count = 2;
    }
    unsigned queued_after = H.command_count;
    if (H.command_count < 65536) {
        unsigned tail = (H.command_head + H.command_count) & 65535;
        H.commands[tail] = command;
        H.command_count++;
        g_mbox_enq++;
        queued_after = H.command_count;
    }
    s_cpu.set_irq(ADSP2101_IRQ2, 1);

    bool diag = command == 0x003a || command == 0x001b || command == 0x00aa;
    if (!diag) {
        for (unsigned c = 0; c < 20000; c += 100) {
            if (H.command_count < queued_after) break;
            s_cpu.execute(100);
        }
    } else {
        unsigned limit = command == 0x001b ? 250000000u : 20000000u;
        for (unsigned c = 0; c < limit; c += 1000) {
            s_cpu.execute(1000);
            if (H.output_full) break;
        }
    }
}

uint8_t dcs2_flag_byte(void)
{
    return (H.command_count == 0 ? 0x40 : 0) | ((H.output_full || H.host_ack_count) ? 0x80 : 0);
}

uint16_t dcs2_read_response(void)
{
    if (H.host_ack_count) { H.host_ack_count--; return H.host_ack[H.host_ack_head++]; }
    uint16_t value = H.output_full ? H.output_data : 0;
    H.output_full = false;
    if (value) {
        for (unsigned c = 0; c < 100000; c += 100) {
            s_cpu.execute(100);
            if (H.output_full) break;
        }
    }
    return value;
}

void dcs2_render(int16_t *samples, int frames, int output_rate)
{
    if (!H.initialized || output_rate <= 0) {
        memset(samples, 0, frames * 2 * sizeof *samples);
        return;
    }
    static int pchist_on = -1;
    if (pchist_on < 0) pchist_on = getenv("DCS_PCHIST") ? 1 : 0;
    if (g_mboxfix < 0) g_mboxfix = getenv("DCS_MBOXFIX") ? 1 : 0;
    for (int n = 0; n < frames; n++) {
        if (H.command_count) {                               /* a host command is still pending */
            g_mbox_render_pending_frames++;
            if (g_mboxfix) { s_cpu.set_irq(ADSP2101_IRQ2, 1); g_mbox_reassert++; }  /* the Encore fix */
        }
        H.cycle_phase += 10000000.0 / output_rate;          /* DSP core clock ~10 MHz */
        int cycles = (int)H.cycle_phase;
        H.cycle_phase -= cycles;
        execute_timed(cycles);
        if (pchist_on) g_pchist[s_cpu.pc() & 0x3fff]++;

        if (H.sport_enabled) {
            H.source_phase += H.source_rate;
            /* Harness-side double buffer. The DSP over-produces and re-clears the whole autobuffer
               (0x36AA) between the SPORT's reads, so reading the LIVE buffer at play_pos catches
               freshly-cleared (zero) positions. Instead snapshot each half from the live buffer the
               moment the SPORT crosses INTO it (the DSP has just finished filling that half while the
               SPORT drained the other one) and read the output from the stable snapshot. */
            /* Output tap. Default = plain LIVE read of the SPORT autobuffer — verified SAMPLE-EXACT vs
               Encore (SWE1 whole-track corr 1.0000). It faithfully reflects what the DSP produced,
               including RFM's early decode stall (dequant stops after ~12 blocks). DCS_SNAP = write-tap
               snapshot (last non-zero PCM/pos) which gap-fills but MASKS such stalls. */
            static int nosnap = -1; if (nosnap < 0) nosnap = getenv("DCS_SNAP") ? 0 : 1;
            int hlen = H.length / 2;
            while (H.source_phase >= output_rate) {
                H.source_phase -= output_rate;
                int relnow = H.play_pos - H.base;
                for (int ch = 0; ch < 2; ch++) {
                    int rel = (H.play_pos - H.base);
                    if (nosnap) H.last_sample[ch] = (int16_t)dcs_data_read((H.play_pos) & 0x3fff);
                    else        H.last_sample[ch] = (rel >= 0 && rel < H.length) ? H.snap[rel] : 0;
                    H.play_pos += H.increment;
                }
                if (getenv("DCS_ISYNC")) s_cpu.set_i(H.ireg, H.play_pos);
                int relative = H.play_pos - H.base;
                bool half = relative >= H.next_irq_pos;
                bool wrapped = relative >= H.length || relative < 0;
                if (wrapped) { H.play_pos = H.base; relative = 0; H.next_irq_pos = H.length / 2; }
                else if (half) H.next_irq_pos = H.length;
                if (half || wrapped) {                       /* SPORT autobuffer refill IRQ */
                    (void)relnow; (void)hlen;
                    s_cpu.set_i(H.ireg, H.play_pos);
                    uint32_t pc_before = s_cpu.pc();
                    s_cpu.set_irq(ADSP2101_IRQ1, 1);
                    s_cpu.execute(4);
                    if (getenv("DCS_IRQTRACE")) { static int k=0; if(k++<6) fprintf(stderr,"[irq1] pc %04x -> %04x imask=%04x\n", pc_before, s_cpu.pc(), s_cpu.imask()); }
                    s_cpu.execute(96);
                    s_cpu.set_irq(ADSP2101_IRQ1, 0);
                }
            }
        }
        for (int ch = 0; ch < 2; ch++)
            samples[n * 2 + ch] = H.sport_enabled ? H.last_sample[ch] : 0;
    }
}

/* ---- bring-up introspection (temporary) ---- */
uint32_t dcs2_dbg_pc(void)        { return s_cpu.pc(); }
int      dcs2_dbg_run(int cyc)    { return s_cpu.execute(cyc); }
unsigned dcs2_dbg_out_writes(void){ return H.output_writes; }
int      dcs2_dbg_sport(void)     { return H.sport_enabled; }
unsigned dcs2_dbg_cmd_count(void) { return H.command_count; }
uint32_t dcs2_dbg_pm(uint16_t a) { return dcs_program_read(a); }
void dcs2_dbg_load_pm(const uint32_t *pm, unsigned lo, unsigned hi) {   /* overwrite PM [lo,hi) */
    for (unsigned a = lo; a < hi; a++) dcs_program_write(a, pm[a] & 0xffffff);
}
/* Run the CURRENT boot program (the flash-resident boot loader, loaded by prepare) until it posts
   its boot-complete 000a. On the real machine this is boot pass 1: it pages engine PM 0x2800-0x3fff
   from the flash. */
void dcs2_dbg_runboot(void) {
    for (unsigned c = 0; c < 2000000 && !H.output_full; c += 100) s_cpu.execute(100);
    H.output_full = false;   /* consume the boot-complete response */
}

/* Boot pass 2 — the x86 host boot (pci_dcs_host_boot): overwrite low PM with the x86-uploaded boot
   loader, reset, and run it. This loader pages the REST of the runtime engine (PM 0x0173-0x01FE
   table + 0x0800-0x27FF) that the flash boot loader does not. `loader` holds n low-PM words. */
void dcs2_dbg_hostboot(const uint32_t *loader, unsigned n) {
    for (unsigned a = 0; a < n; a++) H.program[a] = loader[a] & 0xffffff;
    H.command_head = 0; H.command_count = 0; H.output_full = false;
    s_cpu.reset();
    for (unsigned c = 0; c < 2000000 && !H.output_full; c += 100) s_cpu.execute(100);
    H.output_full = false;   /* consume the boot-complete response */
}
void dcs2_dbg_send(uint16_t cmd) {
    if (cmd == 0xace1) { H.host_ack[0] = 0x0100; H.host_ack[1] = 0x000c; H.host_ack_head = 0; H.host_ack_count = 2; }
    unsigned tail = (H.command_head + H.command_count) & 65535;
    H.commands[tail] = cmd; H.command_count++; g_mbox_enq++;
    s_cpu.set_irq(ADSP2101_IRQ2, 1);
}

void dcs2_dbg_mbox_health(long *enq, long *pop, long *pend, long *reassert, unsigned *qc) {
    if (enq) *enq = g_mbox_enq;
    if (pop) *pop = g_mbox_pop;
    if (pend) *pend = g_mbox_render_pending_frames;
    if (reassert) *reassert = g_mbox_reassert;
    if (qc) *qc = H.command_count;
}
int dcs2_dbg_poll(int max_cyc) {
    if (H.host_ack_count) { H.host_ack_count--; return H.host_ack[H.host_ack_head++]; }
    for (int c = 0; c < max_cyc; c += 100) {
        if (H.output_full) { int v = H.output_data; H.output_full = false; return v; }
        s_cpu.execute(100);
    }
    return -1;
}
uint32_t dcs2_dbg_step(void) { s_cpu.execute(1); return s_cpu.pc(); }
void dcs2_dbg_regs(int *se,unsigned *si,unsigned *sr0,unsigned *sr1,unsigned *ar,unsigned *ax0,unsigned *ay0,unsigned *px){
  *se=s_cpu.se();*si=s_cpu.si();*sr0=s_cpu.sr0();*sr1=s_cpu.sr1();*ar=s_cpu.ar();*ax0=s_cpu.ax0();*ay0=s_cpu.ay0();*px=s_cpu.px();
}
/* the host's second boot (pci_dcs_host_boot_dsp_1ms): pci_reset + re-run the resident boot loader
   (already in program[]) — re-pages the engine into program SRAM. */
void dcs2_reboot(void) {
    /* Replicate the x86 host boot (pci_dcs_host_boot_dsp_1ms), not a plain device_reset. The real
       flow: let the self-boot reach its idle steady state, then the host RE-UPLOADS the clean
       120-word boot loader into program[0..], which RESTORES program < 0x0800 (incl. the interrupt
       vectors the running engine had remapped, e.g. the SPORT1-TX vector), then resets. A plain
       device_reset would re-run the MODIFIED program and leave stale vectors. */
    for (unsigned c = 0; c < 2000000; c += 1000) {
        uint32_t pc = s_cpu.pc() & 0x3fff;
        if (dcs_program_read(0x3980) != 0 && dcs_program_read(0x3deb) != 0 &&
            pc >= 0x3d9c && pc <= 0x3dc1)
            break;
        s_cpu.execute(1000);
    }
    s_cpu.load_boot_data(H.boot_page, H.program);   /* fresh boot loader -> clean program[0..0x77] */
    H.command_head = 0; H.command_count = 0; H.output_full = false;
    H.sport_enabled = false;
    s_cpu.reset();
    /* run until the DSP posts its ready ack (output latch), like the host-boot settle loop */
    for (unsigned c = 0; c < 2000000 && !H.output_full; c += 100) s_cpu.execute(100);
}
void dcs2_dbg_sport_state(int *base,int *plen,int *inc,double *rate,int *pp) {
    *base=H.base; *plen=H.length; *inc=H.increment; *rate=H.source_rate; *pp=H.play_pos;
}
uint16_t dcs2_dbg_dm(uint16_t a) { return dcs_data_read(a); }
void dcs2_dbg_dag(int *iv,int *mv,int *lv) {
    for (int n=0;n<8;n++){ iv[n]=s_cpu.get_i(n); mv[n]=s_cpu.get_m(n); lv[n]=s_cpu.get_l(n); }
}
uint16_t dcs2_dbg_autobuf(void){ return H.control[15]; }  /* S1_AUTOBUF_REG */
long *dcs2_wr_hist(void){ return g_wrhist; }
long *dcs2_pc_hist(void){ return g_pchist; }
uint32_t dcs2_dbg_imask(void){ return s_cpu.imask(); }
uint32_t dcs2_dbg_icntl(void){ return s_cpu.icntl(); }
uint16_t dcs2_dbg_sr1(void){ return s_cpu.sr1(); }
uint16_t dcs2_dbg_sr0(void){ return s_cpu.sr0(); }
uint16_t dcs2_dbg_snd(size_t word){ return word < H.sound_data.size() ? H.sound_data[word] : 0; }
uint16_t dcs2_dbg_ctl(int reg){ return H.control[reg & 31]; }
void dcs2_dbg_setdm(uint16_t a, uint16_t v){ dcs_data_write(a, v); }


/* disassemble `count` PM words starting at `start`, to stderr (bring-up aid) */
#include "2100dasm.h"
#include <sstream>
void dcs2_dbg_disasm(uint16_t start, int count) {
    static adsp21xx_disassembler dasm;
    static uint32_t pm[0x4000];
    for (int a = 0; a < 0x4000; a++) pm[a] = dcs_program_read(a);   /* PM incl. SRAM-overlaid >=0x0800 */
    data_buffer buf{ pm, 0x4000 };
    for (int i = 0; i < count; ) {
        uint16_t pc = (start + i) & 0x3fff;
        std::ostringstream os;
        offs_t n = dasm.disassemble(os, pc, buf, buf) & 0xffff;
        fprintf(stderr, "  %04x: %06x  %s\n", pc, pm[pc], os.str().c_str());
        i += n ? n : 1;
    }
}


/* ============================================================================================
 *  ADSP-2105 opcode differential self-test  (dcs2_opfuzz)
 *  --------------------------------------------------------------------------------------------
 *  The user's key insight: comparing our decode to Encore proves nothing about CORRECTNESS,
 *  because both derive from MAME's adsp2100.cpp — a shared core bug matches AND is wrong.
 *  The ONLY independent oracle is the ADSP-2100/2105 datasheet.  This routine implements a
 *  from-scratch, datasheet-derived reference model of every opcode class the DCS segment-chain
 *  decode executes (ALU add/sub + flags, the barrel shifter LSHIFT/ASHIFT LO ±n and OR-mode,
 *  MAC UU fractional/integer) and fuzzes random operands through BOTH our core (single-stepped
 *  through the real execute path) and the reference, reporting any divergence.  Written without
 *  reference to the MAME source, so it can catch a bug the two MAME-derived decoders share.
 *  Run: DCS_OPFUZZ=1 ./dcs2_extract <repo>
 * ========================================================================================== */
int dcs2_opfuzz(void)
{
    s_cpu.reset();
    const uint16_t PC = 0x0010;
    /* ASTAT layout (from the core's own dump): Z=b0 N=b1 V=b2 C=b3 S=b4 Q=b5 M=b6 X=b7 */
    auto step = [&](uint32_t op){ H.program[PC] = op & 0xffffff; s_cpu.t_pc(PC); s_cpu.execute(1); };

    unsigned long seed = 0x2105c0deUL;
    auto rnd = [&](){ seed = seed*6364136223846793005ULL + 1442695040888963407ULL; return (uint16_t)(seed>>33); };

    long total = 0, fails = 0;
    auto report = [&](const char* name, bool ok, const char* fmt, ...){
        total++;
        if (!ok) { fails++; va_list ap; va_start(ap,fmt);
            fprintf(stderr, "[FAIL %s] ", name); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); }
    };

    /* ---- datasheet reference: 16-bit ALU add/sub, returns {result, ZNVC in bits0..3} ---- */
    auto ref_add = [](uint16_t a, uint16_t b, uint16_t &r, uint16_t &fl){
        uint32_t full = (uint32_t)a + (uint32_t)b; r = full & 0xffff;
        uint16_t az = (r==0), an = (r>>15)&1, ac = (full>>16)&1;
        uint16_t av = ((( (uint16_t)(a^r) & (uint16_t)(b^r) )>>15)&1);      /* same-sign in, diff-sign out */
        fl = (az) | (an<<1) | (av<<2) | (ac<<3);
    };
    auto ref_sub = [](uint16_t a, uint16_t b, uint16_t &r, uint16_t &fl){   /* a - b  == a + ~b + 1 */
        uint32_t full = (uint32_t)a + (uint32_t)(uint16_t)~b + 1u; r = full & 0xffff;
        uint16_t az = (r==0), an = (r>>15)&1, ac = (full>>16)&1;            /* AC=1 => no borrow */
        uint16_t av = ((( (uint16_t)(a^b) & (uint16_t)(a^r) )>>15)&1);
        fl = (az) | (an<<1) | (av<<2) | (ac<<3);
    };
    /* barrel shifter, LO reference (input occupies bits0..15 of the 32-bit field) */
    auto ref_lshift = [](uint16_t in, int n)->uint32_t{ uint64_t v=(uint16_t)in;
        uint64_t r = (n>=0) ? (v<<n) : (v>>(-n)); return (uint32_t)r; };
    auto ref_ashift = [](uint16_t in, int n)->uint32_t{ int64_t v=(int16_t)in;         /* sign-extend */
        int64_t r = (n>=0) ? (v<<n) : (v>>(-n)); return (uint32_t)r; };

    for (int iter=0; iter<200000; iter++) {
        uint16_t a = rnd(), b = rnd();

        /* 1) AR = AR + AY0  (0x22620f) */
        { uint16_t r,fl; ref_add(a,b,r,fl); s_cpu.s_ar(a); s_cpu.s_ay0(b); s_cpu.t_astat(0); step(0x22620f);
          report("ADD", s_cpu.g_ar()==r && (s_cpu.g_astat()&0xf)==fl,
                 "%04x+%04x: core ar=%04x fl=%x  ref ar=%04x fl=%x", a,b,s_cpu.g_ar(),s_cpu.g_astat()&0xf,r,fl); }

        /* 1b) AR = AY0 + 1  (0x22200f) — the exact 0xFFFF+1 continue-sentinel test */
        { uint16_t r,fl; ref_add(a,1,r,fl); s_cpu.s_ay0(a); s_cpu.t_astat(0); step(0x22200f);
          report("ADD1", s_cpu.g_ar()==r && (s_cpu.g_astat()&0xf)==fl,
                 "%04x+1: core ar=%04x fl=%x  ref ar=%04x fl=%x", a,s_cpu.g_ar(),s_cpu.g_astat()&0xf,r,fl); }

        /* 2) AF = AR - AY0  (0x26e20f) */
        { uint16_t r,fl; ref_sub(a,b,r,fl); s_cpu.s_ar(a); s_cpu.s_ay0(b); s_cpu.t_astat(0); step(0x26e20f);
          report("SUB", s_cpu.g_af()==r && (s_cpu.g_astat()&0xf)==fl,
                 "%04x-%04x: core af=%04x fl=%x  ref af=%04x fl=%x", a,b,s_cpu.g_af(),s_cpu.g_astat()&0xf,r,fl); }

        /* 3) shifter LO, logical & arithmetic, +/- shifts used by the byte reader / addr assembly */
        struct { uint32_t op; int n; bool ash; } sh[] = {
            {0x0f1201,1,false},{0x0f1608,8,false},{0x0f16fa,-6,false},{0x0f16f8,-8,false},{0x0f36fe,-2,true} };
        for (auto &s : sh) {
            uint32_t ref = s.ash ? ref_ashift(a,s.n) : ref_lshift(a,s.n);
            s_cpu.s_sr0(a); if (s.op==0x0f1201) s_cpu.s_ar(a);      /* 0f1201 shifts AR, others shift SR0 */
            step(s.op);
            report(s.ash?"ASH":"LSH", s_cpu.g_sr()==ref, "in=%04x by %d: core sr=%08x ref=%08x",
                   a,s.n,s_cpu.g_sr(),ref);
        }

        /* 4) SR = SR OR LSHIFT MR0 BY 8 (LO)  (0x0f1b08) — the 16-bit byte-assembly op at 0x2ff6 */
        { uint32_t old=((uint32_t)a<<16)|b; uint32_t ref = old | ref_lshift(b,8);
          s_cpu.s_sr(old); s_cpu.s_mr0(b); step(0x0f1b08);
          report("ORLSH", s_cpu.g_sr()==ref, "old=%08x mr0=%04x: core sr=%08x ref=%08x", old,b,s_cpu.g_sr(),ref); }

        /* 5) MAC UU, both M_MODE settings (0x20e00f) — command-1 runs with MSTAT bit4=INTEGER */
        for (int mmode=0; mmode<2; mmode++) {
            uint32_t prod = (uint32_t)a * (uint32_t)b;
            uint64_t ref = mmode ? prod : ((uint64_t)prod<<1);
            s_cpu.s_mx0(a); s_cpu.s_my0(b); s_cpu.t_mstat(mmode?0x10:0x00); step(0x20e00f);
            report(mmode?"MAC_I":"MAC_F", (uint32_t)(s_cpu.g_mr()&0xffffffff)==(uint32_t)(ref&0xffffffff),
                   "%04x*%04x m=%d: core mr=%010llx ref=%010llx", a,b,mmode,
                   (unsigned long long)(s_cpu.g_mr()&0xffffffffffULL),(unsigned long long)(ref&0xffffffffffULL));
        }
    }

    /* ---- SE-based (register-shift) shifter: shift_op, used by the DCS dequant BIT READER
       (0x011c SR=LSHIFT SI (HI); 0x011d SR=SR OR LSHIFT SI (LO); 0x011e SR=LSHIFT SR1 (LO)).
       These were NOT covered by the immediate-shift tests above. Datasheet: the 16-bit input is
       referenced HI(bits31:16=input<<16) or LO(bits15:0=input); shift by SE (8-bit signed):
       +=<<, -=>> (logical, zero-fill), |SE|>=32 => 0. A bug here drifts the bit cursor. ---- */
    for (int iter=0; iter<200000; iter++) {
        uint16_t in = rnd(); int se = (int)(int8_t)rnd();   /* full 8-bit signed SE range */
        auto lref = [&](uint32_t v)->uint32_t{ return (se>=0) ? (se<32 ? (v<<se):0u) : (se>-32 ? (v>>(-se)):0u); };
        /* op 0x0e000f: SR = LSHIFT SI (HI) by SE */
        { uint32_t ref = lref((uint32_t)in << 16);
          s_cpu.s_si(in); s_cpu.s_se(se); s_cpu.s_sr(0); step(0x0e000f);
          report("SESH_HI", s_cpu.g_sr()==ref, "SI=%04x se=%d: core=%08x ref=%08x", in,se,s_cpu.g_sr(),ref); }
        /* op 0x0e170f: SR = LSHIFT SR1 (LO) by SE  (xreg7=SR1) */
        { uint32_t ref = lref((uint32_t)in);
          s_cpu.s_sr1(in); s_cpu.s_se(se); step(0x0e170f);
          report("SESH_LO", (s_cpu.g_sr()&0xffffffff)==ref, "SR1=%04x se=%d: core=%08x ref=%08x", in,se,s_cpu.g_sr(),ref); }
        /* op 0x0e180f: SR = SR OR LSHIFT SI (LO) by SE  (function 3 = LO,OR; OR-accumulate into SR) */
        { uint32_t old=((uint32_t)rnd()<<16)|rnd(); uint32_t ref = old | lref((uint32_t)in);
          s_cpu.s_sr(old); s_cpu.s_si(in); s_cpu.s_se(se); step(0x0e180f);
          report("SESH_OR", s_cpu.g_sr()==ref, "SI=%04x se=%d old=%08x: core=%08x ref=%08x", in,se,old,s_cpu.g_sr(),ref); }
    }

    /* ---- interrupt machinery: vectoring + masking + priority, vs the ADSP-2101/2105 datasheet.
       The DCS decode uses IRQ2 (command mailbox) and IRQ1 (SPORT1 autobuffer). Datasheet vector
       table: IRQ2->0x04, SPORT0TX->0x08, SPORT0RX->0x0C, IRQ1/SPORT1TX->0x10, IRQ0/SPORT1RX->0x14,
       TIMER->0x18 (= 0x04 + priority_index*4); IMASK enable bit for index i = (0x20 >> i). ---- */
    {
        struct { int line; int idx; uint16_t vec; const char* nm; }
            irqs[] = { {ADSP2101_IRQ2,0,0x04,"IRQ2"}, {ADSP2101_IRQ1,3,0x10,"IRQ1"} };
        for (auto &q : irqs) {
            uint16_t enable = 0x20 >> q.idx;
            /* enabled + asserted (level-sensitive: ICNTL bit clear) => vector, execute NOP at vec */
            s_cpu.reset(); H.program[q.vec] = 0x000000; s_cpu.t_icntl(0); s_cpu.t_imask(enable);
            s_cpu.t_pc(0x0100); s_cpu.set_irq(q.line, 1); s_cpu.execute(1);
            report("IRQ_VEC", s_cpu.pc() == (uint16_t)(q.vec + 1),
                   "%s asserted+enabled: pc=%04x expected vector 0x%02x(+1)", q.nm, s_cpu.pc(), q.vec);
            /* asserted but MASKED (IMASK bit clear) => no vector, run the main-loop NOP at 0x0100 */
            s_cpu.reset(); H.program[0x0100] = 0x000000; s_cpu.t_icntl(0); s_cpu.t_imask(0);
            s_cpu.t_pc(0x0100); s_cpu.set_irq(q.line, 1); s_cpu.execute(1);
            report("IRQ_MASK", s_cpu.pc() == 0x0101,
                   "%s asserted but masked: pc=%04x expected 0x0101 (no vector)", q.nm, s_cpu.pc());
            s_cpu.set_irq(q.line, 0);
        }
    }

    /* teeth-check: the illegal-opcode/register logger must actually fire. Inject an invalid
       reg-move (0x0d04c0 = write_reg1 to reg-group-1 subgroup 3, illegal on a 2105). */
    { int save = g_adsp_logerr; long before = g_adsp_logerr_cnt; g_adsp_logerr = 1;
      H.program[PC] = 0x0d04c0; s_cpu.t_pc(PC); s_cpu.execute(1);
      bool fired = g_adsp_logerr_cnt > before; g_adsp_logerr = save;
      fprintf(stderr, "[opfuzz] illegal-opcode logger teeth-check (inject 0x0d04c0): %s\n",
              fired ? "FIRED — silent illegal ops WOULD be caught" : "SILENT — logger is broken!");
      if (!fired) fails++; }

    fprintf(stderr, "\n[opfuzz] %ld checks, %ld failures — core %s the datasheet reference on the decode-path opcodes\n",
            total, fails, fails? "DIVERGES from" : "MATCHES");
    return fails ? 1 : 0;
}
