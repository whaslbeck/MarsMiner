/* util.c — file I/O, logging, buffer reads, mkdir -p, MD5, human sizes. */
#include "marsminer.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

int mm_verbosity = 0;

/* -------------------------------------------------------------------------- */
void mm_log(int level, const char *fmt, ...) {
    if (mm_verbosity < level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[marsminer] ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    fflush(stdout);
}

void mm_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[marsminer] ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    fflush(stdout);
}

void mm_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[marsminer] warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void mm_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[marsminer] error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* -------------------------------------------------------------------------- */
mm_buf mm_load(const char *path) {
    mm_buf b = {0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return b;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return b;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return b;
    }
    rewind(f);
    uint8_t *d = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!d) {
        fclose(f);
        return b;
    }
    size_t rd = fread(d, 1, (size_t)n, f);
    fclose(f);
    if (rd != (size_t)n) {
        free(d);
        return b;
    }
    b.data = d;
    b.len = (size_t)n;
    return b;
}

mm_buf mm_load_required(const char *path, const char *what) {
    mm_buf b = mm_load(path);
    if (!b.data)
        mm_die("cannot read %s: %s (%s)", what ? what : "file", path, strerror(errno));
    return b;
}

int mm_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t wr = len ? fwrite(data, 1, len, f) : 0;
    int ok = (wr == len);
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

void mm_buf_free(mm_buf *b) {
    if (b && b->data) {
        free(b->data);
        b->data = NULL;
        b->len = 0;
    }
}

uint32_t mm_u32(const mm_buf *b, size_t off) {
    if (!b->data || off + 4 > b->len)
        return 0;
    const uint8_t *p = b->data + off;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint16_t mm_u16(const mm_buf *b, size_t off) {
    if (!b->data || off + 2 > b->len)
        return 0;
    const uint8_t *p = b->data + off;
    return (uint16_t)(p[0] | p[1] << 8);
}
uint8_t mm_u8(const mm_buf *b, size_t off) {
    if (!b->data || off >= b->len)
        return 0;
    return b->data[off];
}

/* -------------------------------------------------------------------------- */
int mm_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* mkdir -p */
int mm_mkdir_p(const char *path) {
    char tmp[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp)
        return -1;
    memcpy(tmp, path, n + 1);
    if (tmp[n - 1] == '/')
        tmp[n - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

const char *mm_human_size(uint64_t bytes, char buf[32]) {
    const char *u[] = {"B", "KiB", "MiB", "GiB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 3) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(buf, 32, "%llu %s", (unsigned long long)bytes, u[0]);
    else
        snprintf(buf, 32, "%.1f %s", v, u[i]);
    return buf;
}

/* --------------------------------------------------------------------------
 *  MD5 (RFC 1321) — compact public-domain style implementation, for reporting
 *  ROM identity. Not security-sensitive.
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} md5_ctx;

static uint32_t rol(uint32_t x, int c) {
    return (x << c) | (x >> (32 - c));
}

static void md5_block(md5_ctx *m, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 |
               (uint32_t)p[i * 4 + 3] << 24;
    uint32_t A = m->a, B = m->b, C = m->c, D = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t F;
        int g;
        if (i < 16) {
            F = (B & C) | (~B & D);
            g = i;
        } else if (i < 32) {
            F = (D & B) | (~D & C);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            F = B ^ C ^ D;
            g = (3 * i + 5) & 15;
        } else {
            F = C ^ (B | ~D);
            g = (7 * i) & 15;
        }
        F = F + A + K[i] + M[g];
        A = D;
        D = C;
        C = B;
        B = B + rol(F, S[i]);
    }
    m->a += A;
    m->b += B;
    m->c += C;
    m->d += D;
}

void mm_md5_hex(const void *data, size_t len, char out[33]) {
    md5_ctx m = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, {0}, 0};
    const uint8_t *p = (const uint8_t *)data;
    m.len = len;
    while (len >= 64) {
        md5_block(&m, p);
        p += 64;
        len -= 64;
    }
    uint8_t tail[128];
    size_t t = 0;
    memcpy(tail, p, len);
    t = len;
    tail[t++] = 0x80;
    while ((t % 64) != 56)
        tail[t++] = 0;
    uint64_t bits = m.len * 8;
    for (int i = 0; i < 8; i++)
        tail[t++] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < t; i += 64)
        md5_block(&m, tail + i);
    uint32_t v[4] = {m.a, m.b, m.c, m.d};
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(v[i] >> (8 * j));
            out[(i * 4 + j) * 2] = hex[byte >> 4];
            out[(i * 4 + j) * 2 + 1] = hex[byte & 15];
        }
    out[32] = '\0';
}
