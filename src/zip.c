/* zip.c — a minimal STORE-method ZIP writer (self-contained, no dependency).
 *
 * The assets are already compressed (PNG deflate, FLAC), so storing them
 * uncompressed wastes essentially nothing while keeping the container a single
 * dependency-free file both this tool and the engine can produce/read. Output is
 * a standard .zip any tool can open. Deterministic (fixed DOS timestamp) so the
 * same assets produce the same archive.
 */
#define _POSIX_C_SOURCE 200809L /* strdup under -std=c11 */
#include "marsminer.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- CRC32 (IEEE, poly 0xEDB88320) ------------------------------------- */
static uint32_t crc_tab[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
    crc_ready = 1;
}
static uint32_t crc32_buf(const uint8_t *p, size_t n, uint32_t crc) {
    if (!crc_ready)
        crc_init();
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        crc = crc_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- little-endian writers --------------------------------------------- */
static void w16(FILE *f, uint16_t v) {
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}
static void w32(FILE *f, uint32_t v) {
    for (int i = 0; i < 4; i++)
        fputc((v >> (8 * i)) & 0xFF, f);
}

/* central-directory record kept until finish */
typedef struct {
    char *name;
    uint32_t crc, size, offset;
} cdent;

struct mm_zip {
    FILE *f;
    cdent *ents;
    size_t n, cap;
    uint32_t offset; /* running byte offset in the file */
    int ok;
};

mm_zip *mm_zip_create(const char *path) {
    mm_zip *z = (mm_zip *)calloc(1, sizeof *z);
    if (!z)
        return NULL;
    z->f = fopen(path, "wb");
    if (!z->f) {
        free(z);
        return NULL;
    }
    z->ok = 1;
    return z;
}

/* Add one file from disk under archive name `arc` (forward slashes). */
int mm_zip_add_file(mm_zip *z, const char *fs_path, const char *arc) {
    if (!z || !z->ok)
        return -1;
    mm_buf b = mm_load(fs_path);
    if (!b.data && mm_path_exists(fs_path)) { /* empty file: b.data NULL, len 0 — allow */
    }
    uint32_t crc = crc32_buf(b.data, b.len, 0);
    uint16_t nlen = (uint16_t)strlen(arc);

    /* grow central-dir list */
    if (z->n == z->cap) {
        size_t nc = z->cap ? z->cap * 2 : 256;
        cdent *ne = (cdent *)realloc(z->ents, nc * sizeof *ne);
        if (!ne) {
            mm_buf_free(&b);
            z->ok = 0;
            return -1;
        }
        z->ents = ne;
        z->cap = nc;
    }
    z->ents[z->n].name = strdup(arc);
    z->ents[z->n].crc = crc;
    z->ents[z->n].size = (uint32_t)b.len;
    z->ents[z->n].offset = z->offset;
    z->n++;

    /* local file header (method 0 = store; sizes known up front) */
    FILE *f = z->f;
    w32(f, 0x04034b50); /* signature */
    w16(f, 20);         /* version needed */
    w16(f, 0);          /* flags */
    w16(f, 0);          /* method: store */
    w16(f, 0);
    w16(f, 0x21); /* mod time / date = 1980-01-01 (deterministic) */
    w32(f, crc);
    w32(f, (uint32_t)b.len); /* compressed size */
    w32(f, (uint32_t)b.len); /* uncompressed size */
    w16(f, nlen);
    w16(f, 0); /* extra len */
    fwrite(arc, 1, nlen, f);
    if (b.len)
        fwrite(b.data, 1, b.len, f);
    z->offset += 30 + nlen + (uint32_t)b.len;
    mm_buf_free(&b);
    return 0;
}

int mm_zip_finish(mm_zip *z) {
    if (!z)
        return -1;
    FILE *f = z->f;
    int rc = z->ok ? 0 : -1;
    uint32_t cd_start = z->offset;
    uint32_t cd_size = 0;
    for (size_t i = 0; i < z->n; i++) {
        cdent *e = &z->ents[i];
        uint16_t nlen = (uint16_t)strlen(e->name);
        w32(f, 0x02014b50); /* central-dir signature */
        w16(f, 20);
        w16(f, 20); /* version made by / needed */
        w16(f, 0);
        w16(f, 0); /* flags / method(store) */
        w16(f, 0);
        w16(f, 0x21); /* time / date */
        w32(f, e->crc);
        w32(f, e->size);
        w32(f, e->size);
        w16(f, nlen);
        w16(f, 0);
        w16(f, 0); /* name / extra / comment len */
        w16(f, 0);
        w16(f, 0);         /* disk no / internal attrs */
        w32(f, 0);         /* external attrs */
        w32(f, e->offset); /* local-header offset */
        fwrite(e->name, 1, nlen, f);
        cd_size += 46 + nlen; /* fixed record is 46 bytes + name */
    }
    /* end of central directory */
    w32(f, 0x06054b50);
    w16(f, 0);
    w16(f, 0); /* disk numbers */
    w16(f, (uint16_t)z->n);
    w16(f, (uint16_t)z->n);
    w32(f, cd_size);
    w32(f, cd_start);
    w16(f, 0); /* comment len */

    if (fclose(f) != 0)
        rc = -1;
    for (size_t i = 0; i < z->n; i++)
        free(z->ents[i].name);
    free(z->ents);
    free(z);
    return rc;
}

/* ---- pack a directory tree --------------------------------------------- */
/* Recurse `dir`, adding every regular file to `z` with archive name `prefix/…`.
 * Entries are sorted per directory so the archive order is deterministic. */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static long pack_recurse(mm_zip *z, const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue; /* ".", "..", and file-manager junk like .DS_Store */
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = (char **)realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);

    long added = 0;
    for (size_t i = 0; i < n; i++) {
        char fs[2048], arc[2048];
        snprintf(fs, sizeof fs, "%s/%s", dir, names[i]);
        if (prefix[0])
            snprintf(arc, sizeof arc, "%s/%s", prefix, names[i]);
        else
            snprintf(arc, sizeof arc, "%s", names[i]);
        struct stat st;
        if (stat(fs, &st) != 0) {
            free(names[i]);
            continue;
        }
        if (S_ISDIR(st.st_mode))
            added += pack_recurse(z, fs, arc);
        else if (S_ISREG(st.st_mode)) {
            if (mm_zip_add_file(z, fs, arc) == 0)
                added++;
        }
        free(names[i]);
    }
    free(names);
    return added;
}

