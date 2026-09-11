/* used_ids.c — derive the DCS "play set" (the sound-extraction allow-list) from the ROM itself.
 *
 * Every AudioRequest the game issues points at an AudioCodeList (acl); an acl holds the
 * AudioCodeDesc(s) that may sound, and an acd's first u16 is the DCS sound id. So walking every
 * `*_acl` symbol in the shipped symbol table to its acds yields exactly the ids the game can
 * play — a small subset of the ids defined in the sound ROMs. Restricting the decode to that set
 * is what keeps an extraction run from spending hours on unreachable ids.
 *
 * This walk is GAME-AGNOSTIC: it knows only the Pinball 2000 audio-table shapes and the linked
 * symbol table, never a specific title. Nothing here is RfM-specific, and no pre-computed id list
 * is shipped or needed — which is what lets the same code serve another P2K title (SWE1) whose
 * play set is a different one. For the RfM regression baseline that pins this walk's output, see
 * tests/baseline/rfm_dcs_used_ids.txt (and tests/used_ids_test.cpp).
 *
 * 1:1 with gen_used_ids.py, but from the same ROM the decode reads.
 */
#include "marsminer.h"

#include <string.h>

#define ACL_MAX_ENTRIES 256 /* sanity bound: a plausible acl list[] length */

long mm_used_ids_derive(const mm_ctx *c, uint16_t *ids, size_t max, long *n_acls) {
    if (n_acls)
        *n_acls = 0;
    if (!c || !c->game_rom.data || c->game_rom.len < 8 || c->symtab.count == 0)
        return -1;

    const mm_buf *g = &c->game_rom;
    const mm_symtab *st = &c->symtab;
    const size_t glen = g->len;

    uint8_t seen[65536 / 8];
    memset(seen, 0, sizeof seen);
    long acls = 0;

    for (size_t s = 0; s < st->count; s++) {
        const char *nm = st->syms[s].name;
        size_t L = nm ? strlen(nm) : 0;
        if (L < 4 || strcmp(nm + L - 4, "_acl") != 0)
            continue; /* an AudioCodeList */
        uint32_t a = st->syms[s].addr;
        if (a < MM_GBASE || (size_t)(a - MM_GBASE) + 8 > glen)
            continue;
        uint32_t cnt = mm_u32(g, a - MM_GBASE);
        uint32_t lst = mm_u32(g, a - MM_GBASE + 4);
        if (cnt == 0 || cnt > ACL_MAX_ENTRIES)
            continue;
        if (lst < MM_GBASE || (size_t)(lst - MM_GBASE) + 4u * cnt > glen)
            continue;
        acls++;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t acd = mm_u32(g, lst - MM_GBASE + 4u * i);
            if (acd < MM_GBASE || (size_t)(acd - MM_GBASE) + 2 > glen)
                continue;
            uint16_t code = mm_u16(g, acd - MM_GBASE);
            if (code)
                seen[code >> 3] |= (uint8_t)(1u << (code & 7)); /* code 0 = no-sound */
        }
    }
    if (n_acls)
        *n_acls = acls;
    if (acls == 0)
        return -1; /* no acl symbols at all — not a P2K audio-table image */

    long n = 0;
    for (unsigned code = 1; code < 65536; code++)
        if (seen[code >> 3] & (1u << (code & 7))) {
            if ((size_t)n < max)
                ids[n] = (uint16_t)code;
            n++;
        }
    return n; /* n > max means ids[] was too small; nothing truncated silently */
}

long mm_used_ids_write(const char *path, const uint16_t *ids, long n, const char *header) {
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    if (header && header[0])
        fputs(header, f);
    for (long i = 0; i < n; i++)
        fprintf(f, "0x%04x\n", ids[i]);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad)
        return -1;
    return n;
}
