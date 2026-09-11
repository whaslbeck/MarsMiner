/* music.cpp — NOKILL music-loop pass: seamless crossfade loops for kind=music.
 * 1:1 port of extract.py:_music_loops + make_music_loop.py.
 *
 * The per-segment group-KILL truncates DCS music to ~0.03 s, so each kind=music
 * id is re-decoded with DCS_NOKILL (via the single-id decoder dcs2_extract) at a
 * fixed render length, then folded into an equal-power crossfade loop and re-
 * encoded to FLAC in place. The manifest's seconds/frames/loop_seconds update to
 * match. Weak/finite tracks too short to loop are kept as one-shots.
 *
 * This is a DSP stage (double-precision), so its FLAC output is perceptually — not
 * byte — identical to the numpy reference; the deterministic decoders elsewhere are
 * byte/pixel exact.
 */
extern "C" {
#include "marsminer.h"
}
#include "flac_encode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h> /* rmdir */
#include <vector>

int dcs2_extract_main(int argc, char **argv); /* -Dmain=dcs2_extract_main */

static const int SR = 31250;

/* ---- read a stereo S16 WAV -> mono double ------------------------------- */
static std::vector<double> read_dcs_wav(const char *path) {
    std::vector<double> out;
    mm_buf b = mm_load(path);
    if (!b.data)
        return out;
    /* find 'data' chunk; payload starts 8 bytes after the tag (tag+size) */
    size_t off = 44;
    for (size_t i = 0; i + 4 <= b.len; i++)
        if (memcmp(b.data + i, "data", 4) == 0) {
            off = i + 8;
            break;
        }
    /* channels from fmt chunk (default 2) */
    int ch = 2;
    for (size_t i = 0; i + 16 <= b.len; i++)
        if (memcmp(b.data + i, "fmt ", 4) == 0) {
            ch = b.data[i + 10] | b.data[i + 11] << 8;
            break;
        }
    if (ch < 1)
        ch = 1;
    const int16_t *s = (const int16_t *)(b.data + off);
    size_t n = (b.len > off) ? (b.len - off) / 2 : 0;
    if (ch == 2) {
        out.reserve(n / 2);
        for (size_t i = 0; i + 1 < n; i += 2)
            out.push_back(((double)s[i] + (double)s[i + 1]) * 0.5);
    } else {
        out.reserve(n);
        for (size_t i = 0; i < n; i++)
            out.push_back((double)s[i]);
    }
    mm_buf_free(&b);
    return out;
}

/* ---- trim_lead: drop leading near-silence (win=256, thresh=250) --------- */
static std::vector<double> trim_lead(const std::vector<double> &a, double thresh = 250,
                                     int win = 256) {
    size_t N = a.size();
    if ((int)N < win)
        return a;
    /* centered moving average of a^2 (np.convolve 'same'), then sqrt */
    std::vector<double> sq(N);
    for (size_t i = 0; i < N; i++)
        sq[i] = a[i] * a[i];
    std::vector<double> pre(N + 1, 0.0);
    for (size_t i = 0; i < N; i++)
        pre[i + 1] = pre[i] + sq[i];
    int half = win / 2; /* np.convolve 'same' centers the window */
    for (size_t i = 0; i < N; i++) {
        long lo =
            (long)i - (win - 1) + half; /* 'same' alignment: out[i]=sum_{k} a2[i-half+k]/win */
        long hi = lo + win;             /* [lo,hi) with zero-padding outside [0,N) */
        long clo = lo < 0 ? 0 : lo;
        long chi = hi > (long)N ? (long)N : hi;
        double s = (chi > clo) ? (pre[chi] - pre[clo]) : 0.0;
        double e = std::sqrt(s / win);
        if (e > thresh)
            return std::vector<double>(a.begin() + i, a.end());
    }
    return a;
}