long mm_zip_pack_dir(const char *dir, const char *zip_path) {
    mm_zip *z = mm_zip_create(zip_path);
    if (!z)
        return -1;
    long n = pack_recurse(z, dir, "");
    if (mm_zip_finish(z) != 0)
        return -1;
    return n;
}

/* ---- recursive delete --------------------------------------------------- */
/* lstat, not stat: a symlink to a directory must be unlinked, never descended into —
   otherwise a link inside the tree would take the delete outside it. */
int mm_rmtree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return unlink(dir) == 0 ? 0 : -1; /* not a dir: try as file */
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue; /* ".", "..", and file-manager junk like .DS_Store */
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
            mm_rmtree(p);
        else
            unlink(p);
    }
    closedir(d);
    return rmdir(dir);
}

/* Does `dir` look like an asset tree WE produced? mm_rmtree is pointed at a path the user
   named (--out), and deleting it is the default, so guess conservatively: every entry must be
   one of the names a run writes. An unrecognised entry means the directory is (or contains)
   something else, and we keep our hands off it. */
int mm_is_asset_tree(const char *dir) {
    static const char *KNOWN[] = {"sounds", "images",        "fonts",    "tables",
                                  "movies", "manifest.json", "index.csv"};
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int ok = 1, seen = 0;
    while (ok && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        int known = 0;
        for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
            if (strcmp(e->d_name, KNOWN[i]) == 0)
                known = 1;
        if (known)
            seen = 1;
        else
            ok = 0;
    }
    closedir(d);
    return ok && seen;
}
