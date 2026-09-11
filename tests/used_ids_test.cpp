/* used_ids_test.cpp — regression proof for the DCS play-set walk (src/used_ids.c).
 *
 * mm_used_ids_derive() decides WHICH sounds an extraction run decodes. If it silently
 * under-collects, assets go missing; if it over-collects, a run burns hours on unreachable ids.
 * The walk has no other check on it — the decode can't tell a short list from a correct one — so
 * this test pins it against a known-correct answer.
 *
 * The baseline (tests/baseline/rfm_dcs_used_ids.txt) was produced from ONE game at ONE ROM
 * version, and says so in its own header; MarsMiner's code is title-agnostic. So the test reads
 * the expectations OUT of that header — the two ROM md5s, the id count, the acl count — and
 * verifies it is comparing against exactly the image the baseline was made from. A different
 * Pinball 2000 title (SWE1) or a different RfM version gets its own baseline file here; it must
 * not be checked against this one.
 *
 * Skips itself (pass) when the ROMs are absent, so a ROM-free checkout still runs `make test`.
 */
extern "C" {
#include "marsminer.h"
}
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

/* ---- baseline file: header expectations + the id set --------------------- */
struct Baseline {
    std::string game_md5, sym_md5;
    long n_ids = -1;
    long n_acls = -1;
    std::set<unsigned> ids;
    bool ok = false;
};

/* the last whitespace-separated token of a line */
static std::string last_token(const std::string &s) {
    size_t e = s.find_last_not_of(" \t\r\n");
    if (e == std::string::npos)
        return "";
    size_t b = s.find_last_of(" \t", e);
    return s.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b));
}

static Baseline read_baseline(const char *path) {
    Baseline b;
    FILE *f = fopen(path, "r");
    if (!f)
        return b;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        if (s.compare(0, 2, "0x") == 0) { /* an id line */
            b.ids.insert((unsigned)strtoul(s.c_str() + 2, nullptr, 16));
            continue;
        }
        if (s[0] != '#')
            continue;
        if (s.find("game.rom") != std::string::npos && s.find("md5") != std::string::npos)
            b.game_md5 = last_token(s);
        else if (s.find("symbols.rom") != std::string::npos && s.find("md5") != std::string::npos)
            b.sym_md5 = last_token(s);
        else if (s.find("result") != std::string::npos) {
            /* "#   result   796 play ids reachable through 582 acl tables" */
            long a = 0, c = 0;
            const char *p = strstr(s.c_str(), "result");
            if (p && sscanf(p, "result %ld play ids reachable through %ld", &a, &c) == 2) {
                b.n_ids = a;
                b.n_acls = c;
            }
        }
    }
    fclose(f);
    b.ok = !b.game_md5.empty() && !b.sym_md5.empty() && b.n_ids >= 0 && !b.ids.empty();
    return b;
}

/* ---- the bundle files (by their documented names) ------------------------ */
static std::string bundle_path(const char *roms, const char *leaf) {
    return std::string(roms) + "/update_0180/pin2000_50070_0180_" + leaf;
}

static int fails = 0;
static void check(const char *what, bool ok, const std::string &detail = "") {
    printf("%-52s [%s]%s%s\n", what, ok ? "OK" : "FAIL", detail.empty() ? "" : "  ",
           detail.c_str());
    if (!ok)
        fails++;
}

int main(int argc, char **argv) {
    const char *roms = argc > 1 ? argv[1] : "roms";
    const char *base = argc > 2 ? argv[2] : "tests/baseline/rfm_dcs_used_ids.txt";

    Baseline bl = read_baseline(base);
    if (!bl.ok) {
        printf("[FAIL] baseline %s missing or unparsable\n", base);
        return 1;
    }

    std::string gpath = bundle_path(roms, "game.rom"), spath = bundle_path(roms, "symbols.rom");
    mm_buf game = mm_load(gpath.c_str());
    mm_buf syms = mm_load(spath.c_str());
    if (!game.data || !syms.data) {
        printf("[skip] ROMs not found under %s — pass your ROM directory as argv[1]\n", roms);
        mm_buf_free(&game);
        mm_buf_free(&syms);
        return 0;
    }

    /* 1. the baseline is ROM-version-specific: confirm this IS that ROM. */
    char gmd5[33], smd5[33];
    mm_md5_hex(game.data, game.len, gmd5);
    mm_md5_hex(syms.data, syms.len, smd5);
    check("game.rom matches the baseline's md5", bl.game_md5 == gmd5,
          bl.game_md5 == gmd5 ? "" : std::string("got ") + gmd5);
    check("symbols.rom matches the baseline's md5", bl.sym_md5 == smd5,
          bl.sym_md5 == smd5 ? "" : std::string("got ") + smd5);
    if (fails) { /* wrong image: the id comparison below would be meaningless */
        printf("\nThis ROM is not the one the baseline was derived from (RfM 1.80,\n"
               "bundle pin2000_50070_0180). Add a baseline for it rather than\n"
               "regenerating this one — see the baseline header.\n");
        mm_buf_free(&game);
        mm_buf_free(&syms);
        return 1;
    }

    /* 2. run the real walk over the real image. */
    mm_ctx c;
    memset(&c, 0, sizeof c);
    c.game_rom = game;
    if (mm_symbols_parse(&c.symtab, &syms) != 0) {
        printf("[FAIL] symbol parse\n");
        return 1;
    }

    std::vector<uint16_t> ids(65536);
    long acls = 0;
    long n = mm_used_ids_derive(&c, ids.data(), ids.size(), &acls);

    check("mm_used_ids_derive() succeeded", n > 0 && (size_t)n <= ids.size(),
          "n=" + std::to_string(n));
    check("acl count matches the baseline", acls == bl.n_acls,
          "walked " + std::to_string(acls) + ", baseline " + std::to_string(bl.n_acls));
    check("id count matches the baseline", n == bl.n_ids,
          std::to_string(n) + " vs " + std::to_string(bl.n_ids));

    /* 3. the sets must be identical, not merely equal in size. */
    std::set<unsigned> got;
    for (long i = 0; i < n && (size_t)i < ids.size(); i++)
        got.insert(ids[i]);
    std::vector<unsigned> missing, extra;
    for (unsigned v : bl.ids)
        if (!got.count(v))
            missing.push_back(v);
    for (unsigned v : got)
        if (!bl.ids.count(v))
            extra.push_back(v);
    check("play set identical to the baseline", missing.empty() && extra.empty(),
          "missing " + std::to_string(missing.size()) + ", extra " + std::to_string(extra.size()));
    for (size_t i = 0; i < missing.size() && i < 12; i++)
        printf("    missing 0x%04x\n", missing[i]);
    for (size_t i = 0; i < extra.size() && i < 12; i++)
        printf("    extra   0x%04x\n", extra[i]);

    /* 4. the derived list must survive the round trip through the decoder's file format. */
    const char *tmp = "build/.used_ids_roundtrip.txt";
    long w = mm_used_ids_write(tmp, ids.data(), n, "# test\n");
    Baseline rt = read_baseline(tmp);
    remove(tmp);
    check("written list re-reads as the same set", w == n && rt.ids == got);

    mm_symtab_free(&c.symtab);
    mm_buf_free(&game);
    mm_buf_free(&syms);

    printf("\n%s\n", fails == 0 ? "PLAY-SET WALK OK (RfM 1.80 baseline)" : "FAILURES");
    return fails ? 1 : 0;
}