/* ---- best_loop_len: lag with strongest self-similarity ------------------ */
static int best_loop_len(const std::vector<double> &a, double lo_s, double hi_s) {
    size_t N = a.size();
    int refN = (int)std::min((size_t)(2.0 * SR), N / 3);
    if (refN < SR / 2)
        return (int)(hi_s * SR);
    int r0 = (N > (size_t)(0.5 * SR) + refN) ? (int)(0.5 * SR) : 0;
    /* ref = a[r0:r0+refN] - mean */
    double m = 0;
    for (int i = 0; i < refN; i++)
        m += a[r0 + i];
    m /= refN;
    std::vector<double> ref(refN);
    double rn = 0;
    for (int i = 0; i < refN; i++) {
        ref[i] = a[r0 + i] - m;
        rn += ref[i] * ref[i];
    }
    rn = std::sqrt(rn) + 1e-9;
    double bestc = -2.0;
    int bestL = (int)(hi_s * SR);
    for (int L = (int)(lo_s * SR); L < (int)(hi_s * SR); L += 64) {
        if ((size_t)(r0 + L + refN) > N)
            break;
        double sm = 0;
        for (int i = 0; i < refN; i++)
            sm += a[r0 + L + i];
        sm /= refN;
        double dot = 0, sn = 0;
        for (int i = 0; i < refN; i++) {
            double v = a[r0 + L + i] - sm;
            dot += ref[i] * v;
            sn += v * v;
        }
        double c = dot / (rn * (std::sqrt(sn) + 1e-9));
        if (c > bestc) {
            bestc = c;
            bestL = L;
        }
    }
    return bestL;
}

/* ---- crossfade_loop ----------------------------------------------------- */
static std::vector<double> crossfade_loop(const std::vector<double> &a, int L, int X) {
    int N = (int)a.size();
    if (N < L + X)
        L = N - X;
    if (L <= X)
        return a;
    std::vector<double> F(a.begin(), a.begin() + L);
    for (int i = 0; i < X; i++) {
        double t = (double)i / X;
        double fin = std::sin(0.5 * M_PI * t);
        double fout = std::cos(0.5 * M_PI * t);
        F[i] = fin * a[i] + fout * a[L + i];
    }
    return F;
}

/* ---- manifest CSV row (unquoted numeric/identifier fields) --------------- */
struct Row {
    std::vector<std::string> f;
};
static std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : line) {
        if (ch == ',') {
            out.push_back(cur);
            cur.clear();
        } else
            cur += ch;
    }
    out.push_back(cur);
    return out;
}

