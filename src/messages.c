/* messages.c — extract the RFM DMD text strings (message_struct).
 * 1:1 with extract_messages.py.  Each msg_<name> symbol's first word
 * points at a NUL-terminated ASCII string in the game ROM. */
#include "marsminer.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t addr;
    const char *name;
} msgent;

static int cmp_msg(const void *a, const void *b) {
    const msgent *x = a, *y = b;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* Read a printable C string from the game ROM at guest addr. Returns 1 and fills
 * `out` (heap, caller frees) if it is non-empty and all chars are 9..126. */
static int cstr(const mm_buf *grom, uint32_t addr, char **out) {
    *out = NULL;
    if (addr < MM_GBASE)
        return 0;
    size_t o = addr - MM_GBASE;
    if (o >= grom->len)
        return 0;
    size_t e = o;
    while (e < grom->len && grom->data[e] != 0)
        e++;
    size_t len = e - o;
    if (len == 0)
        return 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = grom->data[o + i];
        if (!(ch >= 9 && ch <= 126))
            return 0;
    }
    char *s = (char *)malloc(len + 1);
    if (!s)
        return 0;
    memcpy(s, grom->data + o, len);
    s[len] = '\0';
    *out = s;
    return 1;
}

long mm_extract_messages(const mm_opts *o, mm_ctx *c, const char *out_csv) {
    (void)o;
    const mm_symtab *st = &c->symtab;
    const mm_buf *grom = &c->game_rom;

    msgent *m = (msgent *)malloc(st->count * sizeof *m);
    if (!m)
        return -1;
    size_t nm = 0;
    for (size_t i = 0; i < st->count; i++)
        if (strncmp(st->syms[i].name, "msg_", 4) == 0) {
            m[nm].addr = st->syms[i].addr;
            m[nm].name = st->syms[i].name;
            nm++;
        }
    qsort(m, nm, sizeof *m, cmp_msg);

    FILE *f = fopen(out_csv, "wb");
    if (!f) {
        free(m);
        return -1;
    }
    const char *hdr[] = {"message", "text"};
    mm_csv_row(f, hdr, 2);

    long ok = 0;
    for (size_t i = 0; i < nm; i++) {
        uint32_t ptr = 0;
        int have_ptr = 0;
        if (m[i].addr >= MM_GBASE && m[i].addr - MM_GBASE + 4 <= grom->len) {
            ptr = mm_u32(grom, m[i].addr - MM_GBASE);
            have_ptr = 1;
        }
        char *text = NULL;
        if (have_ptr)
            cstr(grom, ptr, &text);
        const char *row[] = {m[i].name, text ? text : ""};
        mm_csv_row(f, row, 2);
        if (text) {
            ok++;
            free(text);
        }
    }
    fclose(f);
    free(m);
    return ok;
}
