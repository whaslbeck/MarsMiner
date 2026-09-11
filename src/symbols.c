/* symbols.c — parse a Pinball 2000 *_symbols.rom.  1:1 with parse_symbols.py.
 *
 *   [0x00..0x0B] "SYMBOL TABLE"
 *   [0x10..0x13] u32 count
 *   [0x14..0x17] u32 names-region size
 *   [0x18..]     count * {u32 address, u32 name_offset}
 *   names_base = 0x18 + count*8; name_offset is relative to names_base.
 */
#include "marsminer.h"

#include <stdlib.h>
#include <string.h>

int mm_symbols_parse(mm_symtab *st, const mm_buf *rom) {
    memset(st, 0, sizeof *st);
    if (!rom->data || rom->len < 0x18)
        return -1;
    if (memcmp(rom->data, "SYMBOL TABLE", 12) != 0)
        return -1;
    uint32_t count = mm_u32(rom, 0x10);
    if (count == 0 || count > 1000000u)
        return -1;
    size_t ent = 0x18;
    size_t names_base = ent + (size_t)count * 8;
    if (names_base > rom->len)
        return -1;

    mm_sym *syms = (mm_sym *)calloc(count, sizeof *syms);
    if (!syms)
        return -1;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = mm_u32(rom, ent + (size_t)i * 8);
        uint32_t noff = mm_u32(rom, ent + (size_t)i * 8 + 4);
        size_t p = names_base + noff;
        const char *name = "";
        size_t nlen = 0;
        if (p <= rom->len) {
            /* NUL-terminated (fall back to buffer end) */
            size_t q = p;
            while (q < rom->len && rom->data[q] != 0)
                q++;
            nlen = q - p;
            name = (const char *)(rom->data + p);
        }
        char *dup = (char *)malloc(nlen + 1);
        if (!dup) { /* best-effort cleanup */
            for (uint32_t j = 0; j < i; j++)
                free(syms[j].name);
            free(syms);
            return -1;
        }
        if (nlen)
            memcpy(dup, name, nlen);
        dup[nlen] = '\0';
        syms[i].addr = addr;
        syms[i].name = dup;
    }
    st->syms = syms;
    st->count = count;
    return 0;
}

void mm_symtab_free(mm_symtab *st) {
    if (!st->syms)
        return;
    for (size_t i = 0; i < st->count; i++)
        free(st->syms[i].name);
    free(st->syms);
    st->syms = NULL;
    st->count = 0;
}

int mm_symbols_write_csv(const mm_symtab *st, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs("address,name\n", f);
    for (size_t i = 0; i < st->count; i++)
        fprintf(f, "0x%08x,%s\n", st->syms[i].addr, st->syms[i].name);
    return fclose(f) == 0 ? 0 : -1;
}

uint32_t mm_sym_addr(const mm_symtab *st, const char *name, int *found) {
    for (size_t i = 0; i < st->count; i++)
        if (strcmp(st->syms[i].name, name) == 0) {
            if (found)
                *found = 1;
            return st->syms[i].addr;
        }
    if (found)
        *found = 0;
    return 0;
}