long mm_music_loops(const mm_opts *o, mm_ctx *c, const char *sounds_dir) {
    (void)o;
    (void)c;
    char manp[1300];
    snprintf(manp, sizeof manp, "%s/manifest.csv", sounds_dir);
    mm_buf mb = mm_load(manp);
    if (!mb.data)
        return 0;

    /* parse manifest into rows */
    std::string all((const char *)mb.data, mb.len);
    mm_buf_free(&mb);
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char ch : all) {
            if (ch == '\n') {
                if (!cur.empty() && cur.back() == '\r')
                    cur.pop_back();
                lines.push_back(cur);
                cur.clear();
            } else
                cur += ch;
        }
        if (!cur.empty())
            lines.push_back(cur);
    }
    if (lines.empty())
        return 0;
    std::vector<std::string> hdr = split_csv(lines[0]);
    int ci_id = -1, ci_kind = -1, ci_sec = -1, ci_frames = -1, ci_loop = -1;
    for (int i = 0; i < (int)hdr.size(); i++) {
        if (hdr[i] == "id")
            ci_id = i;
        else if (hdr[i] == "kind")
            ci_kind = i;
        else if (hdr[i] == "seconds")
            ci_sec = i;
        else if (hdr[i] == "frames")
            ci_frames = i;
        else if (hdr[i] == "loop_seconds")
            ci_loop = i;
    }
    if (ci_id < 0 || ci_kind < 0)
        return 0;

    const int blocks = 16 * 32;             /* secs*32, secs=16 (extract.py default) */
    const int X = (int)(250.0 / 1000 * SR); /* 250 ms crossfade */
    const double MIN_S = 6.0, MAX_S = 14.0;
    char blockstr[16];
    snprintf(blockstr, sizeof blockstr, "%d", blocks);

    char root[16];
    snprintf(root, sizeof root, ".");
    std::string tmpdir = std::string(sounds_dir) + "/.mmtmp";
    mm_mkdir_p(tmpdir.c_str());

    long done = 0;
    std::vector<Row> rows;
    for (size_t li = 1; li < lines.size(); li++) {
        std::vector<std::string> f = split_csv(lines[li]);
        Row r;
        r.f = f;
        if ((int)f.size() > ci_kind && f[ci_kind] == "music" && (int)f.size() > ci_id) {
            const std::string &id = f[ci_id];
            std::string wav = tmpdir + "/" + id + ".wav";
            setenv("DCS_NOKILL", "1", 1);
            setenv("DCS_RENDERBLOCKS", blockstr, 1);
            setenv("DCS_WAV", wav.c_str(), 1);
            char *av[4] = {(char *)"dcs2_extract", root, (char *)id.c_str(), (char *)"4"};
            dcs2_extract_main(4, av);
            std::vector<double> sig = read_dcs_wav(wav.c_str());
            remove(wav.c_str());
            if (!sig.empty()) {
                sig = trim_lead(sig);
                double dur = (double)sig.size() / SR;
                std::vector<double> F;
                int is_oneshot;
                if (dur < MIN_S + 0.5) {
                    F = sig;
                    is_oneshot = 1;
                } else {
                    double hi = std::min(MAX_S, dur - 0.25 - 0.1);
                    int L = best_loop_len(sig, MIN_S, hi);
                    F = crossfade_loop(sig, L, X);
                    is_oneshot = 0;
                }
                /* to S16 mono */
                std::vector<int16_t> pcm(F.size());
                for (size_t i = 0; i < F.size(); i++) {
                    double v = std::round(F[i]);
                    if (v > 32767)
                        v = 32767;
                    else if (v < -32768)
                        v = -32768;
                    pcm[i] = (int16_t)v;
                }
                std::string flac = std::string(sounds_dir) + "/" + id + ".flac";
                if (flac_encode_s16(flac.c_str(), pcm.data(), pcm.size(), 1, SR, 8)) {
                    double outdur = (double)F.size() / SR;
                    char sd[32], ff[32], ls[32];
                    snprintf(sd, sizeof sd, "%.3f", outdur);
                    snprintf(ff, sizeof ff, "%d", (int)std::lround(outdur * SR));
                    snprintf(ls, sizeof ls, "%.3f", is_oneshot ? 0.0 : outdur);
                    if (ci_sec >= 0 && (int)r.f.size() > ci_sec)
                        r.f[ci_sec] = sd;
                    if (ci_frames >= 0 && (int)r.f.size() > ci_frames)
                        r.f[ci_frames] = ff;
                    if (ci_loop >= 0 && (int)r.f.size() > ci_loop)
                        r.f[ci_loop] = ls;
                    done++;
                }
            }
        }
        rows.push_back(r);
    }
    unsetenv("DCS_NOKILL");
    unsetenv("DCS_RENDERBLOCKS");
    unsetenv("DCS_WAV");
    rmdir(tmpdir.c_str());

    /* rewrite manifest with updated music durations */
    if (done > 0) {
        FILE *w = fopen(manp, "wb");
        if (w) {
            fputs(lines[0].c_str(), w);
            fputc('\n', w);
            for (auto &r : rows) {
                for (size_t i = 0; i < r.f.size(); i++) {
                    if (i)
                        fputc(',', w);
                    fputs(r.f[i].c_str(), w);
                }
                fputc('\n', w);
            }
            fclose(w);
        }
    }
    return done;
}
